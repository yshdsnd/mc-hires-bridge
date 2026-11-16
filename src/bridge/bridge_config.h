#pragma once
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <atomic>

namespace bridgeConfig {

inline std::wstring getConfigFilePath() {
    wchar_t directory[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, directory))) {
        return {};
    }
    std::wstring path = directory;
    path += L"\\Sony\\Music Center\\config\\mc_hires_bridge.json";
    return path;
}

inline uint32_t parseDseeMultiplierFromJson(const std::string& data) {
    auto pos = data.find("\"dsee-multiplier\"");
    if (pos == std::string::npos) return 2;

    auto colon = data.find(':', pos);
    if (colon == std::string::npos) return 2;

    auto valStart = data.find_first_not_of(" \t\r\n\"", colon + 1);
    if (valStart == std::string::npos) return 2;

    if (data[valStart] == '4') return 4;
    return 2;
}

inline uint32_t loadDseeMultiplierFromDisk(const std::wstring& customPath = L"") {
    std::wstring path = customPath.empty() ? getConfigFilePath() : customPath;
    if (path.empty()) return 2;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 2;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart == 0 || size.QuadPart > 65536) {
        CloseHandle(f);
        return 2;
    }

    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(f, data.data(), DWORD(data.size()), &read, nullptr) || read != data.size()) {
        CloseHandle(f);
        return 2;
    }
    CloseHandle(f);

    return parseDseeMultiplierFromJson(data);
}

inline std::atomic<uint32_t>& activeDseeMultiplier() {
    static std::atomic<uint32_t> s_mult{0};
    return s_mult;
}

inline uint32_t getDseeMultiplier() {
    uint32_t current = activeDseeMultiplier().load();
    if (current == 2 || current == 4) return current;
    uint32_t loaded = loadDseeMultiplierFromDisk();
    activeDseeMultiplier().store(loaded);
    return loaded;
}

inline bool saveDseeMultiplier(uint32_t multiplier, const std::wstring& customPath = L"") {
    if (multiplier != 2 && multiplier != 4) multiplier = 2;
    activeDseeMultiplier().store(multiplier);

    std::wstring path = customPath.empty() ? getConfigFilePath() : customPath;
    if (path.empty()) return false;

    auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring dir = path.substr(0, slash);
        CreateDirectoryW(dir.c_str(), nullptr);
    }

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    std::string json = "{\n  \"dsee-multiplier\": " + std::to_string(multiplier) + "\n}\n";
    DWORD written = 0;
    bool ok = WriteFile(f, json.data(), DWORD(json.size()), &written, nullptr) && written == json.size();
    CloseHandle(f);
    return ok;
}

} // namespace bridgeConfig
