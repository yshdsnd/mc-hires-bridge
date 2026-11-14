// Pure observation probe for Issue #22:
// Traces exact event ordering, monotonic timestamps, threads, graphs, callers (module + RVA),
// positions, flags, and HRESULTs across format-changing transitions without altering playback control.
#include <windows.h>
#include <dshow.h>
#include <dsound.h>
#include <mmreg.h>
#include <intrin.h>
#include <cstdio>
#include <cstdarg>
#include <atomic>
#include "MinHook.h"
#include "flow_observer.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "dxguid.lib")

namespace {

flowObserver::FlowLogger g_logger;
thread_local bool g_inHook = false;

struct HookScope {
    HookScope() { g_inHook = true; }
    ~HookScope() { g_inHook = false; }
};

static void* graphFromObject(IUnknown* unk) {
    if (!unk) return nullptr;
    IFilterGraph* graph = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&graph))) && graph) {
        graph->Release();
        return graph;
    }
    IBaseFilter* filter = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&filter))) && filter) {
        FILTER_INFO fi{};
        if (SUCCEEDED(filter->QueryFilterInfo(&fi)) && fi.pGraph) {
            fi.pGraph->Release();
            filter->Release();
            return fi.pGraph;
        }
        filter->Release();
    }
    IPin* pin = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&pin))) && pin) {
        PIN_INFO pi{};
        if (SUCCEEDED(pin->QueryPinInfo(&pi)) && pi.pFilter) {
            FILTER_INFO fi{};
            void* resultGraph = nullptr;
            if (SUCCEEDED(pi.pFilter->QueryFilterInfo(&fi)) && fi.pGraph) {
                resultGraph = fi.pGraph;
                fi.pGraph->Release();
            }
            pi.pFilter->Release();
            pin->Release();
            return resultGraph;
        }
        pin->Release();
    }
    return nullptr;
}

static void filterInfo(IBaseFilter* filter, char* out, size_t outSize) {
    if (!filter || !out || outSize == 0) return;
    FILTER_INFO fi{};
    CLSID clsid{};
    filter->GetClassID(&clsid);
    wchar_t clsidStr[40]{};
    StringFromGUID2(clsid, clsidStr, 40);
    if (SUCCEEDED(filter->QueryFilterInfo(&fi))) {
        snprintf(out, outSize, "name=%ls clsid=%ls", fi.achName, clsidStr);
        if (fi.pGraph) fi.pGraph->Release();
    } else {
        snprintf(out, outSize, "clsid=%ls", clsidStr);
    }
}

static void pinInfo(IPin* pin, char* out, size_t outSize) {
    if (!pin || !out || outSize == 0) return;
    PIN_INFO pi{};
    if (SUCCEEDED(pin->QueryPinInfo(&pi))) {
        char filterStr[128]{};
        filterInfo(pi.pFilter, filterStr, sizeof(filterStr));
        snprintf(out, outSize, "pin_name=%ls dir=%s %s",
                 pi.achName, (pi.dir == PINDIR_INPUT ? "input" : "output"), filterStr);
        if (pi.pFilter) pi.pFilter->Release();
    } else {
        snprintf(out, outSize, "pin=%p", pin);
    }
}

static bool hookOnce(void* target, void* detour, void** original) {
    if (!target) return false;
    MH_STATUS s = MH_CreateHook(target, detour, original);
    return (s == MH_OK || s == MH_ERROR_ALREADY_CREATED);
}

// ---------------------------------------------------------------------------
// 1. IMediaEventSink::Notify (DirectShow events, e.g. 3841 / 0x0F01)
// ---------------------------------------------------------------------------
using NotifyFn = HRESULT(STDMETHODCALLTYPE*)(IMediaEventSink*, long, LONG_PTR, LONG_PTR);
static NotifyFn g_origNotify = nullptr;

static HRESULT STDMETHODCALLTYPE notifyHook(IMediaEventSink* self, long code, LONG_PTR p1, LONG_PTR p2) {
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        void* caller = _ReturnAddress();

        if (code == 3841) {
            void* frames[8]{};
            USHORT count = CaptureStackBackTrace(1, 8, frames, nullptr);
            char bt[384]{};
            flowObserver::FlowLogger::formatBacktrace(frames, count, bt, sizeof(bt));
            g_logger.log("DS_EVENT_3841", self, graph, caller,
                         "code=3841(0x0F01) p1=0x%p p2=0x%p backtrace=%s",
                         reinterpret_cast<void*>(p1), reinterpret_cast<void*>(p2), bt);
        } else if (code == 0x01 /* EC_COMPLETE */ || code == 0x22 /* EC_USERABORT */ || code == 0x23 /* EC_ERRORABORT */) {
            g_logger.log("DS_EVENT_MAJOR", self, graph, caller,
                         "code=0x%04X p1=0x%p p2=0x%p", code, reinterpret_cast<void*>(p1), reinterpret_cast<void*>(p2));
        } else {
            g_logger.log("DS_EVENT", self, graph, caller,
                         "code=%ld(0x%04X) p1=0x%p p2=0x%p", code, code, reinterpret_cast<void*>(p1), reinterpret_cast<void*>(p2));
        }
    }
    return g_origNotify(self, code, p1, p2);
}

// ---------------------------------------------------------------------------
// 2. IMediaSeeking Hooks (SetPositions, GetStopPosition, GetCurrentPosition)
// ---------------------------------------------------------------------------
using SetPositionsFn = HRESULT(STDMETHODCALLTYPE*)(IMediaSeeking*, LONGLONG*, DWORD, LONGLONG*, DWORD);
using GetPositionFn = HRESULT(STDMETHODCALLTYPE*)(IMediaSeeking*, LONGLONG*);
using GetPositionsFn = HRESULT(STDMETHODCALLTYPE*)(IMediaSeeking*, LONGLONG*, LONGLONG*);

static SetPositionsFn g_origSetPositions = nullptr;
static GetPositionFn g_origGetStopPosition = nullptr;
static GetPositionFn g_origGetCurrentPosition = nullptr;
static GetPositionsFn g_origGetPositions = nullptr;

static HRESULT STDMETHODCALLTYPE setPositionsHook(IMediaSeeking* self, LONGLONG* pCurrent, DWORD dwCurrentFlags,
                                                  LONGLONG* pStop, DWORD dwStopFlags) {
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        void* caller = _ReturnAddress();

        char currFlagStr[64]{}, stopFlagStr[64]{};
        flowObserver::FlowLogger::formatSeekingFlags(dwCurrentFlags, currFlagStr, sizeof(currFlagStr));
        flowObserver::FlowLogger::formatSeekingFlags(dwStopFlags, stopFlagStr, sizeof(stopFlagStr));

        void* frames[6]{};
        USHORT count = CaptureStackBackTrace(1, 6, frames, nullptr);
        char bt[384]{};
        flowObserver::FlowLogger::formatBacktrace(frames, count, bt, sizeof(bt));

        LONGLONG cVal = pCurrent ? *pCurrent : -1;
        LONGLONG sVal = pStop ? *pStop : -1;
        double cSec = (pCurrent && cVal >= 0) ? (double(cVal) / 10000000.0) : -1.0;
        double sSec = (pStop && sVal >= 0) ? (double(sVal) / 10000000.0) : -1.0;

        g_logger.log("SEEK_SETPOSITIONS_ENTER", self, graph, caller,
                     "currFlags=%s currPos=%lld(%.4fs) stopFlags=%s stopPos=%lld(%.4fs) backtrace=%s",
                     currFlagStr, cVal, cSec, stopFlagStr, sVal, sSec, bt);
    }

    HRESULT hr = g_origSetPositions(self, pCurrent, dwCurrentFlags, pStop, dwStopFlags);

    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        g_logger.log("SEEK_SETPOSITIONS_LEAVE", self, graph, _ReturnAddress(), "hr=0x%08X", hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE getStopPositionHook(IMediaSeeking* self, LONGLONG* pStop) {
    HRESULT hr = g_origGetStopPosition(self, pStop);
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        LONGLONG val = pStop ? *pStop : -1;
        double sec = (pStop && val >= 0) ? (double(val) / 10000000.0) : -1.0;
        g_logger.log("SEEK_GETSTOP", self, graph, _ReturnAddress(),
                     "stopPos=%lld(%.4fs) hr=0x%08X", val, sec, hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE getCurrentPositionHook(IMediaSeeking* self, LONGLONG* pCurrent) {
    HRESULT hr = g_origGetCurrentPosition(self, pCurrent);
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        LONGLONG val = pCurrent ? *pCurrent : -1;
        double sec = (pCurrent && val >= 0) ? (double(val) / 10000000.0) : -1.0;
        g_logger.log("SEEK_GETCURR", self, graph, _ReturnAddress(),
                     "currPos=%lld(%.4fs) hr=0x%08X", val, sec, hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE getPositionsHook(IMediaSeeking* self, LONGLONG* pCurrent, LONGLONG* pStop) {
    HRESULT hr = g_origGetPositions(self, pCurrent, pStop);
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        LONGLONG cVal = pCurrent ? *pCurrent : -1;
        LONGLONG sVal = pStop ? *pStop : -1;
        g_logger.log("SEEK_GETPOSITIONS", self, graph, _ReturnAddress(),
                     "currPos=%lld stopPos=%lld hr=0x%08X", cVal, sVal, hr);
    }
    return hr;
}

// ---------------------------------------------------------------------------
// 3. IMediaControl & IMediaFilter Hooks (Stop, Pause, Run)
// ---------------------------------------------------------------------------
using ControlVoidFn = HRESULT(STDMETHODCALLTYPE*)(IMediaControl*);
using FilterVoidFn = HRESULT(STDMETHODCALLTYPE*)(IMediaFilter*);
using FilterRunFn = HRESULT(STDMETHODCALLTYPE*)(IMediaFilter*, REFERENCE_TIME);

static ControlVoidFn g_origControlStop = nullptr;
static ControlVoidFn g_origControlPause = nullptr;
static ControlVoidFn g_origControlRun = nullptr;
static FilterVoidFn g_origFilterStop = nullptr;
static FilterVoidFn g_origFilterPause = nullptr;
static FilterRunFn g_origFilterRun = nullptr;

static HRESULT STDMETHODCALLTYPE controlStopHook(IMediaControl* self) {
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        void* caller = _ReturnAddress();
        void* frames[6]{};
        USHORT count = CaptureStackBackTrace(1, 6, frames, nullptr);
        char bt[384]{};
        flowObserver::FlowLogger::formatBacktrace(frames, count, bt, sizeof(bt));

        g_logger.log("CONTROL_STOP_ENTER", self, graph, caller, "backtrace=%s", bt);
    }
    HRESULT hr = g_origControlStop(self);
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        g_logger.log("CONTROL_STOP_LEAVE", self, graph, _ReturnAddress(), "hr=0x%08X", hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE controlPauseHook(IMediaControl* self) {
    if (!g_inHook) {
        HookScope scope;
        g_logger.log("CONTROL_PAUSE", self, graphFromObject(self), _ReturnAddress(), "");
    }
    return g_origControlPause(self);
}

static HRESULT STDMETHODCALLTYPE controlRunHook(IMediaControl* self) {
    if (!g_inHook) {
        HookScope scope;
        g_logger.log("CONTROL_RUN", self, graphFromObject(self), _ReturnAddress(), "");
    }
    return g_origControlRun(self);
}

static HRESULT STDMETHODCALLTYPE filterStopHook(IMediaFilter* self) {
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        void* frames[6]{};
        USHORT count = CaptureStackBackTrace(1, 6, frames, nullptr);
        char bt[384]{};
        flowObserver::FlowLogger::formatBacktrace(frames, count, bt, sizeof(bt));
        g_logger.log("FILTER_STOP_ENTER", self, graph, _ReturnAddress(), "backtrace=%s", bt);
    }
    HRESULT hr = g_origFilterStop(self);
    if (!g_inHook) {
        HookScope scope;
        g_logger.log("FILTER_STOP_LEAVE", self, graphFromObject(self), _ReturnAddress(), "hr=0x%08X", hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE filterPauseHook(IMediaFilter* self) {
    if (!g_inHook) {
        HookScope scope;
        g_logger.log("FILTER_PAUSE", self, graphFromObject(self), _ReturnAddress(), "");
    }
    return g_origFilterPause(self);
}

static HRESULT STDMETHODCALLTYPE filterRunHook(IMediaFilter* self, REFERENCE_TIME tStart) {
    if (!g_inHook) {
        HookScope scope;
        g_logger.log("FILTER_RUN", self, graphFromObject(self), _ReturnAddress(), "tStart=%lld", tStart);
    }
    return g_origFilterRun(self, tStart);
}

// ---------------------------------------------------------------------------
// 4. IFilterGraph / IGraphBuilder Topology Hooks (RemoveFilter, Disconnect, Reconnect)
// ---------------------------------------------------------------------------
using RemoveFilterFn = HRESULT(STDMETHODCALLTYPE*)(IFilterGraph*, IBaseFilter*);
using DisconnectPinFn = HRESULT(STDMETHODCALLTYPE*)(IGraphBuilder*, IPin*);
using ReconnectPinFn = HRESULT(STDMETHODCALLTYPE*)(IGraphBuilder*, IPin*);

static RemoveFilterFn g_origRemoveFilter = nullptr;
static DisconnectPinFn g_origDisconnectPin = nullptr;
static ReconnectPinFn g_origReconnectPin = nullptr;

static HRESULT STDMETHODCALLTYPE removeFilterHook(IFilterGraph* self, IBaseFilter* pFilter) {
    if (!g_inHook) {
        HookScope scope;
        char fStr[128]{};
        filterInfo(pFilter, fStr, sizeof(fStr));
        g_logger.log("GRAPH_REMOVE_FILTER", self, self, _ReturnAddress(), "filter=%p %s", pFilter, fStr);
    }
    return g_origRemoveFilter(self, pFilter);
}

static HRESULT STDMETHODCALLTYPE disconnectPinHook(IGraphBuilder* self, IPin* pPin) {
    if (!g_inHook) {
        HookScope scope;
        char pStr[256]{};
        pinInfo(pPin, pStr, sizeof(pStr));
        g_logger.log("GRAPH_DISCONNECT_PIN", self, self, _ReturnAddress(), "pin=%p %s", pPin, pStr);
    }
    return g_origDisconnectPin(self, pPin);
}

static HRESULT STDMETHODCALLTYPE reconnectPinHook(IGraphBuilder* self, IPin* pPin) {
    if (!g_inHook) {
        HookScope scope;
        char pStr[256]{};
        pinInfo(pPin, pStr, sizeof(pStr));
        g_logger.log("GRAPH_RECONNECT_PIN", self, self, _ReturnAddress(), "pin=%p %s", pPin, pStr);
    }
    return g_origReconnectPin(self, pPin);
}

// ---------------------------------------------------------------------------
// 5. IBaseFilter::Stop & IPin Hooks (BeginFlush, EndFlush, EndOfStream, NewSegment)
// ---------------------------------------------------------------------------
using BaseFilterStopFn = HRESULT(STDMETHODCALLTYPE*)(IBaseFilter*);
using PinOneFn = HRESULT(STDMETHODCALLTYPE*)(IPin*);
using PinSegmentFn = HRESULT(STDMETHODCALLTYPE*)(IPin*, REFERENCE_TIME, REFERENCE_TIME, double);
using MemReceiveFn = HRESULT(STDMETHODCALLTYPE*)(IMemInputPin*, IMediaSample*);

static BaseFilterStopFn g_origBaseFilterStop = nullptr;
static PinOneFn g_origBeginFlush = nullptr;
static PinOneFn g_origEndFlush = nullptr;
static PinOneFn g_origEndOfStream = nullptr;
static PinSegmentFn g_origNewSegment = nullptr;
static MemReceiveFn g_origMemReceive = nullptr;

static HRESULT STDMETHODCALLTYPE baseFilterStopHook(IBaseFilter* self) {
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        char fStr[128]{};
        filterInfo(self, fStr, sizeof(fStr));
        g_logger.log("BASE_FILTER_STOP", self, graph, _ReturnAddress(), "%s", fStr);
    }
    return g_origBaseFilterStop(self);
}

static HRESULT STDMETHODCALLTYPE beginFlushHook(IPin* self) {
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        char pStr[256]{};
        pinInfo(self, pStr, sizeof(pStr));
        g_logger.log("PIN_BEGIN_FLUSH", self, graph, _ReturnAddress(), "%s", pStr);
    }
    return g_origBeginFlush(self);
}

static HRESULT STDMETHODCALLTYPE endFlushHook(IPin* self) {
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        char pStr[256]{};
        pinInfo(self, pStr, sizeof(pStr));
        g_logger.log("PIN_END_FLUSH", self, graph, _ReturnAddress(), "%s", pStr);
    }
    return g_origEndFlush(self);
}

static HRESULT STDMETHODCALLTYPE endOfStreamHook(IPin* self) {
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        char pStr[256]{};
        pinInfo(self, pStr, sizeof(pStr));
        g_logger.log("PIN_EOS", self, graph, _ReturnAddress(), "%s", pStr);
    }
    return g_origEndOfStream(self);
}

static HRESULT STDMETHODCALLTYPE newSegmentHook(IPin* self, REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) {
    if (!g_inHook) {
        HookScope scope;
        void* graph = graphFromObject(self);
        char pStr[256]{};
        pinInfo(self, pStr, sizeof(pStr));
        double sSec = double(tStart) / 10000000.0;
        double eSec = double(tStop) / 10000000.0;
        g_logger.log("PIN_NEW_SEGMENT", self, graph, _ReturnAddress(),
                     "start=%lld(%.4fs) stop=%lld(%.4fs) rate=%.3f %s",
                     tStart, sSec, tStop, eSec, dRate, pStr);
    }
    return g_origNewSegment(self, tStart, tStop, dRate);
}

static std::atomic<uint64_t> g_sampleCount{0};

static HRESULT STDMETHODCALLTYPE memReceiveHook(IMemInputPin* self, IMediaSample* sample) {
    if (!g_inHook && sample) {
        uint64_t cnt = ++g_sampleCount;
        if ((cnt <= 5) || (cnt % 100 == 0)) {
            HookScope scope;
            REFERENCE_TIME s = 0, e = 0;
            HRESULT tHr = sample->GetTime(&s, &e);
            long len = sample->GetActualDataLength();
            IPin* p = nullptr;
            void* graph = nullptr;
            char pStr[128]{};
            if (SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&p))) && p) {
                graph = graphFromObject(p);
                pinInfo(p, pStr, sizeof(pStr));
                p->Release();
            }
            g_logger.log("SAMPLE_RECEIVE", self, graph, _ReturnAddress(),
                         "sampleSeq=%llu len=%ld timeHr=0x%08X start=%lld(%.4fs) end=%lld(%.4fs) %s",
                         cnt, len, tHr, s, double(s) / 1e7, e, double(e) / 1e7, pStr);
        }
    }
    return g_origMemReceive(self, sample);
}

// ---------------------------------------------------------------------------
// 6. DirectSound Buffer Hooks (Stop, Play, SetCurrentPosition)
// ---------------------------------------------------------------------------
using DsStopFn = HRESULT(WINAPI*)(IDirectSoundBuffer*);
using DsPlayFn = HRESULT(WINAPI*)(IDirectSoundBuffer*, DWORD, DWORD, DWORD);
using DsSetPosFn = HRESULT(WINAPI*)(IDirectSoundBuffer*, DWORD);

static DsStopFn g_origDsStop = nullptr;
static DsPlayFn g_origDsPlay = nullptr;
static DsSetPosFn g_origDsSetPos = nullptr;

static HRESULT WINAPI dsStopHook(IDirectSoundBuffer* self) {
    if (!g_inHook) {
        HookScope scope;
        DWORD playCursor = 0, writeCursor = 0;
        self->GetCurrentPosition(&playCursor, &writeCursor);
        DSBCAPS caps{};
        caps.dwSize = sizeof(caps);
        self->GetCaps(&caps);
        WAVEFORMATEX wfx{};
        self->GetFormat(&wfx, sizeof(wfx), nullptr);

        void* frames[6]{};
        USHORT count = CaptureStackBackTrace(1, 6, frames, nullptr);
        char bt[384]{};
        flowObserver::FlowLogger::formatBacktrace(frames, count, bt, sizeof(bt));

        g_logger.log("DS_STOP", self, nullptr, _ReturnAddress(),
                     "playCursor=%lu writeCursor=%lu bufBytes=%lu rate=%lu bits=%u ch=%u backtrace=%s",
                     playCursor, writeCursor, caps.dwBufferBytes,
                     wfx.nSamplesPerSec, wfx.wBitsPerSample, wfx.nChannels, bt);
    }
    return g_origDsStop(self);
}

static HRESULT WINAPI dsPlayHook(IDirectSoundBuffer* self, DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags) {
    if (!g_inHook) {
        HookScope scope;
        DWORD playCursor = 0, writeCursor = 0;
        self->GetCurrentPosition(&playCursor, &writeCursor);
        g_logger.log("DS_PLAY", self, nullptr, _ReturnAddress(),
                     "flags=0x%08X playCursor=%lu writeCursor=%lu", dwFlags, playCursor, writeCursor);
    }
    return g_origDsPlay(self, dwReserved1, dwPriority, dwFlags);
}

static HRESULT WINAPI dsSetPosHook(IDirectSoundBuffer* self, DWORD dwNewPosition) {
    if (!g_inHook) {
        HookScope scope;
        DWORD curPlay = 0, curWrite = 0;
        self->GetCurrentPosition(&curPlay, &curWrite);
        g_logger.log("DS_SETPOSITION", self, nullptr, _ReturnAddress(),
                     "newPos=%lu oldPlay=%lu oldWrite=%lu", dwNewPosition, curPlay, curWrite);
    }
    return g_origDsSetPos(self, dwNewPosition);
}

// ---------------------------------------------------------------------------
// 7. OpcFlac.dll Hooks (COpcFlacPlayer::Open, Close)
// ---------------------------------------------------------------------------
using OpcOpenFn = HRESULT(__fastcall*)(void*, void*, LPCWSTR, DWORD);
using OpcCloseFn = HRESULT(__fastcall*)(void*, void*);

static OpcOpenFn g_origOpcOpen = nullptr;
static OpcCloseFn g_origOpcClose = nullptr;

static HRESULT __fastcall opcOpenHook(void* self, void* /*edx*/, LPCWSTR path, DWORD flags) {
    LARGE_INTEGER tStart;
    QueryPerformanceCounter(&tStart);
    if (!g_inHook) {
        HookScope scope;
        const wchar_t* pName = path ? wcsrchr(path, L'\\') : nullptr;
        const wchar_t* shortPath = pName ? (pName + 1) : (path ? path : L"(null)");
        g_logger.log("OPC_FLAC_OPEN_ENTER", self, nullptr, _ReturnAddress(),
                     "flags=%u(%s) file=%ls", flags, (flags == 1 ? "SeamlessPreRead" : "NormalTrackOpen"), shortPath);
    }

    HRESULT hr = g_origOpcOpen(self, nullptr, path, flags);

    if (!g_inHook) {
        HookScope scope;
        LARGE_INTEGER tEnd, freq;
        QueryPerformanceCounter(&tEnd);
        QueryPerformanceFrequency(&freq);
        int64_t elapsedUs = (freq.QuadPart > 0) ? (((tEnd.QuadPart - tStart.QuadPart) * 1000000LL) / freq.QuadPart) : 0;
        g_logger.log("OPC_FLAC_OPEN_LEAVE", self, nullptr, _ReturnAddress(),
                     "flags=%u hr=0x%08X elapsedUs=%lld", flags, hr, elapsedUs);
    }
    return hr;
}

static HRESULT __fastcall opcCloseHook(void* self, void* /*edx*/) {
    if (!g_inHook) {
        HookScope scope;
        g_logger.log("OPC_FLAC_CLOSE_ENTER", self, nullptr, _ReturnAddress(), "");
    }
    HRESULT hr = g_origOpcClose(self, nullptr);
    if (!g_inHook) {
        HookScope scope;
        g_logger.log("OPC_FLAC_CLOSE_LEAVE", self, nullptr, _ReturnAddress(), "hr=0x%08X", hr);
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Setup & Initialization
// ---------------------------------------------------------------------------
static bool hookPinsOf(IBaseFilter* filter) {
    if (!filter) return false;
    IEnumPins* enumPins = nullptr;
    if (FAILED(filter->EnumPins(&enumPins)) || !enumPins) return false;

    IPin* p = nullptr;
    bool ok = true;
    while (enumPins->Next(1, &p, nullptr) == S_OK) {
        auto v = *reinterpret_cast<void***>(p);
        ok = hookOnce(v[14], reinterpret_cast<void*>(endOfStreamHook), reinterpret_cast<void**>(&g_origEndOfStream)) && ok;
        ok = hookOnce(v[15], reinterpret_cast<void*>(beginFlushHook), reinterpret_cast<void**>(&g_origBeginFlush)) && ok;
        ok = hookOnce(v[16], reinterpret_cast<void*>(endFlushHook), reinterpret_cast<void**>(&g_origEndFlush)) && ok;
        ok = hookOnce(v[17], reinterpret_cast<void*>(newSegmentHook), reinterpret_cast<void**>(&g_origNewSegment)) && ok;

        IMemInputPin* mem = nullptr;
        if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&mem))) && mem) {
            auto memV = *reinterpret_cast<void***>(mem);
            ok = hookOnce(memV[6], reinterpret_cast<void*>(memReceiveHook), reinterpret_cast<void**>(&g_origMemReceive)) && ok;
            mem->Release();
        }
        p->Release();
    }
    enumPins->Release();
    return ok;
}

static bool setupAllHooks() {
    bool ok = (MH_Initialize() == MH_OK);

    // 1. Hook FilterGraph interfaces
    IGraphBuilder* graph = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph))) && graph) {
        auto graphV = *reinterpret_cast<void***>(graph);
        ok = hookOnce(graphV[5], reinterpret_cast<void*>(removeFilterHook), reinterpret_cast<void**>(&g_origRemoveFilter)) && ok;
        ok = hookOnce(graphV[16], reinterpret_cast<void*>(disconnectPinHook), reinterpret_cast<void**>(&g_origDisconnectPin)) && ok;
        ok = hookOnce(graphV[17], reinterpret_cast<void*>(reconnectPinHook), reinterpret_cast<void**>(&g_origReconnectPin)) && ok;

        IMediaEventSink* sink = nullptr;
        if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&sink))) && sink) {
            auto sinkV = *reinterpret_cast<void***>(sink);
            ok = hookOnce(sinkV[3], reinterpret_cast<void*>(notifyHook), reinterpret_cast<void**>(&g_origNotify)) && ok;
            sink->Release();
        }

        IMediaSeeking* seeking = nullptr;
        if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&seeking))) && seeking) {
            auto seekV = *reinterpret_cast<void***>(seeking);
            ok = hookOnce(seekV[11], reinterpret_cast<void*>(getStopPositionHook), reinterpret_cast<void**>(&g_origGetStopPosition)) && ok;
            ok = hookOnce(seekV[12], reinterpret_cast<void*>(getCurrentPositionHook), reinterpret_cast<void**>(&g_origGetCurrentPosition)) && ok;
            ok = hookOnce(seekV[14], reinterpret_cast<void*>(setPositionsHook), reinterpret_cast<void**>(&g_origSetPositions)) && ok;
            ok = hookOnce(seekV[15], reinterpret_cast<void*>(getPositionsHook), reinterpret_cast<void**>(&g_origGetPositions)) && ok;
            seeking->Release();
        }

        IMediaControl* control = nullptr;
        if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&control))) && control) {
            auto ctrlV = *reinterpret_cast<void***>(control);
            ok = hookOnce(ctrlV[7], reinterpret_cast<void*>(controlRunHook), reinterpret_cast<void**>(&g_origControlRun)) && ok;
            ok = hookOnce(ctrlV[8], reinterpret_cast<void*>(controlPauseHook), reinterpret_cast<void**>(&g_origControlPause)) && ok;
            ok = hookOnce(ctrlV[9], reinterpret_cast<void*>(controlStopHook), reinterpret_cast<void**>(&g_origControlStop)) && ok;
            control->Release();
        }

        IMediaFilter* mFilter = nullptr;
        if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&mFilter))) && mFilter) {
            auto filtV = *reinterpret_cast<void***>(mFilter);
            ok = hookOnce(filtV[4], reinterpret_cast<void*>(filterStopHook), reinterpret_cast<void**>(&g_origFilterStop)) && ok;
            ok = hookOnce(filtV[5], reinterpret_cast<void*>(filterPauseHook), reinterpret_cast<void**>(&g_origFilterPause)) && ok;
            ok = hookOnce(filtV[6], reinterpret_cast<void*>(filterRunHook), reinterpret_cast<void**>(&g_origFilterRun)) && ok;
            mFilter->Release();
        }

        graph->Release();
    }

    // 2. Hook Sony DirectShow filters (EQ, Seamless, DSEE)
    struct Spec {
        const wchar_t* file;
        const wchar_t* id;
    };
    Spec specs[] = {
        { L"OmgDseeHxFilter.ax", L"{A481D160-AB4D-4B97-93FE-C1F4B4B781DF}" },
        { L"SeamlessFilter.ax",  L"{6AF73F1F-1266-46E6-9A36-EE75E4BA10BB}" },
        { L"OmgEqxlFilter.ax",   L"{5C968488-5E35-4CFE-98A1-A51C838797DB}" }
    };

    for (const auto& sp : specs) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\%s", sp.file);
        HMODULE hMod = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (hMod) {
            auto getFn = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
                GetProcAddress(hMod, "DllGetClassObject"));
            CLSID cls{};
            CLSIDFromString(sp.id, &cls);
            IClassFactory* factory = nullptr;
            if (getFn && SUCCEEDED(getFn(cls, IID_PPV_ARGS(&factory))) && factory) {
                IBaseFilter* filter = nullptr;
                if (SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&filter))) && filter) {
                    auto filtV = *reinterpret_cast<void***>(filter);
                    ok = hookOnce(filtV[6], reinterpret_cast<void*>(baseFilterStopHook), reinterpret_cast<void**>(&g_origBaseFilterStop)) && ok;
                    hookPinsOf(filter);
                    filter->Release();
                }
                factory->Release();
            }
        }
    }

    // 3. Hook DirectSound Renderer filter and pins
    IBaseFilter* dsRender = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DSoundRender, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dsRender))) && dsRender) {
        auto dsV = *reinterpret_cast<void***>(dsRender);
        ok = hookOnce(dsV[6], reinterpret_cast<void*>(baseFilterStopHook), reinterpret_cast<void**>(&g_origBaseFilterStop)) && ok;
        hookPinsOf(dsRender);
        dsRender->Release();
    }

    // 4. Hook IDirectSoundBuffer
    IDirectSound8* ds8 = nullptr;
    if (SUCCEEDED(DirectSoundCreate8(nullptr, &ds8, nullptr)) && ds8) {
        WAVEFORMATEX wfx{ WAVE_FORMAT_PCM, 2, 48000, 288000, 6, 24, 0 };
        DSBUFFERDESC desc{};
        desc.dwSize = sizeof(desc);
        desc.dwBufferBytes = 28800;
        desc.lpwfxFormat = &wfx;
        IDirectSoundBuffer* buf = nullptr;
        if (SUCCEEDED(ds8->CreateSoundBuffer(&desc, &buf, nullptr)) && buf) {
            auto bufV = *reinterpret_cast<void***>(buf);
            ok = hookOnce(bufV[12], reinterpret_cast<void*>(dsPlayHook), reinterpret_cast<void**>(&g_origDsPlay)) && ok;
            ok = hookOnce(bufV[13], reinterpret_cast<void*>(dsSetPosHook), reinterpret_cast<void**>(&g_origDsSetPos)) && ok;
            ok = hookOnce(bufV[18], reinterpret_cast<void*>(dsStopHook), reinterpret_cast<void**>(&g_origDsStop)) && ok;
            buf->Release();
        }
        ds8->Release();
    }

    // 5. Hook OpcFlac.dll if already loaded
    HMODULE hOpcFlac = GetModuleHandleW(L"OpcFlac.dll");
    if (!hOpcFlac) {
        // Try loading from standard Music Center path if available
        hOpcFlac = LoadLibraryExW(L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\OpcFlac.dll",
                                  nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (hOpcFlac) {
        auto base = reinterpret_cast<uintptr_t>(hOpcFlac);
        void* openAddr = reinterpret_cast<void*>(base + 0x33290);
        void* closeAddr = reinterpret_cast<void*>(base + 0x319E0);
        ok = hookOnce(openAddr, reinterpret_cast<void*>(opcOpenHook), reinterpret_cast<void**>(&g_origOpcOpen)) && ok;
        ok = hookOnce(closeAddr, reinterpret_cast<void*>(opcCloseHook), reinterpret_cast<void**>(&g_origOpcClose)) && ok;
    }

    if (ok) {
        MH_EnableHook(MH_ALL_HOOKS);
    }
    return ok;
}

static DWORD WINAPI startThread(void*) {
    wchar_t tempDir[MAX_PATH], logPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    swprintf_s(logPath, L"%sFlowObserver-%lu.tsv", tempDir, GetCurrentProcessId());

    if (!g_logger.initFile(logPath)) {
        return 1;
    }

    g_logger.log("OBSERVER_START", nullptr, nullptr, nullptr, "log_path=%ls", logPath);

    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = setupAllHooks();
    if (SUCCEEDED(co)) CoUninitialize();

    g_logger.log(ok ? "OBSERVER_READY" : "OBSERVER_FAILED", nullptr, nullptr, nullptr, "status=%d", ok ? 1 : 0);
    return ok ? 0 : 2;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE t = CreateThread(nullptr, 0, startThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    } else if (dwReason == DLL_PROCESS_DETACH) {
        if (!lpReserved) {
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            g_logger.close();
        }
    }
    return TRUE;
}
