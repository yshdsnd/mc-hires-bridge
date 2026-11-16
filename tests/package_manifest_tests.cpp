#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace packageRules {

inline bool isWhitelisted(const std::string& path) {
    static const char* kAllowed[] = {
        "MC HiRes Bridge.exe",
        "flac24_override.dll",
        "README.md",
        "LICENSE",
        "licenses/MinHook.txt"
    };
    for (const char* allowed : kAllowed) {
        if (path == allowed) return true;
    }
    return false;
}

inline bool isForbidden(const std::string& path, std::string* outCategory = nullptr) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(::tolower(c));
    });

    // Check Diagnostic DLL
    if (lower.find(".dll") != std::string::npos) {
        if (lower.find("probe") != std::string::npos ||
            lower.find("tap") != std::string::npos ||
            lower.find("wasapi") != std::string::npos) {
            if (outCategory) *outCategory = "Diagnostic DLL";
            return true;
        }
        if (lower.find("bypass") != std::string::npos ||
            lower.find("legacy") != std::string::npos ||
            lower.find("experiment") != std::string::npos) {
            if (outCategory) *outCategory = "Experimental DLL";
            return true;
        }
    }

    // Check Test or Diagnostic Executables
    if (lower.find(".exe") != std::string::npos) {
        if (path != "MC HiRes Bridge.exe") {
            if (outCategory) *outCategory = "Test/Diagnostic Executable";
            return true;
        }
    }

    // Check Build/Debug Artifacts
    static const char* kDebugExts[] = { ".pdb", ".ilk", ".exp", ".lib", ".obj" };
    for (const char* ext : kDebugExts) {
        if (lower.size() >= strlen(ext) &&
            lower.compare(lower.size() - strlen(ext), strlen(ext), ext) == 0) {
            if (outCategory) *outCategory = "Build/Debug Artifact";
            return true;
        }
    }

    // Check Source/Private Data Artifacts
    static const char* kSourceExts[] = { ".cpp", ".h", ".inl", ".pcm", ".flac", ".wav", ".log" };
    for (const char* ext : kSourceExts) {
        if (lower.size() >= strlen(ext) &&
            lower.compare(lower.size() - strlen(ext), strlen(ext), ext) == 0) {
            if (outCategory) *outCategory = "Source/Audio Artifact";
            return true;
        }
    }

    return false;
}

inline bool validateManifest(const std::vector<std::string>& files, std::string* outFirstError = nullptr) {
    static const char* kRequired[] = {
        "MC HiRes Bridge.exe",
        "flac24_override.dll",
        "README.md",
        "LICENSE",
        "licenses/MinHook.txt"
    };

    // Check all required exist
    for (const char* req : kRequired) {
        if (std::find(files.begin(), files.end(), req) == files.end()) {
            if (outFirstError) *outFirstError = std::string("Missing required file: ") + req;
            return false;
        }
    }

    // Check no unexpected or forbidden files exist
    for (const auto& file : files) {
        std::string cat;
        if (isForbidden(file, &cat)) {
            if (outFirstError) *outFirstError = "Forbidden item detected (" + cat + "): " + file;
            return false;
        }
        if (!isWhitelisted(file)) {
            if (outFirstError) *outFirstError = "Unexpected item (not whitelisted): " + file;
            return false;
        }
    }

    return true;
}

} // namespace packageRules

int main() {
    using namespace packageRules;

    const std::vector<std::string> cleanManifest = {
        "MC HiRes Bridge.exe",
        "flac24_override.dll",
        "README.md",
        "LICENSE",
        "licenses/MinHook.txt"
    };

    std::string err;
    if (!validateManifest(cleanManifest, &err)) {
        std::fprintf(stderr, "FAIL: Clean manifest failed validation: %s\n", err.c_str());
        return 1;
    }

    // Negative Test Cases:
    struct NegativeCase {
        const char* name;
        std::vector<std::string> files;
        const char* expectedKeyword;
    };

    NegativeCase negCases[] = {
        { "Contamination: probe.dll", { "MC HiRes Bridge.exe", "flac24_override.dll", "README.md", "LICENSE", "licenses/MinHook.txt", "probe.dll" }, "probe.dll" },
        { "Contamination: dshow_probe.dll", { "MC HiRes Bridge.exe", "flac24_override.dll", "README.md", "LICENSE", "licenses/MinHook.txt", "dshow_probe.dll" }, "dshow_probe.dll" },
        { "Contamination: wasapi_pcm_probe.dll", { "MC HiRes Bridge.exe", "flac24_override.dll", "README.md", "LICENSE", "licenses/MinHook.txt", "wasapi_pcm_probe.dll" }, "wasapi_pcm_probe.dll" },
        { "Contamination: legacy_16bit_override.dll", { "MC HiRes Bridge.exe", "flac24_override.dll", "README.md", "LICENSE", "licenses/MinHook.txt", "legacy_16bit_override.dll" }, "legacy_16bit_override.dll" },
        { "Contamination: eos_transition_tests.exe", { "MC HiRes Bridge.exe", "flac24_override.dll", "README.md", "LICENSE", "licenses/MinHook.txt", "eos_transition_tests.exe" }, "eos_transition_tests.exe" },
        { "Contamination: track_regression_tool.exe", { "MC HiRes Bridge.exe", "flac24_override.dll", "README.md", "LICENSE", "licenses/MinHook.txt", "track_regression_tool.exe" }, "track_regression_tool.exe" },
        { "Contamination: smoke.exe", { "MC HiRes Bridge.exe", "flac24_override.dll", "README.md", "LICENSE", "licenses/MinHook.txt", "smoke.exe" }, "smoke.exe" },
        { "Contamination: inject.exe", { "MC HiRes Bridge.exe", "flac24_override.dll", "README.md", "LICENSE", "licenses/MinHook.txt", "inject.exe" }, "inject.exe" },
        { "Contamination: debug pdb", { "MC HiRes Bridge.exe", "flac24_override.dll", "README.md", "LICENSE", "licenses/MinHook.txt", "flac24_override.pdb" }, ".pdb" },
        { "Contamination: raw audio pcm", { "MC HiRes Bridge.exe", "flac24_override.dll", "README.md", "LICENSE", "licenses/MinHook.txt", "capture.pcm" }, ".pcm" },
        { "Missing: flac24_override.dll", { "MC HiRes Bridge.exe", "README.md", "LICENSE", "licenses/MinHook.txt" }, "flac24_override.dll" },
        { "Missing: MinHook.txt", { "MC HiRes Bridge.exe", "flac24_override.dll", "README.md", "LICENSE" }, "MinHook.txt" },
        { "Missing: LICENSE", { "MC HiRes Bridge.exe", "flac24_override.dll", "README.md", "licenses/MinHook.txt" }, "LICENSE" }
    };

    for (size_t i = 0; i < sizeof(negCases) / sizeof(negCases[0]); ++i) {
        std::string negErr;
        if (validateManifest(negCases[i].files, &negErr)) {
            std::fprintf(stderr, "FAIL: %s was expected to fail validation but passed!\n", negCases[i].name);
            return 2;
        }
        if (negErr.find(negCases[i].expectedKeyword) == std::string::npos) {
            std::fprintf(stderr, "FAIL: %s error message '%s' did not contain expected keyword '%s'\n",
                         negCases[i].name, negErr.c_str(), negCases[i].expectedKeyword);
            return 3;
        }
    }

    std::puts("All package manifest whitelist and negative contamination detection tests passed.");
    return 0;
}
