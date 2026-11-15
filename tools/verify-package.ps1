<#
.SYNOPSIS
    Verifies and packages the trial distribution ZIP for MC HiRes Bridge (Issue #32).

.DESCRIPTION
    Validates package contents against whitelist, detects forbidden diagnostic/experimental/test
    binaries (blacklist), verifies PE x86 binary formats and license files, and provides
    automated self-tests.

.PARAMETER ZipPath
    Path to an existing ZIP file to verify.

.PARAMETER Directory
    Path to an unzipped directory to verify.

.PARAMETER BuildDir
    Path to CMake build directory (default: 'build-Win32').

.PARAMETER OutputDir
    Destination directory for trial ZIP (default: 'dist').

.PARAMETER CreateZip
    Runs 'cmake --install' and compresses a verified trial ZIP.

.PARAMETER SelfTest
    Executes automated self-tests to prove detection of contaminated packages.

.PARAMETER Quiet
    Suppresses informational output.
#>

[CmdletBinding(DefaultParameterSetName = 'Default')]
param(
    [Parameter(ParameterSetName = 'VerifyZip')]
    [string]$ZipPath,

    [Parameter(ParameterSetName = 'VerifyDir')]
    [string]$Directory,

    [Parameter(ParameterSetName = 'Default')]
    [Parameter(ParameterSetName = 'Create')]
    [string]$BuildDir = 'build-Win32',

    [Parameter(ParameterSetName = 'Create')]
    [string]$OutputDir = 'dist',

    [Parameter(ParameterSetName = 'Create')]
    [switch]$CreateZip,

    [Parameter(ParameterSetName = 'SelfTest')]
    [switch]$SelfTest,

    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Write-Info([string]$message) {
    if (-not $Quiet) {
        Write-Host "[INFO] $message" -ForegroundColor Cyan
    }
}

function Write-Success([string]$message) {
    Write-Host "[PASS] $message" -ForegroundColor Green
}

function Write-Failure([string]$message) {
    Write-Host "[FAIL] $message" -ForegroundColor Red
}

function Find-CMake {
    $cmd = Get-Command 'cmake' -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $candidatePaths = @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\CMake\bin\cmake.exe"
    )

    foreach ($path in $candidatePaths) {
        if (Test-Path -LiteralPath $path) {
            return $path
        }
    }

    throw "CMake executable not found in PATH or standard Visual Studio installation directories."
}

function Get-PEInfo([string]$filePath) {
    if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
        return $null
    }
    try {
        $bytes = [IO.File]::ReadAllBytes($filePath)
        if ($bytes.Length -lt 64) { return $null }
        if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return $null } # 'MZ'
        $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
        if ($peOffset + 96 -gt $bytes.Length) { return $null }
        $peSig = [BitConverter]::ToUInt32($bytes, $peOffset)
        if ($peSig -ne 0x00004550) { return $null } # 'PE\0\0'
        $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
        $chars = [BitConverter]::ToUInt16($bytes, $peOffset + 22)
        $magic = [BitConverter]::ToUInt16($bytes, $peOffset + 24)
        $subsystem = [BitConverter]::ToUInt16($bytes, $peOffset + 92)
        return [PSCustomObject]@{
            IsValidPE  = $true
            Machine    = $machine
            IsX86      = ($machine -eq 0x014C) # IMAGE_FILE_MACHINE_I386
            IsX64      = ($machine -eq 0x8664) # IMAGE_FILE_MACHINE_AMD64
            IsDll      = (($chars -band 0x2000) -ne 0)
            Subsystem  = $subsystem
            IsGui      = ($subsystem -eq 2)
            IsConsole  = ($subsystem -eq 3)
            FileSize   = $bytes.Length
        }
    } catch {
        return $null
    }
}

function Get-RelativePathString([string]$basePath, [string]$targetPath) {
    $baseFull = $basePath.TrimEnd('\', '/') + '\'
    if ($targetPath.StartsWith($baseFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $targetPath.Substring($baseFull.Length).Replace('\', '/')
    }
    $baseUri = [System.Uri]($baseFull)
    $targetUri = [System.Uri]($targetPath)
    return [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($targetUri).ToString()).Replace('\', '/')
}

function Test-PackageContents([string]$targetDir) {
    $result = [PSCustomObject]@{
        IsValid   = $true
        Errors    = [System.Collections.Generic.List[string]]::new()
        Warnings  = [System.Collections.Generic.List[string]]::new()
        Files     = @()
    }

    if (-not (Test-Path -LiteralPath $targetDir -PathType Container)) {
        $result.IsValid = $false
        $result.Errors.Add("Target directory does not exist: $targetDir")
        return $result
    }

    $resolvedRoot = (Resolve-Path -LiteralPath $targetDir).Path
    $subDirs = Get-ChildItem -LiteralPath $resolvedRoot -Directory
    $rootFiles = Get-ChildItem -LiteralPath $resolvedRoot -File
    if ($rootFiles.Count -eq 0 -and $subDirs.Count -eq 1) {
        $resolvedRoot = $subDirs[0].FullName
    }

    $items = Get-ChildItem -LiteralPath $resolvedRoot -File -Recurse
    $relativeFiles = @($items | ForEach-Object {
        Get-RelativePathString $resolvedRoot $_.FullName
    })
    $result.Files = $relativeFiles

    # 1. Whitelist Verification
    $whitelist = @(
        'MC HiRes Bridge.exe',
        'flac24_override.dll',
        'README.md',
        'LICENSE',
        'licenses/MinHook.txt'
    )

    foreach ($expected in $whitelist) {
        if ($relativeFiles -notcontains $expected) {
            $result.IsValid = $false
            $result.Errors.Add("Missing required file: '$expected'")
        }
    }

    foreach ($file in $relativeFiles) {
        if ($whitelist -notcontains $file) {
            $result.IsValid = $false
            $result.Errors.Add("Unexpected file in package (not on whitelist): '$file'")
        }
    }

    # 2. Blacklist / Contaminant Detection
    foreach ($file in $relativeFiles) {
        $name = [IO.Path]::GetFileName($file)
        $ext = [IO.Path]::GetExtension($file).ToLowerInvariant()

        # Check for Diagnostic DLLs
        if ($ext -eq '.dll' -and ($name -like '*probe*' -or $name -like '*tap*' -or $name -like 'wasapi*')) {
            $result.IsValid = $false
            $result.Errors.Add("Contaminant detected [Diagnostic DLL]: '$file'")
        }

        # Check for Experimental DLLs
        if ($ext -eq '.dll' -and ($name -like '*bypass*' -or $name -like '*legacy*' -or $name -like '*experiment*')) {
            $result.IsValid = $false
            $result.Errors.Add("Contaminant detected [Experimental DLL]: '$file'")
        }

        # Check for Test / Diagnostic Executables
        if ($ext -eq '.exe' -and $name -ne 'MC HiRes Bridge.exe') {
            $result.IsValid = $false
            $result.Errors.Add("Contaminant detected [Test/Diagnostic Executable]: '$file'")
        }

        # Check for Build / Debug Artifacts
        if ($ext -in @('.pdb', '.ilk', '.exp', '.lib', '.obj', '.idb')) {
            $result.IsValid = $false
            $result.Errors.Add("Contaminant detected [Build/Debug Artifact]: '$file'")
        }

        # Check for Source or Raw Audio files
        if ($ext -in @('.cpp', '.h', '.inl', '.pcm', '.flac', '.wav', '.log')) {
            $result.IsValid = $false
            $result.Errors.Add("Contaminant detected [Source/Audio Artifact]: '$file'")
        }
    }

    # 3. Binary Format & Architecture Inspection
    $launcherPath = Join-Path $resolvedRoot 'MC HiRes Bridge.exe'
    if (Test-Path -LiteralPath $launcherPath -PathType Leaf) {
        $pe = Get-PEInfo $launcherPath
        if (-not $pe -or -not $pe.IsValidPE) {
            $result.IsValid = $false
            $result.Errors.Add("Launcher 'MC HiRes Bridge.exe' is not a valid PE executable.")
        } else {
            if (-not $pe.IsX86) {
                $result.IsValid = $false
                $result.Errors.Add("Launcher 'MC HiRes Bridge.exe' architecture mismatch: expected x86 (0x014C), found Machine 0x{0:X4}." -f $pe.Machine)
            }
            if ($pe.IsDll) {
                $result.IsValid = $false
                $result.Errors.Add("Launcher 'MC HiRes Bridge.exe' has DLL characteristic flag set.")
            }
            if (-not $pe.IsGui) {
                $result.Warnings.Add("Launcher 'MC HiRes Bridge.exe' subsystem is not GUI (subsystem=$($pe.Subsystem)).")
            }
        }
    }

    $dllPath = Join-Path $resolvedRoot 'flac24_override.dll'
    if (Test-Path -LiteralPath $dllPath -PathType Leaf) {
        $pe = Get-PEInfo $dllPath
        if (-not $pe -or -not $pe.IsValidPE) {
            $result.IsValid = $false
            $result.Errors.Add("DLL 'flac24_override.dll' is not a valid PE image.")
        } else {
            if (-not $pe.IsX86) {
                $result.IsValid = $false
                $result.Errors.Add("DLL 'flac24_override.dll' architecture mismatch: expected x86 (0x014C), found Machine 0x{0:X4}." -f $pe.Machine)
            }
            if (-not $pe.IsDll) {
                $result.IsValid = $false
                $result.Errors.Add("DLL 'flac24_override.dll' missing DLL characteristic flag.")
            }
        }
    }

    # 4. License & Documentation File Inspection
    $readmePath = Join-Path $resolvedRoot 'README.md'
    if (Test-Path -LiteralPath $readmePath -PathType Leaf) {
        $content = Get-Content -LiteralPath $readmePath -Raw -Encoding utf8
        if ([string]::IsNullOrWhiteSpace($content) -or $content -notmatch 'MC HiRes Bridge') {
            $result.IsValid = $false
            $result.Errors.Add("README.md appears invalid or empty.")
        }
    }

    $licensePath = Join-Path $resolvedRoot 'LICENSE'
    if (Test-Path -LiteralPath $licensePath -PathType Leaf) {
        $content = Get-Content -LiteralPath $licensePath -Raw -Encoding utf8
        if ([string]::IsNullOrWhiteSpace($content) -or $content -notmatch 'BSD 2-Clause') {
            $result.IsValid = $false
            $result.Errors.Add("LICENSE appears invalid or missing BSD 2-Clause reference.")
        }
    }

    $minhookPath = Join-Path $resolvedRoot 'licenses/MinHook.txt'
    if (Test-Path -LiteralPath $minhookPath -PathType Leaf) {
        $content = Get-Content -LiteralPath $minhookPath -Raw -Encoding utf8
        if ([string]::IsNullOrWhiteSpace($content) -or $content -notmatch 'MinHook') {
            $result.IsValid = $false
            $result.Errors.Add("licenses/MinHook.txt appears invalid or missing MinHook reference.")
        }
    }

    return $result
}

function Test-ZipArchive([string]$archivePath) {
    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        throw "ZIP archive not found: $archivePath"
    }

    Write-Info "Verifying ZIP archive: $archivePath"
    $tempDir = Join-Path ([IO.Path]::GetTempPath()) ("mc_zip_test_" + [IO.Path]::GetRandomFileName())
    try {
        New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
        Expand-Archive -LiteralPath $archivePath -DestinationPath $tempDir -Force
        $report = Test-PackageContents $tempDir
        return $report
    } finally {
        if (Test-Path -LiteralPath $tempDir) {
            Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Invoke-SelfTest {
    Write-Host "`n=== MC HiRes Bridge Package Verification Self-Test Suite (Issue #32) ===`n" -ForegroundColor Yellow

    $baseTemp = Join-Path ([IO.Path]::GetTempPath()) ("mc_selftest_" + [IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Path $baseTemp -Force | Out-Null

    # Helper to construct a synthetic valid package
    function Initialize-CleanStaging([string]$dir) {
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
        New-Item -ItemType Directory -Path (Join-Path $dir 'licenses') -Force | Out-Null

        # Create valid dummy PE x86 binaries (or copy from build-Win32 if present)
        $realExe = "build-Win32/Release/MC HiRes Bridge.exe"
        $realDll = "build-Win32/Release/flac24_override.dll"
        if ((Test-Path $realExe) -and (Test-Path $realDll)) {
            Copy-Item -LiteralPath $realExe -Destination (Join-Path $dir 'MC HiRes Bridge.exe')
            Copy-Item -LiteralPath $realDll -Destination (Join-Path $dir 'flac24_override.dll')
        } else {
            # Minimal synthetic PE x86
            $fakePe = New-Object byte[] 512
            $fakePe[0] = 0x4D; $fakePe[1] = 0x5A; # MZ
            [BitConverter]::GetBytes([int32]0x80).CopyTo($fakePe, 0x3C)
            [BitConverter]::GetBytes([uint32]0x00004550).CopyTo($fakePe, 0x80) # PE\0\0
            [BitConverter]::GetBytes([uint16]0x014C).CopyTo($fakePe, 0x84) # x86
            [BitConverter]::GetBytes([uint16]0x0102).CopyTo($fakePe, 0x9A) # characteristics
            [BitConverter]::GetBytes([uint16]2).CopyTo($fakePe, 0x80 + 92) # GUI subsystem
            [IO.File]::WriteAllBytes((Join-Path $dir 'MC HiRes Bridge.exe'), $fakePe)

            $fakeDll = New-Object byte[] 512
            [Array]::Copy($fakePe, $fakeDll, 512)
            [BitConverter]::GetBytes([uint16]0x2102).CopyTo($fakeDll, 0x9A) # DLL flag
            [IO.File]::WriteAllBytes((Join-Path $dir 'flac24_override.dll'), $fakeDll)
        }

        "MC HiRes Bridge README" | Set-Content -LiteralPath (Join-Path $dir 'README.md') -Encoding utf8
        "BSD 2-Clause License" | Set-Content -LiteralPath (Join-Path $dir 'LICENSE') -Encoding utf8
        "MinHook License" | Set-Content -LiteralPath (Join-Path $dir 'licenses/MinHook.txt') -Encoding utf8
    }

    $cases = @(
        @{ Id = 'ST-01'; Desc = 'Clean standard package'; Expected = $true; Setup = { param($d) } },
        @{ Id = 'ST-02'; Desc = 'Diagnostic DLL contamination (probe.dll)'; Expected = $false; Setup = { param($d) New-Item -ItemType File -Path (Join-Path $d 'probe.dll') } },
        @{ Id = 'ST-03'; Desc = 'Diagnostic DLL contamination (dshow_probe.dll)'; Expected = $false; Setup = { param($d) New-Item -ItemType File -Path (Join-Path $d 'dshow_probe.dll') } },
        @{ Id = 'ST-04'; Desc = 'Diagnostic DLL contamination (wasapi_pcm_probe.dll)'; Expected = $false; Setup = { param($d) New-Item -ItemType File -Path (Join-Path $d 'wasapi_pcm_probe.dll') } },
        @{ Id = 'ST-05'; Desc = 'Experimental DLL contamination (legacy_16bit_override.dll)'; Expected = $false; Setup = { param($d) New-Item -ItemType File -Path (Join-Path $d 'legacy_16bit_override.dll') } },
        @{ Id = 'ST-06'; Desc = 'Test executable contamination (eos_transition_tests.exe)'; Expected = $false; Setup = { param($d) New-Item -ItemType File -Path (Join-Path $d 'eos_transition_tests.exe') } },
        @{ Id = 'ST-07'; Desc = 'Observation tool contamination (track_regression_tool.exe)'; Expected = $false; Setup = { param($d) New-Item -ItemType File -Path (Join-Path $d 'track_regression_tool.exe') } },
        @{ Id = 'ST-08'; Desc = 'Missing primary DLL (flac24_override.dll)'; Expected = $false; Setup = { param($d) Remove-Item -LiteralPath (Join-Path $d 'flac24_override.dll') } },
        @{ Id = 'ST-09'; Desc = 'Missing MinHook license file'; Expected = $false; Setup = { param($d) Remove-Item -LiteralPath (Join-Path $d 'licenses/MinHook.txt') } },
        @{ Id = 'ST-10'; Desc = 'Debug symbol contamination (flac24_override.pdb)'; Expected = $false; Setup = { param($d) New-Item -ItemType File -Path (Join-Path $d 'flac24_override.pdb') } },
        @{ Id = 'ST-11'; Desc = 'Raw audio / private file contamination (dump.pcm)'; Expected = $false; Setup = { param($d) New-Item -ItemType File -Path (Join-Path $d 'dump.pcm') } }
    )

    $allPassed = $true
    $testResults = [System.Collections.Generic.List[PSCustomObject]]::new()

    foreach ($tc in $cases) {
        $stage = Join-Path $baseTemp $tc.Id
        Initialize-CleanStaging $stage
        & $tc.Setup $stage

        $rep = Test-PackageContents $stage
        $match = ($rep.IsValid -eq $tc.Expected)

        if (-not $match) {
            $allPassed = $false
        }

        $testResults.Add([PSCustomObject]@{
            CaseId    = $tc.Id
            Scenario  = $tc.Desc
            Expected  = if ($tc.Expected) { "PASS" } else { "FAIL (Detected)" }
            Actual    = if ($rep.IsValid) { "PASS" } else { "FAIL (Detected)" }
            Verdict   = if ($match) { "PASS" } else { "FAIL" }
            ErrorNote = if ($rep.Errors.Count -gt 0) { $rep.Errors[0] } else { "-" }
        })
    }

    # Clean up temp
    Remove-Item -LiteralPath $baseTemp -Recurse -Force -ErrorAction SilentlyContinue

    # Print Table
    Write-Host "| Case ID | Scenario | Expected | Actual | Verdict | First Error Detected |"
    Write-Host "|:---|:---|:---:|:---:|:---:|:---|"
    foreach ($r in $testResults) {
        Write-Host ("| {0} | {1} | {2} | {3} | **{4}** | {5} |" -f $r.CaseId, $r.Scenario, $r.Expected, $r.Actual, $r.Verdict, $r.ErrorNote)
    }

    Write-Host ""
    if ($allPassed) {
        Write-Success "All $($testResults.Count) Self-Test cases PASSED! Detection logic verified."
        return $true
    } else {
        Write-Failure "Some Self-Test cases FAILED!"
        return $false
    }
}

# --- Main Entry Point ---

if ($SelfTest) {
    $ok = Invoke-SelfTest
    if ($ok) { exit 0 } else { exit 1 }
}

if ($ZipPath) {
    $report = Test-ZipArchive $ZipPath
    if ($report.IsValid) {
        Write-Success "ZIP verification PASSED: '$ZipPath' is a clean, compliant distribution."
        exit 0
    } else {
        Write-Failure "ZIP verification FAILED: '$ZipPath'"
        foreach ($err in $report.Errors) {
            Write-Host "  - $err" -ForegroundColor Red
        }
        exit 1
    }
}

if ($Directory) {
    $report = Test-PackageContents $Directory
    if ($report.IsValid) {
        Write-Success "Directory verification PASSED: '$Directory' is clean and compliant."
        exit 0
    } else {
        Write-Failure "Directory verification FAILED: '$Directory'"
        foreach ($err in $report.Errors) {
            Write-Host "  - $err" -ForegroundColor Red
        }
        exit 1
    }
}

if ($CreateZip) {
    Write-Info "Installing binaries from build directory: $BuildDir"
    $stageDir = Join-Path 'package' 'MC HiRes Bridge'
    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force
    }

    $cmakeExe = Find-CMake
    $cmakeArgs = @('--install', $BuildDir, '--config', 'Release', '--prefix', $stageDir)
    & $cmakeExe @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "cmake --install failed with exit code $LASTEXITCODE"
    }

    Write-Info "Verifying staged package contents..."
    $report = Test-PackageContents $stageDir
    if (-not $report.IsValid) {
        Write-Failure "Staged package failed verification:"
        foreach ($err in $report.Errors) {
            Write-Host "  - $err" -ForegroundColor Red
        }
        exit 1
    }

    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    $zipTarget = Join-Path $OutputDir 'mc-hires-bridge-windows-x86.zip'
    if (Test-Path -LiteralPath $zipTarget) {
        Remove-Item -LiteralPath $zipTarget -Force
    }

    Write-Info "Compressing verified package to: $zipTarget"
    Compress-Archive -LiteralPath $stageDir -DestinationPath $zipTarget -CompressionLevel Optimal -Force

    $hash = (Get-FileHash -LiteralPath $zipTarget -Algorithm SHA256).Hash.ToLowerInvariant()
    $hashFile = "$zipTarget.sha256"
    "$hash  $([IO.Path]::GetFileName($zipTarget))" | Set-Content -LiteralPath $hashFile -Encoding ascii

    Write-Info "Validating created ZIP archive..."
    $zipReport = Test-ZipArchive $zipTarget
    if (-not $zipReport.IsValid) {
        Write-Failure "Created ZIP failed self-extraction verification!"
        foreach ($err in $zipReport.Errors) {
            Write-Host "  - $err" -ForegroundColor Red
        }
        exit 1
    }

    Write-Success "Trial ZIP created and verified successfully!"
    Write-Host "  ZIP:      $zipTarget" -ForegroundColor Green
    Write-Host "  SHA-256:  $hash" -ForegroundColor Green
    Write-Host "  Checksum: $hashFile" -ForegroundColor Green
    exit 0
}

# Default action if no flag specified: verify current build install
Write-Info "Running verification on default build staging..."
$stageDir = Join-Path 'package' 'MC HiRes Bridge'
$cmakeExe = Find-CMake
& $cmakeExe --install $BuildDir --config Release --prefix $stageDir
$report = Test-PackageContents $stageDir
if ($report.IsValid) {
    Write-Success "Default build installation is clean and verified."
    exit 0
} else {
    Write-Failure "Default build installation failed verification:"
    foreach ($err in $report.Errors) {
        Write-Host "  - $err" -ForegroundColor Red
    }
    exit 1
}
