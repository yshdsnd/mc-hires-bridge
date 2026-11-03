param(
    [Parameter(Mandatory=$true)][int]$TargetPid,
    [Parameter(Mandatory=$true)][ValidateSet('x86','x64')][string]$Architecture,
    [string]$BinPath
)
$ErrorActionPreference = 'Stop'
$targetProcess = Get-Process -Id $TargetPid
$logPath = Join-Path $env:TEMP "AudioFormatProbe-$TargetPid.tsv"
if (Test-Path -LiteralPath $logPath) { throw "Log already exists: $logPath. Check whether already attached. Restart the target for a fresh capture; preserve old logs first." }
if (!$BinPath) { $buildDirectory = if ($Architecture -eq 'x86') { 'build-Win32' } else { 'build-x64' }; $BinPath = Join-Path $PSScriptRoot "../$buildDirectory/Release" }
& "$binPath/inject.exe" $TargetPid "$binPath/probe.dll"
if ($LASTEXITCODE) { throw 'Injection failed' }
$ready = $false
for ($attempt=0; $attempt -lt 100; $attempt++) {
    if (Test-Path -LiteralPath $logPath) {
        $content = Get-Content -LiteralPath $logPath
        if ($content -match "`tPARTIAL_OR_FAILED`t|`tERROR\.") { throw "Hook setup incomplete. Inspect $logPath" }
        if ($content -match "`tREADY`t") { $ready = $true; break }
    }
    Start-Sleep -Milliseconds 100
}
if (!$ready) { throw "No READY within 10 seconds. Inspect $logPath before retrying." }
Write-Host "Ready: $($targetProcess.ProcessName) PID=$TargetPid. Reopen the audio output, then play the comparison file."
Write-Host "Log: $logPath"
Write-Host 'Exit the target app to remove the hooks. Copy the log before your next run.'
