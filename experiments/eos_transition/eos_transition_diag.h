#pragma once
// Diagnostic logging with monotonic clock, thread ID, graph identifier,
// resolved caller module+RVA, lock acquisition tracking, and application
// command dispatch tracing for Issue #25 / Issue #27.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>
#include <string>

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

// Module range tracking for zero-overhead caller filtering
struct ModuleRange {
    ULONG_PTR start = 0;
    ULONG_PTR end = 0;
    char name[64]{};
};

class ModuleTracker {
public:
    static constexpr size_t MAX_MODULES = 16;
    ModuleRange modules[MAX_MODULES]{};
    size_t count = 0;
    SRWLOCK lock = SRWLOCK_INIT;

    void addModule(HMODULE hMod, const char* overrideName = nullptr) {
        if (!hMod) return;
        AcquireSRWLockExclusive(&lock);
        auto base = reinterpret_cast<ULONG_PTR>(hMod);
        for (size_t i = 0; i < count; ++i) {
            if (modules[i].start == base) {
                ReleaseSRWLockExclusive(&lock);
                return;
            }
        }
        if (count < MAX_MODULES) {
            auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
            if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE) {
                auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                if (nt && nt->Signature == IMAGE_NT_SIGNATURE) {
                    modules[count].start = base;
                    modules[count].end = base + nt->OptionalHeader.SizeOfImage;
                    if (overrideName) {
                        strncpy_s(modules[count].name, overrideName, sizeof(modules[count].name) - 1);
                    } else {
                        wchar_t wpath[MAX_PATH]{};
                        GetModuleFileNameW(hMod, wpath, MAX_PATH);
                        const wchar_t* pName = wcsrchr(wpath, L'\\');
                        const wchar_t* baseName = pName ? pName + 1 : wpath;
                        WideCharToMultiByte(CP_UTF8, 0, baseName, -1, modules[count].name, sizeof(modules[count].name), nullptr, nullptr);
                    }
                    ++count;
                }
            }
        }
        ReleaseSRWLockExclusive(&lock);
    }

    const char* findModule(void* ptr) {
        if (!ptr) return nullptr;
        auto addr = reinterpret_cast<ULONG_PTR>(ptr);
        AcquireSRWLockShared(&lock);
        for (size_t i = 0; i < count; ++i) {
            if (addr >= modules[i].start && addr < modules[i].end) {
                const char* n = modules[i].name;
                ReleaseSRWLockShared(&lock);
                return n;
            }
        }
        ReleaseSRWLockShared(&lock);
        return nullptr;
    }

    void refresh() {
        const wchar_t* targetNames[] = {
            L"OpcDSPlay.dll",
            L"OpcFlac.dll",
            L"SeamlessFilter.ax",
            L"OmgEqxlFilter.ax",
            L"MusicCenter.exe"
        };
        for (const auto* name : targetNames) {
            HMODULE h = GetModuleHandleW(name);
            if (h) {
                addModule(h);
            }
        }
    }
};

static inline ModuleTracker& getModuleTracker() {
    static ModuleTracker tracker;
    return tracker;
}

// Lock state and contention tracker (Issue #27)
struct LockState {
    DWORD ownerTid = 0;
    uint64_t acquiredMonoMs = 0;
    void* lastCaller = nullptr;
    uint32_t recursionCount = 0;
};

struct LockContentionRecord {
    void* lockAddr = nullptr;
    void* caller = nullptr;
    DWORD tid = 0;
    uint64_t tryMonoMs = 0;
    uint64_t acquiredMonoMs = 0;
    uint64_t releasedMonoMs = 0;
    uint64_t waitMs = 0;
    uint64_t heldMs = 0;
};

class LockTracker {
public:
    SRWLOCK lock = SRWLOCK_INIT;
    std::map<void*, LockState> activeLocks;
    std::map<DWORD, uint64_t> pendingTries; // tid -> tryMonoMs
    std::vector<LockContentionRecord> history;

    void onTry(void* cs, void* caller, DWORD tid, uint64_t monoMs) {
        AcquireSRWLockExclusive(&lock);
        pendingTries[tid] = monoMs;
        ReleaseSRWLockExclusive(&lock);
    }

    uint64_t onAcquired(void* cs, void* caller, DWORD tid, uint64_t monoMs) {
        uint64_t waitMs = 0;
        AcquireSRWLockExclusive(&lock);
        auto itTry = pendingTries.find(tid);
        if (itTry != pendingTries.end()) {
            if (monoMs >= itTry->second) {
                waitMs = monoMs - itTry->second;
            }
            pendingTries.erase(itTry);
        }
        auto& state = activeLocks[cs];
        if (state.ownerTid == tid) {
            ++state.recursionCount;
        } else {
            state.ownerTid = tid;
            state.acquiredMonoMs = monoMs;
            state.lastCaller = caller;
            state.recursionCount = 1;
        }
        ReleaseSRWLockExclusive(&lock);
        return waitMs;
    }

    uint64_t onReleased(void* cs, void* caller, DWORD tid, uint64_t monoMs) {
        uint64_t heldMs = 0;
        AcquireSRWLockExclusive(&lock);
        auto it = activeLocks.find(cs);
        if (it != activeLocks.end() && it->second.ownerTid == tid) {
            if (it->second.recursionCount > 1) {
                --it->second.recursionCount;
            } else {
                if (monoMs >= it->second.acquiredMonoMs && it->second.acquiredMonoMs > 0) {
                    heldMs = monoMs - it->second.acquiredMonoMs;
                }
                activeLocks.erase(it);
            }
        }
        ReleaseSRWLockExclusive(&lock);
        return heldMs;
    }

    void reset() {
        AcquireSRWLockExclusive(&lock);
        activeLocks.clear();
        pendingTries.clear();
        history.clear();
        ReleaseSRWLockExclusive(&lock);
    }
};

static inline LockTracker& getLockTracker() {
    static LockTracker tracker;
    return tracker;
}

// In-memory trace record for synthetic tests and diagnostics
struct TraceItem {
    uint64_t monoMs = 0;
    DWORD tid = 0;
    std::string eventName;
    void* caller = nullptr;
    uint64_t p1 = 0;
    uint64_t p2 = 0;
    HRESULT hr = S_OK;
};

class TraceCollector {
public:
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<TraceItem> items;

    void add(const char* event, void* caller, uint64_t p1 = 0, uint64_t p2 = 0, HRESULT hr = S_OK) {
        TraceItem item;
        item.monoMs = DiagClock::monotonicMs();
        item.tid = GetCurrentThreadId();
        item.eventName = event ? event : "";
        item.caller = caller;
        item.p1 = p1;
        item.p2 = p2;
        item.hr = hr;
        AcquireSRWLockExclusive(&lock);
        items.push_back(item);
        ReleaseSRWLockExclusive(&lock);
    }

    void clear() {
        AcquireSRWLockExclusive(&lock);
        items.clear();
        ReleaseSRWLockExclusive(&lock);
    }

    size_t countEvent(const char* event) {
        size_t c = 0;
        AcquireSRWLockShared(&lock);
        for (const auto& it : items) {
            if (it.eventName == event) ++c;
        }
        ReleaseSRWLockShared(&lock);
        return c;
    }
};

static inline TraceCollector& getTraceCollector() {
    static TraceCollector collector;
    return collector;
}

} // namespace eosDiag
