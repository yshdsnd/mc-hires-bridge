#pragma once
// Diagnostic logging with monotonic clock, thread ID, graph identifier,
// and resolved caller module+RVA for Issue #25 manual operation investigation.
#include <windows.h>
#include <cstdio>
#include <cstdint>

namespace eosDiag {

struct DiagClock {
    static inline uint64_t startQpc = 0;
    static inline uint64_t freq = 0;

    static void init() {
        if (freq == 0) {
            LARGE_INTEGER f, c;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&c);
            freq = f.QuadPart ? f.QuadPart : 1;
            startQpc = c.QuadPart;
        }
    }

    static uint64_t monotonicUs() {
        init();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return ((c.QuadPart - startQpc) * 1000000ULL) / freq;
    }

    static uint64_t monotonicMs() {
        return monotonicUs() / 1000ULL;
    }
};

static inline void formatCaller(void* ptr, char* buf, size_t maxLen) {
    if (!ptr) {
        sprintf_s(buf, maxLen, "null");
        return;
    }
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(ptr), &m) && m) {
        wchar_t wpath[MAX_PATH]{};
        GetModuleFileNameW(m, wpath, MAX_PATH);
        const wchar_t* pName = wcsrchr(wpath, L'\\');
        const wchar_t* base = pName ? pName + 1 : wpath;
        char name[64]{};
        WideCharToMultiByte(CP_UTF8, 0, base, -1, name, sizeof(name), nullptr, nullptr);
        auto offset = reinterpret_cast<ULONG_PTR>(ptr) - reinterpret_cast<ULONG_PTR>(m);
        sprintf_s(buf, maxLen, "%s+0x%lX", name, static_cast<unsigned long>(offset));
    } else {
        sprintf_s(buf, maxLen, "0x%p", ptr);
    }
}

} // namespace eosDiag
