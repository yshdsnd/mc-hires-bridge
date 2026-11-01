#pragma once
#include <windows.h>
#include <string>

namespace bridgeStartup {
inline std::wstring eventName(const wchar_t* state, DWORD pid) {
    return std::wstring(L"Local\\MC_HiRes_Bridge_") + state + L"_" + std::to_wstring(pid);
}
// Keep these handles alive while the DLL is running, including after the launcher exits.
struct Signal {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName(L"Ready", GetCurrentProcessId()).c_str());
    HANDLE failed = CreateEventW(nullptr, TRUE, FALSE, eventName(L"Failed", GetCurrentProcessId()).c_str());
    bool reported = false;
    void complete(bool success) {
        reported = true;
        if (auto event = success ? ready : failed) SetEvent(event);
    }
    ~Signal() {
        if (!reported && failed) SetEvent(failed);
        if (ready) CloseHandle(ready);
        if (failed) CloseHandle(failed);
    }
};
}
