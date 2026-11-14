#pragma once

#include <windows.h>
#include <dshow.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <atomic>
#include <string>
#include <vector>

namespace flowObserver {

struct FlowEvent {
    uint64_t seq = 0;
    int64_t timeUs = 0;
    uint64_t tickMs = 0;
    uint32_t tid = 0;
    void* graphId = nullptr;
    void* objPtr = nullptr;
    char event[48]{};
    char caller[128]{};
    char details[512]{};
};

class FlowLogger {
public:
    FlowLogger() {
        QueryPerformanceFrequency(&freq_);
        QueryPerformanceCounter(&startCounter_);
    }

    ~FlowLogger() {
        close();
    }

    bool initFile(const wchar_t* path) {
        close();
        file_ = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            return false;
        }
        writeHeader();
        return true;
    }

    void initMemory() {
        close();
        memoryMode_ = true;
    }

    void close() {
        AcquireSRWLockExclusive(&lock_);
        if (file_ != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(file_);
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
        memoryMode_ = false;
        ReleaseSRWLockExclusive(&lock_);
    }

    void log(const char* eventName, void* obj, void* graph, void* callerAddr, const char* format, ...) {
        char callerStr[128]{};
        formatCaller(callerAddr, callerStr, sizeof(callerStr));

        char detailsStr[512]{};
        if (format && format[0]) {
            va_list args;
            va_start(args, format);
            vsnprintf(detailsStr, sizeof(detailsStr), format, args);
            va_end(args);
        }

        recordEvent(eventName, obj, graph, callerStr, detailsStr);
    }

    void logWithCallerStr(const char* eventName, void* obj, void* graph, const char* callerStr, const char* format, ...) {
        char detailsStr[512]{};
        if (format && format[0]) {
            va_list args;
            va_start(args, format);
            vsnprintf(detailsStr, sizeof(detailsStr), format, args);
            va_end(args);
        }

        recordEvent(eventName, obj, graph, callerStr ? callerStr : "unknown", detailsStr);
    }

    static void formatCaller(void* returnAddr, char* out, size_t outSize) {
        if (!out || outSize == 0) return;
        if (!returnAddr) {
            snprintf(out, outSize, "unknown");
            return;
        }

        HMODULE hMod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(returnAddr), &hMod) && hMod) {
            wchar_t modPath[MAX_PATH]{};
            GetModuleFileNameW(hMod, modPath, MAX_PATH);
            const wchar_t* pSlash = wcsrchr(modPath, L'\\');
            const wchar_t* pName = pSlash ? (pSlash + 1) : modPath;
            auto rva = reinterpret_cast<ULONG_PTR>(returnAddr) - reinterpret_cast<ULONG_PTR>(hMod);
            snprintf(out, outSize, "%ls+0x%08X", pName, static_cast<unsigned int>(rva));
        } else {
            snprintf(out, outSize, "0x%p", returnAddr);
        }
    }

    static void formatBacktrace(void** frames, size_t count, char* out, size_t outSize) {
        if (!out || outSize == 0) return;
        out[0] = '\0';
        size_t offset = 0;
        for (size_t i = 0; i < count && frames && frames[i]; ++i) {
            char frameBuf[96]{};
            formatCaller(frames[i], frameBuf, sizeof(frameBuf));
            int n = snprintf(out + offset, (outSize > offset) ? (outSize - offset) : 0,
                             "%s%s", (i > 0 ? " -> " : ""), frameBuf);
            if (n < 0 || offset + static_cast<size_t>(n) >= outSize) break;
            offset += static_cast<size_t>(n);
        }
    }

    static void formatSeekingFlags(DWORD flags, char* out, size_t outSize) {
        if (!out || outSize == 0) return;
        DWORD posBits = flags & AM_SEEKING_PositioningBitsMask;
        const char* posStr = "NoPositioning";
        if (posBits == AM_SEEKING_AbsolutePositioning) posStr = "Absolute";
        else if (posBits == AM_SEEKING_RelativePositioning) posStr = "Relative";
        else if (posBits == AM_SEEKING_IncrementalPositioning) posStr = "Incremental";

        char extra[128] = "";
        if (flags & AM_SEEKING_SeekToKeyFrame) strcat_s(extra, "|KeyFrame");
        if (flags & AM_SEEKING_ReturnTime)     strcat_s(extra, "|ReturnTime");
        if (flags & AM_SEEKING_Segment)        strcat_s(extra, "|Segment");
        if (flags & AM_SEEKING_NoFlush)        strcat_s(extra, "|NoFlush");

        snprintf(out, outSize, "0x%02X(%s%s)", flags, posStr, extra);
    }

    std::vector<FlowEvent> getMemoryEvents() const {
        AcquireSRWLockShared(const_cast<SRWLOCK*>(&lock_));
        auto copy = events_;
        ReleaseSRWLockShared(const_cast<SRWLOCK*>(&lock_));
        return copy;
    }

    size_t eventCount() const {
        return eventSeq_.load(std::memory_order_relaxed);
    }

private:
    void writeHeader() {
        const char header[] = "seq\ttime_us\ttick_ms\ttid\tgraph_id\tobj_ptr\tevent\tcaller\tdetails\r\n";
        DWORD written = 0;
        WriteFile(file_, header, static_cast<DWORD>(strlen(header)), &written, nullptr);
    }

    void recordEvent(const char* eventName, void* obj, void* graph, const char* callerStr, const char* detailsStr) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        int64_t diff = now.QuadPart - startCounter_.QuadPart;
        int64_t timeUs = (freq_.QuadPart > 0) ? ((diff * 1000000LL) / freq_.QuadPart) : 0;
        uint64_t tickMs = GetTickCount64();
        uint32_t tid = GetCurrentThreadId();
        uint64_t seq = ++eventSeq_;

        AcquireSRWLockExclusive(&lock_);
        if (memoryMode_) {
            FlowEvent ev{};
            ev.seq = seq;
            ev.timeUs = timeUs;
            ev.tickMs = tickMs;
            ev.tid = tid;
            ev.graphId = graph;
            ev.objPtr = obj;
            strncpy_s(ev.event, eventName ? eventName : "UNKNOWN", sizeof(ev.event) - 1);
            strncpy_s(ev.caller, callerStr ? callerStr : "unknown", sizeof(ev.caller) - 1);
            strncpy_s(ev.details, detailsStr ? detailsStr : "", sizeof(ev.details) - 1);
            events_.push_back(ev);
        }

        if (file_ != INVALID_HANDLE_VALUE) {
            char line[1024];
            int n = snprintf(line, sizeof(line),
                             "%llu\t%lld\t%llu\t%u\t%p\t%p\t%s\t%s\t%s\r\n",
                             seq, timeUs, tickMs, tid, graph, obj,
                             eventName ? eventName : "UNKNOWN",
                             callerStr ? callerStr : "unknown",
                             detailsStr ? detailsStr : "");
            if (n > 0) {
                DWORD written = 0;
                WriteFile(file_, line, static_cast<DWORD>(n), &written, nullptr);
            }
        }
        ReleaseSRWLockExclusive(&lock_);
    }

    HANDLE file_ = INVALID_HANDLE_VALUE;
    bool memoryMode_ = false;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    LARGE_INTEGER freq_{};
    LARGE_INTEGER startCounter_{};
    std::atomic<uint64_t> eventSeq_{0};
    std::vector<FlowEvent> events_;
};

} // namespace flowObserver
