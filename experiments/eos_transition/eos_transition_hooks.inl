// Experimental hooks for segment-based EndOfStream delivery across format-changing
// transitions. Included by src/bridge/flac24_override.cpp only when
// EXPERIMENT_EOS_TRANSITION is defined.
static SRWLOCK eosLock = SRWLOCK_INIT;
static eosTransition::Tracker eosTracker;
static thread_local bool inEosHook = false;

struct EosHookScope {
    bool outer;
    EosHookScope() : outer(!inEosHook) { inEosHook = true; }
    ~EosHookScope() { if (outer) inEosHook = false; }
};

static void* graphKeyFromGraph(IFilterGraph* graph) {
    if (!graph) return nullptr;
    IUnknown* unk = nullptr;
    if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&unk))) && unk) {
        unk->Release();
        return reinterpret_cast<void*>(unk);
    }
    return reinterpret_cast<void*>(graph);
}

static void* graphKeyFromFilter(IBaseFilter* filter) {
    if (!filter) return nullptr;
    FILTER_INFO fi{};
    if (FAILED(filter->QueryFilterInfo(&fi)) || !fi.pGraph) return nullptr;
    void* key = graphKeyFromGraph(fi.pGraph);
    fi.pGraph->Release();
    return key;
}

static void* graphKeyFromPin(IPin* pin) {
    if (!pin) return nullptr;
    PIN_INFO pi{};
    if (FAILED(pin->QueryPinInfo(&pi)) || !pi.pFilter) return nullptr;
    void* key = graphKeyFromFilter(pi.pFilter);
    pi.pFilter->Release();
    return key;
}

static void* graphKeyFromMemPin(IMemInputPin* mem) {
    if (!mem) return nullptr;
    IPin* pin = nullptr;
    if (FAILED(mem->QueryInterface(IID_PPV_ARGS(&pin))) || !pin) return nullptr;
    void* key = graphKeyFromPin(pin);
    pin->Release();
    return key;
}

static void* graphKeyFromObject(IUnknown* obj) {
    if (!obj) return nullptr;
    IFilterGraph* graph = nullptr;
    if (SUCCEEDED(obj->QueryInterface(IID_PPV_ARGS(&graph))) && graph) {
        void* key = graphKeyFromGraph(graph);
        graph->Release();
        return key;
    }
    IBaseFilter* filter = nullptr;
    if (SUCCEEDED(obj->QueryInterface(IID_PPV_ARGS(&filter))) && filter) {
        void* key = graphKeyFromFilter(filter);
        filter->Release();
        return key;
    }
    IPin* pin = nullptr;
    if (SUCCEEDED(obj->QueryInterface(IID_PPV_ARGS(&pin))) && pin) {
        void* key = graphKeyFromPin(pin);
        pin->Release();
        return key;
    }
    return nullptr;
}

static bool querySourceWaveFormat(IUnknown* self, WAVEFORMATEX& waveOut) {
    memset(&waveOut, 0, sizeof(waveOut));
    IBaseFilter* f = nullptr;
    if (FAILED(self->QueryInterface(IID_PPV_ARGS(&f)))) return false;
    IEnumPins* pins = nullptr;
    bool found = false;
    if (SUCCEEDED(f->EnumPins(&pins))) {
        IPin* p = nullptr;
        while (pins->Next(1, &p, nullptr) == S_OK) {
            PIN_DIRECTION direction{};
            p->QueryDirection(&direction);
            IEnumMediaTypes* types = nullptr;
            if (direction == PINDIR_OUTPUT && SUCCEEDED(p->EnumMediaTypes(&types))) {
                AM_MEDIA_TYPE* mt = nullptr;
                if (types->Next(1, &mt, nullptr) == S_OK) {
                    if (mt->majortype == MEDIATYPE_Audio && mt->subtype == MEDIASUBTYPE_PCM &&
                        mt->formattype == FORMAT_WaveFormatEx && mt->cbFormat >= 16 && mt->pbFormat) {
                        memcpy(&waveOut, mt->pbFormat, min(size_t(mt->cbFormat), sizeof(waveOut)));
                        found = true;
                    }
                    if (mt->cbFormat) CoTaskMemFree(mt->pbFormat);
                    if (mt->pUnk) mt->pUnk->Release();
                    CoTaskMemFree(mt);
                }
                types->Release();
            }
            p->Release();
            if (found) break;
        }
        pins->Release();
    }
    f->Release();
    return found;
}

static bool isPreReadReturnAddress(void* returnAddress) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(returnAddress), &m)) {
        return false;
    }
    auto base = reinterpret_cast<ULONG_PTR>(m);
    return (reinterpret_cast<ULONG_PTR>(returnAddress) - base) == 0x31E7A;
}

static void onEosSetterDecision(IUnknown* self, void* returnAddress, bool eligible24, HRESULT hr) {
    WAVEFORMATEX wave{};
    querySourceWaveFormat(self, wave);
    void* key = graphKeyFromObject(self);
    bool preRead = isPreReadReturnAddress(returnAddress);
    AcquireSRWLockExclusive(&eosLock);
    if (preRead) {
        bool applied24 = eligible24 && SUCCEEDED(hr);
        eosTracker.onPreReadFormatCandidate(key, applied24, wave.nSamplesPerSec, wave.nChannels,
                                            applied24 ? 24 : wave.wBitsPerSample);
    } else {
        eosTracker.onManualTrackChange(key);
    }
    ReleaseSRWLockExclusive(&eosLock);
    record(preRead ? (eligible24 && SUCCEEDED(hr) ? "EOS_PREREAD_24" : "EOS_PREREAD_DIFF")
                   : "EOS_TRACK_OPEN",
           wave.nSamplesPerSec, eligible24 && SUCCEEDED(hr) ? 24 : wave.wBitsPerSample, hr);
}

static void onEosGraphStatus(IFilterGraph* graph, bool active24, DWORD rate, WORD channels, WORD bits) {
    void* key = graphKeyFromGraph(graph);
    if (!key) return;
    AcquireSRWLockExclusive(&eosLock);
    eosTracker.onGraphActivated(key, active24, rate, channels, bits);
    ReleaseSRWLockExclusive(&eosLock);
}

static void syncGraphActivationFromPin(IPin* pin, void* key) {
    if (!pin || !key) return;
    AM_MEDIA_TYPE mt{};
    if (SUCCEEDED(pin->ConnectionMediaType(&mt))) {
        if (mt.formattype == FORMAT_WaveFormatEx && mt.cbFormat >= 16 && mt.pbFormat) {
            WAVEFORMATEX wave{};
            memcpy(&wave, mt.pbFormat, min(size_t(mt.cbFormat), sizeof(wave)));
            bool is24 = bridgeStatus::directSoundSelected() &&
                        supportedOutputRate(wave.nSamplesPerSec) &&
                        wave.wBitsPerSample == 24 &&
                        wave.nChannels == 2;
            AcquireSRWLockExclusive(&eosLock);
            eosTracker.onGraphActivated(key, is24, wave.nSamplesPerSec, wave.nChannels, wave.wBitsPerSample);
            ReleaseSRWLockExclusive(&eosLock);
        }
        if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
        if (mt.pUnk) mt.pUnk->Release();
    }
}

#include <dsound.h>
#include <amaudio.h>

static const GUID kIID_IAMDirectSound = { 0x546f4260, 0xd82c, 0x11cf, { 0x8d, 0x23, 0x00, 0xaa, 0x00, 0x5c, 0xf4, 0x66 } };

using PinOneFn = HRESULT(STDMETHODCALLTYPE*)(IPin*);
using PinSegmentFn = HRESULT(STDMETHODCALLTYPE*)(IPin*, REFERENCE_TIME, REFERENCE_TIME, double);
using MemReceiveFn = HRESULT(STDMETHODCALLTYPE*)(IMemInputPin*, IMediaSample*);
using ControlPauseFn = HRESULT(STDMETHODCALLTYPE*)(IMediaControl*);
using ControlStopFn = HRESULT(STDMETHODCALLTYPE*)(IMediaControl*);
using FilterPauseFn = HRESULT(STDMETHODCALLTYPE*)(IMediaFilter*);
using FilterStopFn = HRESULT(STDMETHODCALLTYPE*)(IMediaFilter*);
using GetPositionFn = HRESULT(STDMETHODCALLTYPE*)(IMediaSeeking*, LONGLONG*);
using SetPositionsFn = HRESULT(STDMETHODCALLTYPE*)(IMediaSeeking*, LONGLONG*, DWORD, LONGLONG*, DWORD);
using DsPlayFn = HRESULT(WINAPI*)(IDirectSoundBuffer*, DWORD, DWORD, DWORD);
using DsStopFn = HRESULT(WINAPI*)(IDirectSoundBuffer*);
using DsSetCurrentPositionFn = HRESULT(WINAPI*)(IDirectSoundBuffer*, DWORD);
using DsSetVolumeFn = HRESULT(WINAPI*)(IDirectSoundBuffer*, LONG);

static MemReceiveFn originalSeamlessReceive = nullptr;
static PinOneFn originalSeamlessEos = nullptr;
static PinOneFn originalSeamlessBeginFlush = nullptr;
static PinOneFn originalSeamlessEndFlush = nullptr;
static PinSegmentFn originalSeamlessNewSegment = nullptr;

static MemReceiveFn originalEqReceive = nullptr;
static PinOneFn originalEqEos = nullptr;
static PinOneFn originalEqBeginFlush = nullptr;
static PinOneFn originalEqEndFlush = nullptr;
static PinSegmentFn originalEqNewSegment = nullptr;

static ControlPauseFn originalControlPause = nullptr;
static ControlStopFn originalControlStop = nullptr;
static FilterPauseFn originalFilterPause = nullptr;
static FilterStopFn originalFilterStop = nullptr;
static GetPositionFn originalGetStopPosition = nullptr;
static GetPositionFn originalGetCurrentPosition = nullptr;
static SetPositionsFn originalSetPositions = nullptr;
static DsPlayFn originalDsPlay = nullptr;
static DsStopFn originalDsStop = nullptr;
static DsSetCurrentPositionFn originalDsSetCurrentPosition = nullptr;
static DsSetVolumeFn originalDsSetVolume = nullptr;

using EnterCsFn = void(WINAPI*)(LPCRITICAL_SECTION);
using LeaveCsFn = void(WINAPI*)(LPCRITICAL_SECTION);
using DispatchMsgFn = LRESULT(WINAPI*)(const MSG*);

static EnterCsFn originalOpcEnterCriticalSection = nullptr;
static LeaveCsFn originalOpcLeaveCriticalSection = nullptr;
static DispatchMsgFn originalDispatchMessageW = nullptr;

static thread_local bool inLockHook = false;
static thread_local bool inMsgHook = false;
static std::atomic<bool> g_opcLockHooksAttached{false};
static bool g_lockDiagEnabled = true;

#include "eos_transition_diag.h"

enum class PauseMode {
    Defer,
    Observe,
    AsyncVirtual
};

#if defined(EXPERIMENT_ASYNC_VIRTUAL_PAUSE)
static PauseMode g_pauseMode = PauseMode::AsyncVirtual;
#else
static PauseMode g_pauseMode = PauseMode::Defer;
#endif
static bool g_diagEnabled = false;

static void initDiagMode() {
    wchar_t envBuf[32]{};
    DWORD m = GetEnvironmentVariableW(L"MC_BRIDGE_PAUSE_MODE", envBuf, 32);
    if (m > 0) {
        if (_wcsicmp(envBuf, L"observe") == 0 || _wcsicmp(envBuf, L"0") == 0) {
            g_pauseMode = PauseMode::Observe;
        } else if (_wcsicmp(envBuf, L"sync_defer") == 0 || _wcsicmp(envBuf, L"defer") == 0) {
            g_pauseMode = PauseMode::Defer;
        } else if (_wcsicmp(envBuf, L"async_virtual") == 0 || _wcsicmp(envBuf, L"async") == 0 || _wcsicmp(envBuf, L"1") == 0) {
            g_pauseMode = PauseMode::AsyncVirtual;
        }
    }
    DWORD n = GetEnvironmentVariableW(L"MC_BRIDGE_PAUSE_MODE", envBuf, 32);
    if (n > 0 && (_wcsicmp(envBuf, L"diag") == 0 || _wcsicmp(envBuf, L"diagnostic") == 0)) {
        g_diagEnabled = true;
    }
    DWORD d = GetEnvironmentVariableW(L"MC_BRIDGE_DIAGNOSTICS", envBuf, 32);
    if (d > 0 && (_wcsicmp(envBuf, L"1") == 0 || _wcsicmp(envBuf, L"true") == 0)) {
        g_diagEnabled = true;
    }
#ifdef EXPERIMENT_PAUSE_DIAGNOSTICS
    g_diagEnabled = true;
#endif

    g_lockDiagEnabled = g_diagEnabled;
    DWORD lockVar = GetEnvironmentVariableW(L"MC_BRIDGE_LOCK_DIAG", envBuf, 32);
    if (lockVar > 0) {
        if (_wcsicmp(envBuf, L"0") == 0 || _wcsicmp(envBuf, L"off") == 0 || _wcsicmp(envBuf, L"false") == 0) {
            g_lockDiagEnabled = false;
        } else if (_wcsicmp(envBuf, L"1") == 0 || _wcsicmp(envBuf, L"on") == 0 || _wcsicmp(envBuf, L"true") == 0) {
            g_lockDiagEnabled = true;
        }
    }

    eosDiag::DiagClock::init();
}

static void recordDiag(const char* event, void* graphKey, void* callerAddr, uint64_t p1 = 0, uint64_t p2 = 0, HRESULT hr = S_OK) {
    eosDiag::getTraceCollector().add(event, callerAddr, p1, p2, hr);
    if (!g_diagEnabled) {
        record(event, static_cast<DWORD>(p1), static_cast<DWORD>(p2), hr);
        return;
    }
    uint64_t monoMs = eosDiag::DiagClock::monotonicMs();
    DWORD tid = GetCurrentThreadId();
    char callerStr[128]{};
    eosDiag::formatCaller(callerAddr, callerStr, sizeof(callerStr));

    char line[512]{};
    SYSTEMTIME t;
    GetSystemTime(&t);
    int n = sprintf_s(line, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\tmono_ms=%llu\ttid=%lu\tgraph=%p\t%s\tcaller=%s\tp1=%llu\tp2=%llu\thr=%08lx\r\n",
                      t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                      monoMs, tid, graphKey, event, callerStr, p1, p2, ULONG(hr));
    AcquireSRWLockExclusive(&mutex);
    if (output != INVALID_HANDLE_VALUE && n > 0) {
        DWORD written = 0;
        WriteFile(output, line, n, &written, nullptr);
    }
    ReleaseSRWLockExclusive(&mutex);
}

// Targeted IAT lock hooks on OpcDSPlay.dll (Issue #28)
static void WINAPI opcDsPlayEnterCsHook(LPCRITICAL_SECTION cs) {
    if (!cs || inLockHook || !g_lockDiagEnabled) {
        if (originalOpcEnterCriticalSection) originalOpcEnterCriticalSection(cs);
        else EnterCriticalSection(cs);
        return;
    }
    inLockHook = true;
    void* caller = _ReturnAddress();
    uint64_t tTry = eosDiag::DiagClock::monotonicMs();
    DWORD tid = GetCurrentThreadId();
    eosDiag::getLockTracker().onTry(cs, caller, tid, tTry);
    recordDiag("ENGINE_LOCK_TRY", cs, caller, reinterpret_cast<uint64_t>(cs), 0, S_OK);
    inLockHook = false;

    if (originalOpcEnterCriticalSection) originalOpcEnterCriticalSection(cs);
    else EnterCriticalSection(cs);

    inLockHook = true;
    uint64_t tAcquired = eosDiag::DiagClock::monotonicMs();
    uint64_t waitMs = eosDiag::getLockTracker().onAcquired(cs, caller, tid, tAcquired);
    recordDiag("ENGINE_LOCK_ACQUIRED", cs, caller, reinterpret_cast<uint64_t>(cs), waitMs, S_OK);
    inLockHook = false;
}

static void WINAPI opcDsPlayLeaveCsHook(LPCRITICAL_SECTION cs) {
    if (!cs || inLockHook || !g_lockDiagEnabled) {
        if (originalOpcLeaveCriticalSection) originalOpcLeaveCriticalSection(cs);
        else LeaveCriticalSection(cs);
        return;
    }
    inLockHook = true;
    void* caller = _ReturnAddress();
    uint64_t tRelease = eosDiag::DiagClock::monotonicMs();
    DWORD tid = GetCurrentThreadId();
    uint64_t heldMs = eosDiag::getLockTracker().onReleased(cs, caller, tid, tRelease);
    recordDiag("ENGINE_LOCK_RELEASED", cs, caller, reinterpret_cast<uint64_t>(cs), heldMs, S_OK);
    inLockHook = false;

    if (originalOpcLeaveCriticalSection) originalOpcLeaveCriticalSection(cs);
    else LeaveCriticalSection(cs);
}

static bool tryAttachOpcDsPlayLockHooks() {
    if (!g_lockDiagEnabled || g_opcLockHooksAttached.load()) return g_opcLockHooksAttached.load();
    HMODULE hOpc = GetModuleHandleW(L"OpcDSPlay.dll");
    if (!hOpc) {
        eosDiag::getHookStatusReporter().report("Engine.OpcDSPlay.IAT.EnterCriticalSection", "PENDING_MODULE_LOAD", S_FALSE);
        return false;
    }
    bool okEnter = eosDiag::hookIat(hOpc, "KERNEL32.dll", "EnterCriticalSection",
                                    reinterpret_cast<void*>(opcDsPlayEnterCsHook),
                                    reinterpret_cast<void**>(&originalOpcEnterCriticalSection));
    bool okLeave = eosDiag::hookIat(hOpc, "KERNEL32.dll", "LeaveCriticalSection",
                                    reinterpret_cast<void*>(opcDsPlayLeaveCsHook),
                                    reinterpret_cast<void**>(&originalOpcLeaveCriticalSection));
    if (okEnter && okLeave) {
        g_opcLockHooksAttached.store(true);
        eosDiag::getHookStatusReporter().report("Engine.OpcDSPlay.IAT.EnterCriticalSection", "OK", S_OK);
        eosDiag::getHookStatusReporter().report("Engine.OpcDSPlay.IAT.LeaveCriticalSection", "OK", S_OK);
        recordDiag("DIAG_HOOK_STATUS", hOpc, nullptr, 1, 1, S_OK);
        return true;
    } else {
        eosDiag::getHookStatusReporter().report("Engine.OpcDSPlay.IAT.EnterCriticalSection", okEnter ? "OK" : "FAILED", okEnter ? S_OK : E_FAIL);
        eosDiag::getHookStatusReporter().report("Engine.OpcDSPlay.IAT.LeaveCriticalSection", okLeave ? "OK" : "FAILED", okLeave ? S_OK : E_FAIL);
        recordDiag("DIAG_HOOK_STATUS", hOpc, nullptr, okEnter ? 1 : 0, okLeave ? 1 : 0, E_FAIL);
        return false;
    }
}

static std::atomic<DWORD> g_lastUserInputTick{0};

static LRESULT WINAPI dispatchMessageWHook(const MSG* lpMsg) {
    if (!inMsgHook && lpMsg) {
        UINT msg = lpMsg->message;
        if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
            (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
            msg == WM_COMMAND || msg == WM_SYSCOMMAND || msg == WM_APP ||
            (msg >= 0x8000 && msg <= 0xBFFF)) {
            g_lastUserInputTick.store(GetTickCount());
            if (g_diagEnabled) {
                inMsgHook = true;
                void* caller = _ReturnAddress();
                recordDiag("APP_INPUT_DISPATCH", reinterpret_cast<void*>(lpMsg->hwnd), caller, msg, lpMsg->wParam, S_OK);
                inMsgHook = false;
            }
        }
    }
    return originalDispatchMessageW ? originalDispatchMessageW(lpMsg) : DispatchMessageW(lpMsg);
}

static void initPauseMode() {
    initDiagMode();
}

#ifdef TESTING_HEADLESS
static bool g_forceOpcDsPlayPauseCaller = false;
static bool g_simulateThreadCreateFailure = false;
static bool g_forceManualSeek = false;
static bool g_forceManualStop = false;
static bool g_forceManualRun = false;
#endif

static bool isCallerFromOpcDSPlay(void* returnAddress) {
#ifdef TESTING_HEADLESS
    if (g_forceOpcDsPlayPauseCaller) return true;
#endif
    if (!returnAddress) return false;
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(returnAddress), &hMod) || !hMod) {
        return false;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(hMod, path, MAX_PATH);
    const wchar_t* pName = wcsrchr(path, L'\\');
    const wchar_t* baseName = pName ? (pName + 1) : path;
    return (_wcsicmp(baseName, L"OpcDSPlay.dll") == 0);
}

static bool safeQueryDirectSoundBuffer(IDirectSoundBuffer* buf, DWORD* playCursor, DWORD* writeCursor, DWORD* status, HRESULT* hrPos, HRESULT* hrStat) {
    if (!buf) return false;
    __try {
        if (hrPos) *hrPos = buf->GetCurrentPosition(playCursor, writeCursor);
        if (hrStat) *hrStat = buf->GetStatus(status);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (hrPos) *hrPos = E_FAIL;
        if (hrStat) *hrStat = E_FAIL;
        return false;
    }
}

static bool isOpcDsPlayPauseCaller(void* returnAddress) {
#ifdef TESTING_HEADLESS
    if (g_forceOpcDsPlayPauseCaller) return true;
#endif
    if (!returnAddress) return false;
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(returnAddress), &hMod) || !hMod) {
        return false;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(hMod, path, MAX_PATH);
    const wchar_t* pName = wcsrchr(path, L'\\');
    const wchar_t* baseName = pName ? (pName + 1) : path;
    if (_wcsicmp(baseName, L"OpcDSPlay.dll") != 0) {
        return false;
    }
    auto base = reinterpret_cast<ULONG_PTR>(hMod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto rva = reinterpret_cast<ULONG_PTR>(returnAddress) - base;
    // Music Center 2.7.4 x86 timestamp 0x63EB12DD and Pause caller RVA 0xDAC9
    bool isKnownMc274 = (nt->FileHeader.TimeDateStamp == 0x63EB12DD && rva == 0xDAC9);
    return isKnownMc274;
}

static void* g_lastActiveSoundBuffer = nullptr;

static HRESULT WINAPI dsStopHook(IDirectSoundBuffer* self) {
    if (!inEosHook) {
        void* caller = _ReturnAddress();
        AcquireSRWLockShared(&eosLock);
        bool virtualActive = eosTracker.hasAnyVirtualPauseActive();
        void* activeGraphKey = nullptr;
        bool isAssoc = eosTracker.isTransitionBuffer(self, &activeGraphKey);
        ReleaseSRWLockShared(&eosLock);

        if (!virtualActive && self && !g_lastActiveSoundBuffer) {
            g_lastActiveSoundBuffer = self;
        }

        if (virtualActive && self && !isAssoc) {
            AcquireSRWLockExclusive(&eosLock);
            void* activePauseKey = nullptr;
            if (eosTracker.findActiveVirtualPauseGraph(&activePauseKey)) {
                auto* g = eosTracker.findGraph(activePauseKey);
                if (g && !g->transition.primaryDsBuffer) {
                    self->AddRef();
                    eosTracker.registerActiveSoundBuffer(activePauseKey, self);
                    isAssoc = true;
                    activeGraphKey = activePauseKey;
                    recordDiag("DRAIN_DSOUND_ATTACHED_IN_STOP", activePauseKey, caller, reinterpret_cast<uint64_t>(self), 0, S_OK);
                }
            }
            ReleaseSRWLockExclusive(&eosLock);
        }

        if (virtualActive) {
            bool isManualStop = false;
#ifdef TESTING_HEADLESS
            if (g_forceManualStop) isManualStop = true;
#endif
            // Check if user manually stopped via input message
            DWORD lastInput = g_lastUserInputTick.load();
            if (lastInput != 0 && (GetTickCount() - lastInput < 400)) {
                isManualStop = true;
            }

            // If user did not manually request stop, suppress automatic renderer/engine buffer stop
            // so the queued PCM audio drains naturally through the sound card!
            if (!isManualStop) {
                recordDiag("DS_STOP_SUPPRESSED_FOR_VIRTUAL_PAUSE", activeGraphKey, caller, 0, 0, S_OK);
                return DS_OK;
            } else {
                AcquireSRWLockExclusive(&eosLock);
                eosTracker.cancelVirtualPause(activeGraphKey, eosTransition::PauseDeferralStatus::CancelledStop);
                ReleaseSRWLockExclusive(&eosLock);
                recordDiag("PAUSE_DEFERRED_CANCELLED", activeGraphKey, caller, static_cast<DWORD>(eosTransition::PauseDeferralStatus::CancelledStop), 0, S_OK);
                recordDiag("DEFERRED_SETPOSITIONS_PURGED", activeGraphKey, caller, 0, 0, S_OK);
            }
        }

        // Normal stop: proceed to original DirectSoundBuffer::Stop
        DWORD play = 0, write = 0;
        self->GetCurrentPosition(&play, &write);
        record("DS_STOP", play, write, S_OK);
    }
    return originalDsStop(self);
}

static HRESULT WINAPI dsSetCurrentPositionHook(IDirectSoundBuffer* self, DWORD dwNewPosition) {
    if (!inEosHook && self) {
        void* caller = _ReturnAddress();
        AcquireSRWLockShared(&eosLock);
        bool virtualActive = eosTracker.hasAnyVirtualPauseActive();
        void* activeGraphKey = nullptr;
        bool isAssoc = eosTracker.isTransitionBuffer(self, &activeGraphKey);
        ReleaseSRWLockShared(&eosLock);

        if (virtualActive) {
            bool isManualSeek = false;
#ifdef TESTING_HEADLESS
            if (g_forceManualSeek) isManualSeek = true;
#endif
            DWORD lastInput = g_lastUserInputTick.load();
            if (lastInput != 0 && (GetTickCount() - lastInput < 400)) {
                isManualSeek = true;
            }

            if (!isManualSeek && (isAssoc || !activeGraphKey)) {
                recordDiag("DS_SETCURRENTPOS_SUPPRESSED_FOR_VIRTUAL_PAUSE", activeGraphKey ? activeGraphKey : reinterpret_cast<void*>(self), caller, dwNewPosition, 0, S_OK);
                return DS_OK;
            }
        }
    }
    return originalDsSetCurrentPosition ? originalDsSetCurrentPosition(self, dwNewPosition) : DSERR_GENERIC;
}

static HRESULT WINAPI dsSetVolumeHook(IDirectSoundBuffer* self, LONG lVolume) {
    if (!inEosHook && self) {
        void* caller = _ReturnAddress();
        AcquireSRWLockShared(&eosLock);
        bool virtualActive = eosTracker.hasAnyVirtualPauseActive();
        void* activeGraphKey = nullptr;
        bool isAssoc = eosTracker.isTransitionBuffer(self, &activeGraphKey);
        ReleaseSRWLockShared(&eosLock);

        if (virtualActive) {
            bool isManual = false;
            DWORD lastInput = g_lastUserInputTick.load();
            if (lastInput != 0 && (GetTickCount() - lastInput < 400)) {
                isManual = true;
            }

            if (!isManual && (isAssoc || !activeGraphKey)) {
                recordDiag("DS_SETVOLUME_SUPPRESSED_FOR_VIRTUAL_PAUSE", activeGraphKey ? activeGraphKey : reinterpret_cast<void*>(self), caller, static_cast<uint64_t>(lVolume), 0, S_OK);
                return DS_OK;
            }
        }
    }
    return originalDsSetVolume ? originalDsSetVolume(self, lVolume) : DSERR_GENERIC;
}

static HRESULT WINAPI dsPlayHook(IDirectSoundBuffer* self, DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags) {
    if (!inEosHook) {
        void* caller = _ReturnAddress();
        void* activeGraphKey = nullptr;
        AcquireSRWLockShared(&eosLock);
        bool virtualActive = eosTracker.hasAnyVirtualPauseActive();
        bool isAssoc = eosTracker.isTransitionBuffer(self, &activeGraphKey);
        ReleaseSRWLockShared(&eosLock);

        DWORD playPos = 0, writePos = 0;
        if (self) self->GetCurrentPosition(&playPos, &writePos);
        recordDiag("DS_PLAY", activeGraphKey ? activeGraphKey : reinterpret_cast<void*>(self), caller,
                   playPos, dwFlags, S_OK);

        if (!virtualActive) {
            g_lastActiveSoundBuffer = self;
            AcquireSRWLockExclusive(&eosLock);
            eosTracker.registerActiveSoundBuffer(activeGraphKey, self);
            ReleaseSRWLockExclusive(&eosLock);
        }

        if (virtualActive && !isAssoc) {
            bool isManualPlay = false;
            DWORD lastInput = g_lastUserInputTick.load();
            if (lastInput != 0 && (GetTickCount() - lastInput < 400)) {
                isManualPlay = true;
            }
#ifdef TESTING_HEADLESS
            if (g_forceManualRun) isManualPlay = true;
#endif
            if (!isManualPlay) {
                AcquireSRWLockExclusive(&eosLock);
                void* activePauseKey = nullptr;
                eosTracker.findActiveVirtualPauseGraph(&activePauseKey);
                eosTracker.deferAutomaticDsPlay(activePauseKey ? activePauseKey : activeGraphKey, self,
                                                dwReserved1, dwPriority, dwFlags, caller);
                ReleaseSRWLockExclusive(&eosLock);
                recordDiag("DS_PLAY_DEFERRED_FOR_VIRTUAL_PAUSE", reinterpret_cast<void*>(self), caller,
                           playPos, dwFlags, DS_OK);
                return DS_OK;
            } else {
                AcquireSRWLockExclusive(&eosLock);
                void* activePauseKey = nullptr;
                eosTracker.findActiveVirtualPauseGraph(&activePauseKey);
                eosTracker.cancelVirtualPause(activePauseKey ? activePauseKey : activeGraphKey, eosTransition::PauseDeferralStatus::CancelledManualPlay);
                ReleaseSRWLockExclusive(&eosLock);
                recordDiag("PAUSE_DEFERRED_CANCELLED", activePauseKey ? activePauseKey : activeGraphKey, caller, static_cast<DWORD>(eosTransition::PauseDeferralStatus::CancelledManualPlay), 0, S_OK);
                recordDiag("DEFERRED_OPERATIONS_PURGED", activePauseKey ? activePauseKey : activeGraphKey, caller, 0, 0, S_OK);
            }
        }
    }
    return originalDsPlay ? originalDsPlay(self, dwReserved1, dwPriority, dwFlags) : DSERR_GENERIC;
}

static HRESULT STDMETHODCALLTYPE originalEqEndOfStream(IPin* pin) {
    return originalEqEos ? originalEqEos(pin) : S_OK;
}

static IDirectSoundBuffer* findDirectSoundBufferFromGraph(IFilterGraph* graph) {
    if (!graph) return nullptr;
    IDirectSoundBuffer* foundBuffer = nullptr;
    IEnumFilters* filters = nullptr;
    if (SUCCEEDED(graph->EnumFilters(&filters))) {
        IBaseFilter* f = nullptr;
        while (filters->Next(1, &f, nullptr) == S_OK) {
            IAMDirectSound* amDs = nullptr;
            if (SUCCEEDED(f->QueryInterface(kIID_IAMDirectSound, reinterpret_cast<void**>(&amDs))) && amDs) {
                LPDIRECTSOUNDBUFFER dsb = nullptr;
                if (SUCCEEDED(amDs->GetSecondaryBufferInterface(&dsb)) && dsb) {
                    foundBuffer = dsb;
                }
                amDs->Release();
            }
            f->Release();
            if (foundBuffer) break;
        }
        filters->Release();
    }
    return foundBuffer;
}

static IPin* findEqInputPinFromGraph(IFilterGraph* graph) {
    if (!graph) return nullptr;
    CLSID eqClsid{};
    CLSIDFromString(L"{5C968488-5E35-4CFE-98A1-A51C838797DB}", &eqClsid);
    IPin* foundPin = nullptr;
    IEnumFilters* filters = nullptr;
    if (SUCCEEDED(graph->EnumFilters(&filters))) {
        IBaseFilter* f = nullptr;
        while (filters->Next(1, &f, nullptr) == S_OK) {
            CLSID cls{};
            if (SUCCEEDED(f->GetClassID(&cls)) && cls == eqClsid) {
                IEnumPins* pins = nullptr;
                if (SUCCEEDED(f->EnumPins(&pins))) {
                    IPin* p = nullptr;
                    while (pins->Next(1, &p, nullptr) == S_OK) {
                        PIN_DIRECTION dir{};
                        if (SUCCEEDED(p->QueryDirection(&dir)) && dir == PINDIR_INPUT) {
                            foundPin = p;
                            break;
                        }
                        p->Release();
                    }
                    pins->Release();
                }
            }
            f->Release();
            if (foundPin) break;
        }
        filters->Release();
    }
    return foundPin;
}

static IPin* findEqInputPinInGraph(IPin* seamlessPin) {
    if (!seamlessPin) return nullptr;
    PIN_INFO pi{};
    if (FAILED(seamlessPin->QueryPinInfo(&pi)) || !pi.pFilter) return nullptr;
    FILTER_INFO fi{};
    HRESULT hr = pi.pFilter->QueryFilterInfo(&fi);
    pi.pFilter->Release();
    if (FAILED(hr) || !fi.pGraph) return nullptr;
    IPin* foundPin = findEqInputPinFromGraph(fi.pGraph);
    fi.pGraph->Release();
    return foundPin;
}

static HRESULT STDMETHODCALLTYPE seamlessReceiveHook(IMemInputPin* self, IMediaSample* sample) {
    void* key = nullptr;
    long preLen = 0;
    if (!inEosHook && sample) {
        preLen = sample->GetActualDataLength();
        if (preLen > 0) {
            key = graphKeyFromMemPin(self);
            if (key) {
                AcquireSRWLockExclusive(&eosLock);
                eosTracker.onUpstreamSamplePreReceive(key, static_cast<uint32_t>(preLen));
                ReleaseSRWLockExclusive(&eosLock);
            }
        }
    }
    auto hr = originalSeamlessReceive(self, sample);
    if (!inEosHook && SUCCEEDED(hr) && sample) {
        long len = sample->GetActualDataLength();
        if (len > 0) {
            if (!key) key = graphKeyFromMemPin(self);
            REFERENCE_TIME sStart = 0, sEnd = 0;
            bool hasTime = SUCCEEDED(sample->GetTime(&sStart, &sEnd));
            AcquireSRWLockExclusive(&eosLock);
            eosTracker.onUpstreamSample(key, static_cast<uint32_t>(len), hasTime, sStart, sEnd);
            ReleaseSRWLockExclusive(&eosLock);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE seamlessEosHook(IPin* self) {
    auto hr = originalSeamlessEos(self);
    if (!inEosHook && SUCCEEDED(hr)) {
        void* key = graphKeyFromPin(self);
        syncGraphActivationFromPin(self, key);
        bool deliverNow = false;
        AcquireSRWLockExclusive(&eosLock);
        deliverNow = eosTracker.onUpstreamEndOfStream(key);
        ReleaseSRWLockExclusive(&eosLock);
        record("EOS_UPSTREAM", 0, deliverNow ? 1 : 0, hr);
        if (deliverNow && originalEqEos) {
            IPin* eqPin = findEqInputPinInGraph(self);
            if (eqPin) {
                EosHookScope scope;
                auto eqHr = originalEqEos(eqPin);
                record("EOS_DELIVERED_IMMEDIATE", 0, 1, eqHr);
                eqPin->Release();
            }
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE seamlessBeginFlushHook(IPin* self) {
    if (!inEosHook) {
        void* key = graphKeyFromPin(self);
        void* caller = _ReturnAddress();
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onBeginFlush(key);
        uint32_t gen = eosTracker.getTransitionGeneration(key);
        ReleaseSRWLockExclusive(&eosLock);
        recordDiag("SEAMLESS_BEGIN_FLUSH", key, caller, gen, 0, S_OK);
    }
    return originalSeamlessBeginFlush(self);
}

static HRESULT STDMETHODCALLTYPE seamlessEndFlushHook(IPin* self) {
    if (!inEosHook) {
        void* key = graphKeyFromPin(self);
        void* caller = _ReturnAddress();
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onEndFlush(key);
        uint32_t gen = eosTracker.getTransitionGeneration(key);
        ReleaseSRWLockExclusive(&eosLock);
        recordDiag("SEAMLESS_END_FLUSH", key, caller, gen, 0, S_OK);
    }
    return originalSeamlessEndFlush(self);
}

static HRESULT STDMETHODCALLTYPE seamlessNewSegmentHook(IPin* self, REFERENCE_TIME tStart, REFERENCE_TIME tStop, double rate) {
    if (!inEosHook) {
        void* key = graphKeyFromPin(self);
        syncGraphActivationFromPin(self, key);
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onUpstreamNewSegment(key, tStart, tStop, rate);
        ReleaseSRWLockExclusive(&eosLock);
        record("EOS_NEW_SEGMENT", static_cast<DWORD>(tStart / 10000), static_cast<DWORD>(tStop / 10000), S_OK);
    }
    return originalSeamlessNewSegment(self, tStart, tStop, rate);
}

static HRESULT STDMETHODCALLTYPE eqReceiveHook(IMemInputPin* self, IMediaSample* sample) {
    long len = 0;
    REFERENCE_TIME sStart = 0, sEnd = 0;
    bool hasTime = false;
    if (!inEosHook && sample) {
        len = sample->GetActualDataLength();
        hasTime = SUCCEEDED(sample->GetTime(&sStart, &sEnd));
    }
    auto hr = originalEqReceive(self, sample);
    if (!inEosHook && SUCCEEDED(hr) && len > 0) {
        void* key = graphKeyFromMemPin(self);
        bool deliverEos = false;
        AcquireSRWLockExclusive(&eosLock);
        deliverEos = eosTracker.onDownstreamSample(key, static_cast<uint32_t>(len), hasTime, sStart, sEnd);
        ReleaseSRWLockExclusive(&eosLock);
        if (deliverEos && originalEqEos) {
            IPin* pin = nullptr;
            if (SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&pin))) && pin) {
                EosHookScope scope;
                auto eosHr = originalEqEndOfStream(pin);
                record("EOS_DELIVERED_DOWNSTREAM", static_cast<DWORD>(sEnd / 10000), static_cast<DWORD>(len), eosHr);
                pin->Release();
            }
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE eqEosHook(IPin* self) {
    if (inEosHook) {
        return originalEqEos(self);
    }
    void* key = graphKeyFromPin(self);
    bool allow = true;
    AcquireSRWLockExclusive(&eosLock);
    allow = eosTracker.onDownstreamEndOfStream(key);
    ReleaseSRWLockExclusive(&eosLock);
    if (!allow) {
        record("EOS_DUPLICATE_SUPPRESSED", 0, 0, S_OK);
        return S_OK;
    }
    auto hr = originalEqEos(self);
    record("EOS_PASSTHROUGH_DOWNSTREAM", 0, 1, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE eqBeginFlushHook(IPin* self) {
    if (!inEosHook) {
        void* key = graphKeyFromPin(self);
        void* caller = _ReturnAddress();
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onBeginFlush(key);
        uint32_t gen = eosTracker.getTransitionGeneration(key);
        ReleaseSRWLockExclusive(&eosLock);
        recordDiag("EQ_BEGIN_FLUSH", key, caller, gen, 0, S_OK);
    }
    return originalEqBeginFlush(self);
}

static HRESULT STDMETHODCALLTYPE eqEndFlushHook(IPin* self) {
    if (!inEosHook) {
        void* key = graphKeyFromPin(self);
        void* caller = _ReturnAddress();
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onEndFlush(key);
        uint32_t gen = eosTracker.getTransitionGeneration(key);
        ReleaseSRWLockExclusive(&eosLock);
        recordDiag("EQ_END_FLUSH", key, caller, gen, 0, S_OK);
    }
    return originalEqEndFlush(self);
}

static HRESULT STDMETHODCALLTYPE eqNewSegmentHook(IPin* self, REFERENCE_TIME tStart, REFERENCE_TIME tStop, double rate) {
    return originalEqNewSegment(self, tStart, tStop, rate);
}

static eosTransition::PauseDeferralStatus deferFormatChangePause(IUnknown* self, void* key, void* caller, bool isKnownCaller) {
    recordDiag("PAUSE_DEFERRED_START", key, caller, isKnownCaller ? 1 : 0, 0, S_OK);
    AcquireSRWLockExclusive(&eosLock);
    eosTracker.markFormatChangePauseDeferred(key);
    ReleaseSRWLockExclusive(&eosLock);

    // 1. Deliver EOS once to EQ ONLY IF downstream has already received all PCM of the segment!
    // If downstream hasn't received all PCM yet, eqReceiveHook on the streaming thread will
    // deliver EOS when the final PCM actually arrives, preventing premature blocking and lock contention.
    IFilterGraph* graph = nullptr;
    if (SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&graph))) && graph) {
        IPin* eqPin = findEqInputPinFromGraph(graph);
        if (eqPin) {
            bool deliverNow = false;
            AcquireSRWLockExclusive(&eosLock);
            const auto* g = eosTracker.findGraph(key);
            deliverNow = g && !g->eosDelivered && eosTracker.hasDownstreamReachedTarget(key);
            if (deliverNow) {
                eosTracker.onDownstreamEndOfStream(key);
            }
            ReleaseSRWLockExclusive(&eosLock);
            if (deliverNow && originalEqEos) {
                recordDiag("EQ_EOS_CALL_ENTER", key, caller, 1, 0, S_OK);
                DWORD eosStart = GetTickCount();
                EosHookScope scope;
                originalEqEndOfStream(eqPin);
                DWORD eosDuration = GetTickCount() - eosStart;
                recordDiag("EQ_EOS_CALL_LEAVE", key, caller, eosDuration, 1, S_OK);
            }
            eqPin->Release();
        }
        graph->Release();
    }

    // Check if cancelled during EQ EOS call (e.g. from concurrent UI thread)
    bool stillActiveAfterEos = false;
    eosTransition::PauseDeferralStatus exitStatus = eosTransition::PauseDeferralStatus::CompletedDrain;
    AcquireSRWLockShared(&eosLock);
    const auto* gCheck = eosTracker.findGraph(key);
    stillActiveAfterEos = gCheck && gCheck->isPauseDeferralActive();
    if (!stillActiveAfterEos && gCheck) {
        exitStatus = gCheck->pauseDeferralStatus;
    }
    ReleaseSRWLockShared(&eosLock);

    if (!stillActiveAfterEos) {
        recordDiag("PAUSE_DEFERRED_CANCEL_AFTER_EQ_EOS", key, caller, static_cast<DWORD>(exitStatus), 0, S_OK);
        return exitStatus;
    }

    // 2. Query IMediaSeeking to wait for track end drain
    IMediaSeeking* seeking = nullptr;
    self->QueryInterface(IID_PPV_ARGS(&seeking));
    LONGLONG curPos = 0, stopPos = 0;
    DWORD remainingMs = 0;
    if (seeking && SUCCEEDED(seeking->GetCurrentPosition(&curPos)) &&
        SUCCEEDED(seeking->GetStopPosition(&stopPos)) && stopPos > curPos) {
        remainingMs = static_cast<DWORD>((stopPos - curPos) / 10000);
    } else {
        AcquireSRWLockShared(&eosLock);
        const auto* g = eosTracker.findGraph(key);
        if (g && g->targetBytes > g->downstreamBytes && g->sampleRate > 0) {
            uint64_t diff = g->targetBytes - g->downstreamBytes;
            uint32_t align = (g->bitsPerSample / 8) * (g->channels > 0 ? g->channels : 2);
            if (align > 0) {
                remainingMs = static_cast<DWORD>((diff * 1000ULL) / (g->sampleRate * align));
            }
        }
        ReleaseSRWLockShared(&eosLock);
    }

    DWORD startTick = GetTickCount();
    DWORD maxWait = remainingMs > 0 ? (remainingMs + 300) : 3000;
    if (maxWait > 4500) maxWait = 4500; // safety ceiling: max 4.5s
    recordDiag("DRAIN_WAIT_ENTER", key, caller, remainingMs, maxWait, S_OK);

    DWORD lastTickLog = 0;
    while (true) {
        bool stillActive = false;
        AcquireSRWLockShared(&eosLock);
        const auto* g = eosTracker.findGraph(key);
        stillActive = g && g->isPauseDeferralActive();
        if (!stillActive && g) {
            exitStatus = g->pauseDeferralStatus;
        }
        ReleaseSRWLockShared(&eosLock);

        if (!stillActive) {
            recordDiag("DRAIN_WAIT_CANCEL_DETECTED", key, caller, static_cast<DWORD>(exitStatus), GetTickCount() - startTick, S_OK);
            break;
        }

        LONGLONG c = 0, s = 0;
        if (seeking && SUCCEEDED(seeking->GetCurrentPosition(&c)) &&
            SUCCEEDED(seeking->GetStopPosition(&s))) {
            if (c >= s && s > 0) {
                exitStatus = eosTransition::PauseDeferralStatus::CompletedDrain;
                break;
            }
        }

        if (GetTickCount() - startTick >= maxWait) {
            exitStatus = eosTransition::PauseDeferralStatus::TimedOut;
            break;
        }

        if (g_diagEnabled && (GetTickCount() - lastTickLog >= 250)) {
            lastTickLog = GetTickCount();
            recordDiag("DRAIN_WAIT_TICK", key, caller, static_cast<DWORD>(c / 10000), static_cast<DWORD>(s / 10000), S_OK);
        }

        MSG msg;
        bool quitReceived = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quitReceived = true;
                PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quitReceived) {
            exitStatus = eosTransition::PauseDeferralStatus::CancelledQuit;
            break;
        }
        Sleep(15);
    }
    if (seeking) seeking->Release();

    AcquireSRWLockExclusive(&eosLock);
    if (exitStatus == eosTransition::PauseDeferralStatus::CompletedDrain ||
        exitStatus == eosTransition::PauseDeferralStatus::TimedOut) {
        eosTracker.onFormatChangePauseCompleted(key, exitStatus);
    } else {
        eosTracker.cancelFormatChangePause(key, exitStatus);
    }
    ReleaseSRWLockExclusive(&eosLock);

    recordDiag("PAUSE_DEFERRED_RELEASE", key, caller, static_cast<DWORD>(exitStatus), GetTickCount() - startTick, S_OK);
    return exitStatus;
}

struct AsyncDrainParams {
    IStream* marshalStream = nullptr;
    IMediaSeeking* rawSeeking = nullptr;
    void* key = nullptr;
    void* caller = nullptr;
    void* primaryDsBuffer = nullptr;
    DWORD startTick = 0;
    DWORD maxWaitMs = 4500;
};

static DWORD WINAPI asyncDrainWorkerThread(LPVOID param) {
    auto* p = reinterpret_cast<AsyncDrainParams*>(param);
    if (!p) return 0;

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMediaSeeking* seeking = nullptr;
    if (p->marshalStream) {
        CoGetInterfaceAndReleaseStream(p->marshalStream, IID_IMediaSeeking, reinterpret_cast<void**>(&seeking));
        p->marshalStream = nullptr;
    } else if (p->rawSeeking) {
        seeking = p->rawSeeking;
        p->rawSeeking = nullptr;
    }

    DWORD startTick = p->startTick;
    DWORD maxWait = p->maxWaitMs;
    void* key = p->key;
    void* caller = p->caller;
    void* initialDsBuf = p->primaryDsBuffer;

    GUID timeFormat = GUID_NULL;
    HRESULT hrTf = seeking ? seeking->GetTimeFormat(&timeFormat) : E_POINTER;
    recordDiag("ASYNC_DRAIN_SEEKING_INIT", key, caller,
               (timeFormat == TIME_FORMAT_MEDIA_TIME) ? 1 : 0,
               static_cast<uint64_t>(timeFormat.Data1), hrTf);

    DWORD lastHeartbeatTick = 0;

    while (true) {
        Sleep(10);

        bool active = false;
        bool cancelled = false;
        AcquireSRWLockShared(&eosLock);
        const auto* g = eosTracker.findGraph(key);
        active = g && g->isVirtualPauseActive();
        cancelled = g && g->isCancelled();
        ReleaseSRWLockShared(&eosLock);

        if (!active || cancelled) {
            recordDiag("PAUSE_DRAIN_ABORTED_CANCEL", key, caller, 0, 0, S_OK);
            break;
        }

        // Two-stage drain verification:
        // Stage 1: Downstream PCM delivery
        bool pcmDelivered = false;
        bool curEosDelivered = false;
        uint64_t targetB = 0, downB = 0, upB = 0;
        uint32_t gen = 0;
        void* primaryBuf = nullptr;
        bool trackerRendererComplete = false;

        AcquireSRWLockExclusive(&eosLock);
        if (eosTracker.hasDownstreamReachedTarget(key)) {
            eosTracker.onDownstreamPcmDelivered(key);
        }
        const auto* curG = eosTracker.findGraph(key);
        if (curG) {
            pcmDelivered = curG->transition.pcmDeliveryComplete;
            targetB = curG->targetBytes;
            downB = curG->downstreamBytes;
            upB = curG->upstreamBytes;
            gen = curG->transitionGeneration;
            primaryBuf = curG->transition.primaryDsBuffer;
            trackerRendererComplete = curG->transition.rendererOutputComplete;
            curEosDelivered = curG->eosDelivered;
        }
        ReleaseSRWLockExclusive(&eosLock);

        if (!primaryBuf) primaryBuf = initialDsBuf;

        // If upstream completed reading (upB == targetB && targetB > 0) but downstream hasn't reached target yet,
        // intermediate DSP filters (like DSEE) may be holding the remaining tail samples in their latency delay-line.
        // Deliver EOS to EQ input pin to flush these samples!
        if (upB == targetB && targetB > 0 && downB < targetB && !curEosDelivered) {
            IFilterGraph* fg = nullptr;
            if (seeking && SUCCEEDED(seeking->QueryInterface(IID_PPV_ARGS(&fg))) && fg) {
                IPin* eqPin = findEqInputPinFromGraph(fg);
                if (eqPin && originalEqEos) {
                    EosHookScope scope;
                    originalEqEndOfStream(eqPin);
                    recordDiag("EOS_DELIVERED_ASYNC_DRAIN", key, caller, downB, targetB, S_OK);
                    eqPin->Release();
                }
                fg->Release();
            }
        }

        // Stage 2: Audio renderer final output drain
        LONGLONG c = 0, s = 0;
        HRESULT hrCur = seeking ? seeking->GetCurrentPosition(&c) : E_POINTER;
        HRESULT hrStop = seeking ? seeking->GetStopPosition(&s) : E_POINTER;

        bool rendererFinished = false;
        if (SUCCEEDED(hrCur) && SUCCEEDED(hrStop)) {
            if (c >= s && s > 0) {
                rendererFinished = true;
            }
        }
        if (trackerRendererComplete) {
            rendererFinished = true;
        }

        // DirectSound hardware playback cursor & status
        IDirectSoundBuffer* activeDsBuf = reinterpret_cast<IDirectSoundBuffer*>(primaryBuf);
        DWORD dsPlayCursor = 0, dsWriteCursor = 0, dsStatus = 0;
        HRESULT hrDsPos = E_POINTER, hrDsStat = E_POINTER;
        bool hasDs = safeQueryDirectSoundBuffer(activeDsBuf, &dsPlayCursor, &dsWriteCursor, &dsStatus, &hrDsPos, &hrDsStat);

        // Fallback: If DirectSound buffer stopped playing after PCM was delivered, or cursor drained
        if (hasDs && pcmDelivered && !(dsStatus & DSBSTATUS_PLAYING)) {
            rendererFinished = true;
        }

        if (rendererFinished && !trackerRendererComplete) {
            AcquireSRWLockExclusive(&eosLock);
            eosTracker.onRendererOutputComplete(key);
            ReleaseSRWLockExclusive(&eosLock);
        }

        // Periodic progress heartbeat every 500ms
        if (g_diagEnabled && (GetTickCount() - lastHeartbeatTick >= 500)) {
            lastHeartbeatTick = GetTickCount();

            // 1. Position heartbeat: curPos(ms), stopPos(ms), HRESULT of GetCurrentPosition
            recordDiag("DRAIN_HEARTBEAT_POS", key, caller,
                       static_cast<DWORD>(c / 10000), static_cast<DWORD>(s / 10000),
                       hrCur);

            // 2. PCM delivery heartbeat: downstream bytes, target bytes, generation in hr
            recordDiag("DRAIN_HEARTBEAT_PCM", key, caller,
                       downB, targetB,
                       static_cast<HRESULT>(gen));

            // 3. Upstream vs Downstream byte counters: upstream bytes, downstream bytes, completion flags
            recordDiag("DRAIN_HEARTBEAT_BYTES", key, caller,
                       upB, downB,
                       (pcmDelivered ? 0x01 : 0) | (rendererFinished ? 0x02 : 0));

            // 4. DirectSound buffer playback cursor & status
            if (activeDsBuf && hasDs) {
                recordDiag("DRAIN_HEARTBEAT_DSOUND", activeDsBuf, caller,
                           (static_cast<uint64_t>(dsPlayCursor) << 32) | static_cast<uint64_t>(dsWriteCursor),
                           static_cast<uint64_t>(dsStatus),
                           hrDsPos);
            }
        }

        // Must satisfy BOTH Stage 1 (PCM delivered) AND Stage 2 (renderer finished)
        bool completed = pcmDelivered && rendererFinished;
        bool timedOut = (GetTickCount() - startTick >= maxWait);

        if (completed || timedOut) {
            auto status = completed ? eosTransition::PauseDeferralStatus::CompletedDrain
                                    : eosTransition::PauseDeferralStatus::TimedOut;

            if (timedOut) {
                recordDiag("PAUSE_DRAIN_TIMEOUT_STATE", key, caller,
                           (pcmDelivered ? 1 : 0), (rendererFinished ? 1 : 0),
                           hrCur);
                recordDiag("PAUSE_DRAIN_TIMEOUT_PCM_DIFF", key, caller,
                           targetB, downB,
                           targetB > downB ? static_cast<HRESULT>(targetB - downB) : S_OK);
                recordDiag("PAUSE_DRAIN_TIMEOUT_UPSTREAM_DIFF", key, caller,
                           upB, targetB,
                           static_cast<HRESULT>(gen));
                recordDiag("PAUSE_DRAIN_TIMEOUT_SEEK_DIFF", key, caller,
                           static_cast<DWORD>(c / 10000), static_cast<DWORD>(s / 10000),
                           hrCur);
                if (activeDsBuf && hasDs) {
                    recordDiag("PAUSE_DRAIN_TIMEOUT_DSOUND", activeDsBuf, caller,
                               (static_cast<uint64_t>(dsPlayCursor) << 32) | static_cast<uint64_t>(dsWriteCursor),
                               static_cast<uint64_t>(dsStatus),
                               hrDsPos);
                }
            }

            AcquireSRWLockExclusive(&eosLock);
            bool executedOnce = eosTracker.onVirtualPauseCompleted(key, status);
            eosTransition::GraphState::DeferredNextTrackPreparation prep;
            bool hasPrep = false;
            if (executedOnce) {
                hasPrep = eosTracker.popDeferredNextTrackPreparation(key, prep);
            }
            ReleaseSRWLockExclusive(&eosLock);

            if (executedOnce) {
                recordDiag(completed ? "PAUSE_DEFERRED_COMPLETED" : "PAUSE_DEFERRED_TIMED_OUT",
                           key, caller, GetTickCount() - startTick, 0, S_OK);

                if (hasPrep) {
                    void* targetGraphKey = prep.targetGraph ? prep.targetGraph : key;

                    // STEP 1: Apply deferred seek (position 0)
                    if (prep.hasSeek && originalSetPositions) {
                        IMediaSeeking* targetSeek = nullptr;
                        if (prep.targetSeekingMarshalStream) {
                            HRESULT hrM = CoGetInterfaceAndReleaseStream(prep.targetSeekingMarshalStream, IID_IMediaSeeking, reinterpret_cast<void**>(&targetSeek));
                            prep.targetSeekingMarshalStream = nullptr;
                            if (FAILED(hrM)) {
                                recordDiag("DEFERRED_SEEK_UNMARSHAL_FAILED", targetGraphKey, caller, 0, 0, hrM);
                            }
                        } else if (seeking) {
                            targetSeek = seeking;
                            targetSeek->AddRef();
                        }
                        if (targetSeek) {
                            HRESULT hrSeek = originalSetPositions(targetSeek, &prep.seekCurrent, prep.seekCurrentFlags,
                                                                  &prep.seekStop, prep.seekStopFlags);
                            recordDiag("DEFERRED_SETPOSITIONS_EXECUTED", targetGraphKey, caller,
                                       static_cast<DWORD>(prep.seekCurrent / 10000), prep.seekCurrentFlags, hrSeek);
                            targetSeek->Release();
                        }
                    }

                    // STEP 2: Apply deferred playback start (IMediaControl::Run)
                    if (prep.hasRun && originalRun) {
                        IMediaControl* targetControl = nullptr;
                        if (prep.targetControlMarshalStream) {
                            HRESULT hrM = CoGetInterfaceAndReleaseStream(prep.targetControlMarshalStream, IID_IMediaControl, reinterpret_cast<void**>(&targetControl));
                            prep.targetControlMarshalStream = nullptr;
                            if (FAILED(hrM)) {
                                recordDiag("DEFERRED_RUN_UNMARSHAL_FAILED", targetGraphKey, caller, 0, 0, hrM);
                            }
                        }
                        if (targetControl) {
                            HRESULT hrRun = originalRun(targetControl);
                            updateStatus(targetControl, hrRun);
                            recordDiag("DEFERRED_CONTROL_RUN_EXECUTED", targetGraphKey, caller,
                                       prep.transitionGeneration, 0, hrRun);
                            targetControl->Release();
                        }
                    }

                    // STEP 3: Apply deferred DirectSound Play (if separate Play was queued)
                    if (prep.hasDsPlay && originalDsPlay && prep.targetDsBuffer) {
                        HRESULT hrPlay = originalDsPlay(reinterpret_cast<IDirectSoundBuffer*>(prep.targetDsBuffer), prep.dsReserved1, prep.dsPriority, prep.dsFlags);
                        recordDiag("DEFERRED_DS_PLAY_EXECUTED", reinterpret_cast<void*>(prep.targetDsBuffer), caller,
                                   prep.transitionGeneration, prep.dsFlags, hrPlay);
                    }
                }
            }
            break;
        }
    }

    if (seeking) seeking->Release();
    if (p->primaryDsBuffer) {
        reinterpret_cast<IDirectSoundBuffer*>(p->primaryDsBuffer)->Release();
        p->primaryDsBuffer = nullptr;
    }
    if (SUCCEEDED(hrCo)) CoUninitialize();
    delete p;
    return 0;
}

template<class T>
static void startAsyncDrainMonitor(T* self, void* key, void* caller) {
    auto* p = new AsyncDrainParams();
    p->key = key;
    p->caller = caller;
    p->startTick = GetTickCount();
    p->maxWaitMs = 4500;

    IDirectSoundBuffer* dsBuf = nullptr;
    IFilterGraph* fg = nullptr;
    if (self && SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&fg))) && fg) {
        dsBuf = findDirectSoundBufferFromGraph(fg);
        fg->Release();
    }
    if (!dsBuf && g_lastActiveSoundBuffer) {
        dsBuf = reinterpret_cast<IDirectSoundBuffer*>(g_lastActiveSoundBuffer);
        dsBuf->AddRef();
    }
    if (dsBuf) {
        p->primaryDsBuffer = dsBuf;
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.registerActiveSoundBuffer(key, dsBuf);
        ReleaseSRWLockExclusive(&eosLock);
        recordDiag("DRAIN_DSOUND_ATTACHED", key, caller, reinterpret_cast<uint64_t>(dsBuf), 0, S_OK);
    } else {
        p->primaryDsBuffer = nullptr;
    }

    IMediaFilter* mf = nullptr;
    if (self && SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&mf))) && mf) {
        IReferenceClock* clock = nullptr;
        HRESULT hrClock = mf->GetSyncSource(&clock);
        recordDiag("ASYNC_DRAIN_CLOCK_CHECK", key, caller, clock ? 1 : 0, 0, hrClock);
        if (clock) clock->Release();
        mf->Release();
    }

    IMediaSeeking* seeking = nullptr;
    if (self) {
        self->QueryInterface(IID_PPV_ARGS(&seeking));
    }
    if (seeking) {
        IStream* pStream = nullptr;
        HRESULT hrM = CoMarshalInterThreadInterfaceInStream(IID_IMediaSeeking, seeking, &pStream);
        if (SUCCEEDED(hrM) && pStream) {
            p->marshalStream = pStream;
        } else {
            p->rawSeeking = seeking;
            seeking->AddRef();
        }
        seeking->Release();
    }

#ifdef TESTING_HEADLESS
    if (g_simulateThreadCreateFailure) {
        recordDiag("ASYNC_MONITOR_THREAD_CREATE_FAILED", key, caller, ERROR_NOT_ENOUGH_MEMORY, 0, E_FAIL);
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.cancelVirtualPause(key, eosTransition::PauseDeferralStatus::TimedOut);
        ReleaseSRWLockExclusive(&eosLock);
        recordDiag("PAUSE_DEFERRED_ABORT_BY_THREAD_FAILURE", key, caller, 0, 0, S_OK);
        if (p->marshalStream) p->marshalStream->Release();
        if (p->rawSeeking) p->rawSeeking->Release();
        if (p->primaryDsBuffer) {
            reinterpret_cast<IDirectSoundBuffer*>(p->primaryDsBuffer)->Release();
            p->primaryDsBuffer = nullptr;
        }
        delete p;
        return;
    }
#endif

    HANDLE hThread = CreateThread(nullptr, 0, asyncDrainWorkerThread, p, 0, nullptr);
    if (!hThread) {
        recordDiag("ASYNC_MONITOR_THREAD_CREATE_FAILED", key, caller, GetLastError(), 0, E_FAIL);
        // Do NOT block caller synchronously!
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.cancelVirtualPause(key, eosTransition::PauseDeferralStatus::TimedOut);
        ReleaseSRWLockExclusive(&eosLock);
        recordDiag("PAUSE_DEFERRED_ABORT_BY_THREAD_FAILURE", key, caller, 0, 0, S_OK);
        if (p->marshalStream) p->marshalStream->Release();
        if (p->rawSeeking) p->rawSeeking->Release();
        if (p->primaryDsBuffer) {
            reinterpret_cast<IDirectSoundBuffer*>(p->primaryDsBuffer)->Release();
            p->primaryDsBuffer = nullptr;
        }
        delete p;
        return;
    }
    CloseHandle(hThread);
}

static bool handleEosControlRun(IMediaControl* self, void* caller, HRESULT* outHr) {
    if (inEosHook) return false;
    void* key = graphKeyFromObject(self);
    void* activePauseGraphKey = nullptr;
    bool isExplicitManual = false;
#ifdef TESTING_HEADLESS
    if (g_forceManualRun) isExplicitManual = true;
#endif

    AcquireSRWLockExclusive(&eosLock);
    auto classification = eosTracker.classifyRunOperation(key, isExplicitManual,
                                                          g_lastUserInputTick.load(), &activePauseGraphKey);
    uint32_t gen = eosTracker.getTransitionGeneration(key);

    if (classification == eosTransition::Tracker::RunClassification::ManualCancelled) {
        eosTracker.cancelVirtualPause(activePauseGraphKey ? activePauseGraphKey : key, eosTransition::PauseDeferralStatus::CancelledManualPlay);
        ReleaseSRWLockExclusive(&eosLock);
        recordDiag("CONTROL_RUN_MANUAL_PLAY", key, caller, gen, 0, S_OK);
        recordDiag("PAUSE_DEFERRED_CANCELLED", activePauseGraphKey ? activePauseGraphKey : key, caller, static_cast<DWORD>(eosTransition::PauseDeferralStatus::CancelledManualPlay), 0, S_OK);
        recordDiag("DEFERRED_OPERATIONS_PURGED", activePauseGraphKey ? activePauseGraphKey : key, caller, 0, 0, S_OK);
        return false;
    } else if (classification == eosTransition::Tracker::RunClassification::AutomatedDeferred) {
        IStream* marshalStream = nullptr;
        if (self) {
            CoMarshalInterThreadInterfaceInStream(IID_IMediaControl, self, &marshalStream);
        }
        eosTracker.deferAutomaticRun(activePauseGraphKey ? activePauseGraphKey : key, key, self, marshalStream, caller);
        ReleaseSRWLockExclusive(&eosLock);
        recordDiag("DEFERRED_CONTROL_RUN_QUEUED", key, caller, gen, 0, S_OK);
        if (outHr) *outHr = S_OK;
        return true;
    }

    ReleaseSRWLockExclusive(&eosLock);
    recordDiag("CONTROL_RUN", key, caller, gen, (classification == eosTransition::Tracker::RunClassification::OldGraphIgnored) ? 1 : 0, S_OK);
    return false;
}

static void onEosControlRun(IMediaControl* self, void* caller) {
    HRESULT hr = S_OK;
    handleEosControlRun(self, caller, &hr);
}

static HRESULT STDMETHODCALLTYPE controlPauseHook(IMediaControl* self) {
    if (!inEosHook) {
        tryAttachOpcDsPlayLockHooks();
        void* caller = _ReturnAddress();
        void* key = graphKeyFromObject(self);
        bool isKnownCaller = isOpcDsPlayPauseCaller(caller);
        bool pendingFormatChange = false;
        bool deferralActive = false;
        AcquireSRWLockShared(&eosLock);
        const auto* g = eosTracker.findGraph(key);
        deferralActive = g && g->isPauseDeferralActive();
        pendingFormatChange = eosTracker.isFormatChangePending(key);
        ReleaseSRWLockShared(&eosLock);

        recordDiag("BOUNDARY_CONTROL_PAUSE_ENTER", key, caller, isKnownCaller ? 1 : 0, pendingFormatChange ? 1 : 0, S_OK);

        if (g_pauseMode == PauseMode::Observe) {
            recordDiag("CONTROL_PAUSE_OBSERVED", key, caller, isKnownCaller ? 1 : 0, pendingFormatChange ? 1 : 0, S_OK);
            return originalControlPause(self);
        }

        // Reentrant manual pause from user while deferral is active
        if (deferralActive && !isKnownCaller) {
            AcquireSRWLockExclusive(&eosLock);
            eosTracker.cancelFormatChangePause(key, eosTransition::PauseDeferralStatus::CancelledManualPause);
            ReleaseSRWLockExclusive(&eosLock);
            recordDiag("PAUSE_DEFERRED_ABORT_BY_MANUAL_PAUSE", key, caller, 0, 0, S_OK);
            return originalControlPause(self);
        }

        if (isKnownCaller && pendingFormatChange) {
            if (g_pauseMode == PauseMode::AsyncVirtual) {
                AcquireSRWLockExclusive(&eosLock);
                eosTracker.markVirtualPauseActive(key, GetCurrentThreadId(), g_lastActiveSoundBuffer);
                ReleaseSRWLockExclusive(&eosLock);

                recordDiag("PAUSE_ASYNC_VIRTUAL_ENTER", key, caller, 0, 0, S_OK);
                recordDiag("PAUSE_DEFERRED_PENDING", key, caller, 0, 0, S_OK);

                startAsyncDrainMonitor(self, key, caller);

                recordDiag("PAUSE_ASYNC_VIRTUAL_LEAVE_IMMEDIATE", key, caller, 0, 0, S_OK);
                return S_OK;
            }

            auto exitStatus = deferFormatChangePause(self, key, caller, isKnownCaller);
            if (exitStatus == eosTransition::PauseDeferralStatus::CancelledSeek ||
                exitStatus == eosTransition::PauseDeferralStatus::CancelledStop ||
                exitStatus == eosTransition::PauseDeferralStatus::CancelledQuit) {
                recordDiag("CONTROL_PAUSE_BYPASS_CANCEL", key, caller, static_cast<DWORD>(exitStatus), 0, S_OK);
                return S_OK;
            }
        }
    }
    auto hr = originalControlPause(self);
    if (!inEosHook) {
        void* caller = _ReturnAddress();
        void* key = graphKeyFromObject(self);
        recordDiag("BOUNDARY_CONTROL_PAUSE_LEAVE", key, caller, 0, 0, hr);
    }
    return hr;
}

static void onEosFilterRun(IMediaFilter* self, void* caller, REFERENCE_TIME tStart) {
    if (!inEosHook) {
        void* key = graphKeyFromObject(self);
        AcquireSRWLockShared(&eosLock);
        uint32_t gen = eosTracker.getTransitionGeneration(key);
        bool anyVirtual = eosTracker.hasAnyVirtualPauseActive();
        void* activePauseKey = nullptr;
        eosTracker.findActiveVirtualPauseGraph(&activePauseKey);
        ReleaseSRWLockShared(&eosLock);

        recordDiag("FILTER_RUN", key, caller, static_cast<uint64_t>(tStart / 10000), anyVirtual ? 1 : 0, S_OK);
        if (anyVirtual && key != activePauseKey) {
            recordDiag("FILTER_RUN_NEW_GRAPH_DURING_VIRTUAL_PAUSE", key, caller, gen, static_cast<uint64_t>(tStart / 10000), S_OK);
        }
    }
}

static HRESULT STDMETHODCALLTYPE filterPauseHook(IMediaFilter* self) {
    if (!inEosHook) {
        void* caller = _ReturnAddress();
        void* key = graphKeyFromObject(self);
        bool isKnownCaller = isOpcDsPlayPauseCaller(caller);
        bool pendingFormatChange = false;
        bool deferralActive = false;
        AcquireSRWLockShared(&eosLock);
        const auto* g = eosTracker.findGraph(key);
        deferralActive = g && g->isPauseDeferralActive();
        pendingFormatChange = eosTracker.isFormatChangePending(key);
        ReleaseSRWLockShared(&eosLock);

        recordDiag("BOUNDARY_FILTER_PAUSE_ENTER", key, caller, isKnownCaller ? 1 : 0, pendingFormatChange ? 1 : 0, S_OK);

        if (g_pauseMode == PauseMode::Observe) {
            recordDiag("FILTER_PAUSE_OBSERVED", key, caller, isKnownCaller ? 1 : 0, pendingFormatChange ? 1 : 0, S_OK);
            return originalFilterPause(self);
        }

        // Reentrant manual pause from user while deferral is active
        if (deferralActive && !isKnownCaller) {
            AcquireSRWLockExclusive(&eosLock);
            eosTracker.cancelFormatChangePause(key, eosTransition::PauseDeferralStatus::CancelledManualPause);
            ReleaseSRWLockExclusive(&eosLock);
            recordDiag("PAUSE_DEFERRED_ABORT_BY_MANUAL_PAUSE", key, caller, 0, 0, S_OK);
            return originalFilterPause(self);
        }

        if (isKnownCaller && pendingFormatChange) {
            if (g_pauseMode == PauseMode::AsyncVirtual) {
                AcquireSRWLockExclusive(&eosLock);
                eosTracker.markVirtualPauseActive(key, GetCurrentThreadId(), g_lastActiveSoundBuffer);
                ReleaseSRWLockExclusive(&eosLock);

                recordDiag("PAUSE_ASYNC_VIRTUAL_ENTER", key, caller, 0, 0, S_OK);
                recordDiag("PAUSE_DEFERRED_PENDING", key, caller, 0, 0, S_OK);

                startAsyncDrainMonitor(self, key, caller);

                recordDiag("PAUSE_ASYNC_VIRTUAL_LEAVE_IMMEDIATE", key, caller, 0, 0, S_OK);
                return S_OK;
            }

            auto exitStatus = deferFormatChangePause(self, key, caller, isKnownCaller);
            if (exitStatus == eosTransition::PauseDeferralStatus::CancelledSeek ||
                exitStatus == eosTransition::PauseDeferralStatus::CancelledStop ||
                exitStatus == eosTransition::PauseDeferralStatus::CancelledQuit) {
                recordDiag("FILTER_PAUSE_BYPASS_CANCEL", key, caller, static_cast<DWORD>(exitStatus), 0, S_OK);
                return S_OK;
            }
        }

        // Suppress automatic filter pause on old graph while Virtual Pause is active
        bool isVirtualActive = false;
        AcquireSRWLockShared(&eosLock);
        const auto* gV = eosTracker.findGraph(key);
        isVirtualActive = gV && gV->isVirtualPauseActive();
        ReleaseSRWLockShared(&eosLock);
        if (isVirtualActive) {
            bool isManual = false;
            DWORD lastInput = g_lastUserInputTick.load();
            if (lastInput != 0 && (GetTickCount() - lastInput < 400)) {
                isManual = true;
            }
            if (!isManual) {
                recordDiag("FILTER_PAUSE_SUPPRESSED_FOR_VIRTUAL_PAUSE", key, caller, 0, 0, S_OK);
                return S_OK;
            }
        }
    }
    auto hr = originalFilterPause(self);
    if (!inEosHook) {
        void* caller = _ReturnAddress();
        void* key = graphKeyFromObject(self);
        recordDiag("BOUNDARY_FILTER_PAUSE_LEAVE", key, caller, 0, 0, hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE controlStopHook(IMediaControl* self) {
    if (!inEosHook) {
        tryAttachOpcDsPlayLockHooks();
        void* caller = _ReturnAddress();
        void* key = graphKeyFromObject(self);
        bool isKnownCaller = isOpcDsPlayPauseCaller(caller);
        recordDiag("BOUNDARY_CONTROL_STOP_ENTER", key, caller, 0, 0, S_OK);
        AcquireSRWLockExclusive(&eosLock);
        auto* g = eosTracker.findGraph(key);
        if (g && g->isVirtualPauseActive()) {
            bool isManualStop = false;
#ifdef TESTING_HEADLESS
            if (g_forceManualStop) isManualStop = true;
#endif
            DWORD lastInput = g_lastUserInputTick.load();
            if (lastInput != 0 && (GetTickCount() - lastInput < 400)) {
                isManualStop = true;
            }
            if (!isManualStop) {
                ReleaseSRWLockExclusive(&eosLock);
                recordDiag("CONTROL_STOP_SUPPRESSED_FOR_VIRTUAL_PAUSE", key, caller, 0, 0, S_OK);
                return S_OK;
            } else {
                eosTracker.cancelVirtualPause(key, eosTransition::PauseDeferralStatus::CancelledStop);
                recordDiag("PAUSE_DEFERRED_CANCELLED", key, caller, static_cast<DWORD>(eosTransition::PauseDeferralStatus::CancelledStop), 0, S_OK);
                recordDiag("DEFERRED_SETPOSITIONS_PURGED", key, caller, 0, 0, S_OK);
            }
        } else if (g && g->isPauseDeferralActive()) {
            eosTracker.cancelFormatChangePause(key, eosTransition::PauseDeferralStatus::CancelledStop);
            recordDiag("PAUSE_DEFERRED_ABORT_BY_STOP", key, caller, 0, 0, S_OK);
        }
        ReleaseSRWLockExclusive(&eosLock);

        bool pendingFormatChange = false;
        AcquireSRWLockShared(&eosLock);
        pendingFormatChange = eosTracker.isFormatChangePending(key);
        ReleaseSRWLockShared(&eosLock);

        if (pendingFormatChange) {
            recordDiag("EOS_STOP_DEFERRED_START", key, caller, 0, 0, S_OK);
            AcquireSRWLockExclusive(&eosLock);
            eosTracker.markFormatChangeStopDeferred(key);
            ReleaseSRWLockExclusive(&eosLock);

            // 1. Deliver EOS once to EQ if not yet delivered
            IFilterGraph* graph = nullptr;
            if (SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&graph))) && graph) {
                IPin* eqPin = findEqInputPinFromGraph(graph);
                if (eqPin) {
                    bool deliverNow = false;
                    AcquireSRWLockExclusive(&eosLock);
                    const auto* gCheck = eosTracker.findGraph(key);
                    deliverNow = gCheck && !gCheck->eosDelivered && eosTracker.hasDownstreamReachedTarget(key);
                    if (deliverNow) {
                        eosTracker.onDownstreamEndOfStream(key);
                    }
                    ReleaseSRWLockExclusive(&eosLock);
                    if (deliverNow && originalEqEos) {
                        EosHookScope scope;
                        originalEqEndOfStream(eqPin);
                        recordDiag("EOS_DELIVERED_DEFERRED_STOP", key, caller, 0, 1, S_OK);
                    }
                    eqPin->Release();
                }
                graph->Release();
            }

            // 2. Query IMediaSeeking to wait for track end drain
            IMediaSeeking* seeking = nullptr;
            self->QueryInterface(IID_PPV_ARGS(&seeking));
            LONGLONG curPos = 0, stopPos = 0;
            DWORD remainingMs = 0;
            if (seeking && SUCCEEDED(seeking->GetCurrentPosition(&curPos)) &&
                SUCCEEDED(seeking->GetStopPosition(&stopPos)) && stopPos > curPos) {
                remainingMs = static_cast<DWORD>((stopPos - curPos) / 10000);
            } else {
                AcquireSRWLockShared(&eosLock);
                const auto* gCheck = eosTracker.findGraph(key);
                if (gCheck && gCheck->targetBytes > gCheck->downstreamBytes && gCheck->sampleRate > 0) {
                    uint64_t diff = gCheck->targetBytes - gCheck->downstreamBytes;
                    uint32_t align = (gCheck->bitsPerSample / 8) * (gCheck->channels > 0 ? gCheck->channels : 2);
                    if (align > 0) {
                        remainingMs = static_cast<DWORD>((diff * 1000ULL) / (gCheck->sampleRate * align));
                    }
                }
                ReleaseSRWLockShared(&eosLock);
            }

            DWORD startTick = GetTickCount();
            DWORD maxWait = remainingMs > 0 ? (remainingMs + 300) : 3000;
            if (maxWait > 4500) maxWait = 4500; // safety ceiling: max 4.5s

            while (true) {
                LONGLONG c = 0, s = 0;
                if (seeking && SUCCEEDED(seeking->GetCurrentPosition(&c)) &&
                    SUCCEEDED(seeking->GetStopPosition(&s))) {
                    if (c >= s && s > 0) break;
                }
                if (GetTickCount() - startTick >= maxWait) break;

                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                Sleep(15);
            }
            if (seeking) seeking->Release();

            AcquireSRWLockExclusive(&eosLock);
            eosTracker.onFormatChangeCompleted(key);
            ReleaseSRWLockExclusive(&eosLock);
            recordDiag("EOS_STOP_DEFERRED_COMPLETE", key, caller, remainingMs, GetTickCount() - startTick, S_OK);
        }

        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onStop(key);
        ReleaseSRWLockExclusive(&eosLock);
        recordDiag("EOS_RESET_STOP", key, caller, 0, 0, S_OK);
    }
    auto hr = originalControlStop(self);
    if (!inEosHook) {
        void* caller = _ReturnAddress();
        void* key = graphKeyFromObject(self);
        recordDiag("BOUNDARY_CONTROL_STOP_LEAVE", key, caller, 0, 0, hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE filterStopHook(IMediaFilter* self) {
    if (!inEosHook) {
        void* caller = _ReturnAddress();
        void* key = graphKeyFromObject(self);
        recordDiag("BOUNDARY_FILTER_STOP_ENTER", key, caller, 0, 0, S_OK);
        AcquireSRWLockExclusive(&eosLock);
        auto* g = eosTracker.findGraph(key);
        if (g && g->isVirtualPauseActive()) {
            bool isManualStop = false;
#ifdef TESTING_HEADLESS
            if (g_forceManualStop) isManualStop = true;
#endif
            DWORD lastInput = g_lastUserInputTick.load();
            if (lastInput != 0 && (GetTickCount() - lastInput < 400)) {
                isManualStop = true;
            }
            if (!isManualStop) {
                ReleaseSRWLockExclusive(&eosLock);
                recordDiag("FILTER_STOP_SUPPRESSED_FOR_VIRTUAL_PAUSE", key, caller, 0, 0, S_OK);
                return S_OK;
            } else {
                eosTracker.cancelVirtualPause(key, eosTransition::PauseDeferralStatus::CancelledStop);
                recordDiag("PAUSE_DEFERRED_CANCELLED", key, caller, static_cast<DWORD>(eosTransition::PauseDeferralStatus::CancelledStop), 0, S_OK);
                recordDiag("DEFERRED_SETPOSITIONS_PURGED", key, caller, 0, 0, S_OK);
            }
        } else if (g && g->isPauseDeferralActive()) {
            eosTracker.cancelFormatChangePause(key, eosTransition::PauseDeferralStatus::CancelledStop);
            recordDiag("PAUSE_DEFERRED_ABORT_BY_FILTER_STOP", key, caller, 0, 0, S_OK);
        }
        eosTracker.onStop(key);
        ReleaseSRWLockExclusive(&eosLock);
        recordDiag("EOS_RESET_FILTER_STOP", key, caller, 0, 0, S_OK);
    }
    auto hr = originalFilterStop(self);
    if (!inEosHook) {
        void* caller = _ReturnAddress();
        void* key = graphKeyFromObject(self);
        recordDiag("BOUNDARY_FILTER_STOP_LEAVE", key, caller, 0, 0, hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE getStopPositionHook(IMediaSeeking* self, LONGLONG* pStop) {
    auto hr = originalGetStopPosition(self, pStop);
    if (!inEosHook && SUCCEEDED(hr) && pStop) {
        void* key = graphKeyFromObject(self);
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onStopPositionReported(key, *pStop);
        ReleaseSRWLockExclusive(&eosLock);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE getCurrentPositionHook(IMediaSeeking* self, LONGLONG* pCurrent) {
    auto hr = originalGetCurrentPosition(self, pCurrent);
    if (!inEosHook && SUCCEEDED(hr) && pCurrent) {
        void* key = graphKeyFromObject(self);
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onCurrentPositionReported(key, *pCurrent);
        ReleaseSRWLockExclusive(&eosLock);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE setPositionsHook(IMediaSeeking* self, LONGLONG* pCurrent, DWORD dwCurrentFlags,
                                                  LONGLONG* pStop, DWORD dwStopFlags) {
    if (!inEosHook) {
        void* caller = _ReturnAddress();
        const DWORD currPosBits = dwCurrentFlags & AM_SEEKING_PositioningBitsMask;
        const DWORD stopPosBits = dwStopFlags & AM_SEEKING_PositioningBitsMask;
        bool modifiesCurr = (currPosBits != AM_SEEKING_NoPositioning) && (pCurrent != nullptr);
        bool modifiesStop = (stopPosBits != AM_SEEKING_NoPositioning) && (pStop != nullptr);
        if (modifiesCurr || modifiesStop) {
            void* key = graphKeyFromObject(self);
            LONGLONG newPos = modifiesCurr ? *pCurrent : -1;
            LONGLONG newStop = modifiesStop ? *pStop : -1;
            recordDiag("BOUNDARY_SEEK_SETPOSITIONS_ENTER", key, caller, modifiesCurr ? static_cast<DWORD>(newPos / 10000) : 0, dwCurrentFlags, S_OK);
            AcquireSRWLockExclusive(&eosLock);
            void* activePauseGraphKey = nullptr;
            bool isExplicitManual = false;
            bool isExplicitEngine = false;
#ifdef TESTING_HEADLESS
            if (g_forceManualSeek) isExplicitManual = true;
            if (g_forceOpcDsPlayPauseCaller) isExplicitEngine = true;
#endif
            auto classification = eosTracker.classifySeekOperation(key, isExplicitManual, isExplicitEngine,
                                                                   newPos, g_lastUserInputTick.load(), &activePauseGraphKey);
            if (classification == eosTransition::Tracker::SeekClassification::OldGraphAutomatedPark) {
                ReleaseSRWLockExclusive(&eosLock);
                recordDiag("OLD_GRAPH_SETPOSITIONS_PARKED", key, caller, modifiesCurr ? static_cast<DWORD>(newPos / 10000) : 0, dwCurrentFlags, S_OK);
                return S_OK; // Absorbed: do not flush old graph or cancel Virtual Pause
            } else if (classification == eosTransition::Tracker::SeekClassification::AutomatedDeferred) {
                IStream* marshalStream = nullptr;
                if (self) {
                    CoMarshalInterThreadInterfaceInStream(IID_IMediaSeeking, self, &marshalStream);
                }
                eosTracker.deferAutomaticSeek(activePauseGraphKey ? activePauseGraphKey : key, key, self,
                                              newPos, dwCurrentFlags, newStop, dwStopFlags, marshalStream);
                ReleaseSRWLockExclusive(&eosLock);
                recordDiag("DEFERRED_SETPOSITIONS_QUEUED", key, caller, modifiesCurr ? static_cast<DWORD>(newPos / 10000) : 0, dwCurrentFlags, S_OK);
                return S_OK;
            } else if (classification == eosTransition::Tracker::SeekClassification::ManualCancelled) {
                eosTracker.cancelVirtualPause(activePauseGraphKey ? activePauseGraphKey : key, eosTransition::PauseDeferralStatus::CancelledSeek);
                recordDiag("PAUSE_DEFERRED_CANCELLED", key, caller, static_cast<DWORD>(eosTransition::PauseDeferralStatus::CancelledSeek), 0, S_OK);
                recordDiag("DEFERRED_SETPOSITIONS_PURGED", key, caller, 0, 0, S_OK);
            } else {
                auto* g = eosTracker.findGraph(key);
                if (g && g->isPauseDeferralActive()) {
                    eosTracker.cancelFormatChangePause(key, eosTransition::PauseDeferralStatus::CancelledSeek);
                    recordDiag("PAUSE_DEFERRED_ABORT_BY_SEEK", key, caller, modifiesCurr ? static_cast<DWORD>(newPos / 10000) : 0, 0, S_OK);
                }
            }
            eosTracker.onSeekPositions(key, modifiesCurr, newPos, modifiesStop, newStop);
            ReleaseSRWLockExclusive(&eosLock);
            recordDiag("EOS_RESET_SEEK", key, caller, modifiesCurr ? static_cast<DWORD>(newPos / 10000) : 0, dwCurrentFlags, S_OK);
        }
    }
    auto hr = originalSetPositions(self, pCurrent, dwCurrentFlags, pStop, dwStopFlags);
    if (!inEosHook) {
        void* caller = _ReturnAddress();
        void* key = graphKeyFromObject(self);
        recordDiag("BOUNDARY_SEEK_SETPOSITIONS_LEAVE", key, caller, 0, 0, hr);
    }
    return hr;
}

static bool hookPinMethods(IBaseFilter* filter,
                           MemReceiveFn receiveHookFn, MemReceiveFn* origReceiveOut,
                           PinOneFn eosHookFn, PinOneFn* origEosOut,
                           PinOneFn beginFlushHookFn, PinOneFn* origBeginFlushOut,
                           PinOneFn endFlushHookFn, PinOneFn* origEndFlushOut,
                           PinSegmentFn newSegmentHookFn, PinSegmentFn* origNewSegmentOut) {
    IEnumPins* e = nullptr;
    if (FAILED(filter->EnumPins(&e))) return false;
    bool hooked = false;
    IPin* p = nullptr;
    while (e->Next(1, &p, nullptr) == S_OK) {
        PIN_DIRECTION dir{};
        if (SUCCEEDED(p->QueryDirection(&dir)) && dir == PINDIR_INPUT) {
            auto pinVtable = *reinterpret_cast<void***>(p);
            HMODULE pinned = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(pinVtable[14]), &pinned);
            bool ok = MH_CreateHook(pinVtable[14], reinterpret_cast<void*>(eosHookFn), reinterpret_cast<void**>(origEosOut)) == MH_OK &&
                      MH_CreateHook(pinVtable[15], reinterpret_cast<void*>(beginFlushHookFn), reinterpret_cast<void**>(origBeginFlushOut)) == MH_OK &&
                      MH_CreateHook(pinVtable[16], reinterpret_cast<void*>(endFlushHookFn), reinterpret_cast<void**>(origEndFlushOut)) == MH_OK &&
                      MH_CreateHook(pinVtable[17], reinterpret_cast<void*>(newSegmentHookFn), reinterpret_cast<void**>(origNewSegmentOut)) == MH_OK;
            IMemInputPin* mem = nullptr;
            if (ok && SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&mem)))) {
                auto memVtable = *reinterpret_cast<void***>(mem);
                ok = MH_CreateHook(memVtable[6], reinterpret_cast<void*>(receiveHookFn), reinterpret_cast<void**>(origReceiveOut)) == MH_OK;
                mem->Release();
            } else {
                ok = false;
            }
            hooked = ok;
            p->Release();
            break;
        }
        p->Release();
    }
    e->Release();
    return hooked;
}

static IBaseFilter* createSonyFilterInstance(const wchar_t* dllName, const wchar_t* clsidStr) {
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\%s", dllName);
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return nullptr;
    CLSID clsid{};
    if (FAILED(CLSIDFromString(clsidStr, &clsid))) return nullptr;
    auto getClassObj = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
        GetProcAddress(module, "DllGetClassObject"));
    if (!getClassObj) return nullptr;
    IClassFactory* factory = nullptr;
    if (FAILED(getClassObj(clsid, IID_PPV_ARGS(&factory))) || !factory) return nullptr;
    IBaseFilter* filter = nullptr;
    factory->CreateInstance(nullptr, IID_PPV_ARGS(&filter));
    factory->Release();
    return filter;
}

static bool setupEosTransitionHooks(IGraphBuilder* graph, IMediaControl* control, IMediaFilter* mediaFilter) {
    if (!graph || !control || !mediaFilter) return false;
    initPauseMode();
    record(g_pauseMode == PauseMode::Observe ? "PAUSE_MODE_OBSERVE" : (g_pauseMode == PauseMode::AsyncVirtual ? "PAUSE_MODE_ASYNC" : "PAUSE_MODE_DEFER"), 0, 0, S_OK);
    auto controlVtable = *reinterpret_cast<void***>(control);
    auto filterVtable = *reinterpret_cast<void***>(mediaFilter);
    bool ok = MH_CreateHook(controlVtable[8], reinterpret_cast<void*>(controlPauseHook), reinterpret_cast<void**>(&originalControlPause)) == MH_OK &&
              MH_CreateHook(controlVtable[9], reinterpret_cast<void*>(controlStopHook), reinterpret_cast<void**>(&originalControlStop)) == MH_OK &&
              MH_CreateHook(filterVtable[4], reinterpret_cast<void*>(filterStopHook), reinterpret_cast<void**>(&originalFilterStop)) == MH_OK &&
              MH_CreateHook(filterVtable[5], reinterpret_cast<void*>(filterPauseHook), reinterpret_cast<void**>(&originalFilterPause)) == MH_OK;
    IMediaSeeking* seeking = nullptr;
    bool seekingOk = false;
    if (ok && SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&seeking)))) {
        auto seekVtable = *reinterpret_cast<void***>(seeking);
        seekingOk = MH_CreateHook(seekVtable[11], reinterpret_cast<void*>(getStopPositionHook), reinterpret_cast<void**>(&originalGetStopPosition)) == MH_OK &&
                    MH_CreateHook(seekVtable[12], reinterpret_cast<void*>(getCurrentPositionHook), reinterpret_cast<void**>(&originalGetCurrentPosition)) == MH_OK &&
                    MH_CreateHook(seekVtable[14], reinterpret_cast<void*>(setPositionsHook), reinterpret_cast<void**>(&originalSetPositions)) == MH_OK;
        seeking->Release();
    }
    if (!ok || !seekingOk) return false;

    eosDiag::getHookStatusReporter().report("DirectShow.ControlRun", ok ? "OK" : "FAILED", ok ? S_OK : E_FAIL);
    eosDiag::getHookStatusReporter().report("DirectShow.ControlPause", ok ? "OK" : "FAILED", ok ? S_OK : E_FAIL);
    eosDiag::getHookStatusReporter().report("DirectShow.ControlStop", ok ? "OK" : "FAILED", ok ? S_OK : E_FAIL);
    eosDiag::getHookStatusReporter().report("DirectShow.FilterRun", ok ? "OK" : "FAILED", ok ? S_OK : E_FAIL);
    eosDiag::getHookStatusReporter().report("DirectShow.FilterPause", ok ? "OK" : "FAILED", ok ? S_OK : E_FAIL);
    eosDiag::getHookStatusReporter().report("DirectShow.FilterStop", ok ? "OK" : "FAILED", ok ? S_OK : E_FAIL);
    eosDiag::getHookStatusReporter().report("DirectShow.Seeking", seekingOk ? "OK" : "FAILED", seekingOk ? S_OK : E_FAIL);

    if (g_diagEnabled) {
        eosDiag::getModuleTracker().refresh();
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (!hUser32) hUser32 = LoadLibraryW(L"user32.dll");
        if (hUser32) {
            auto pDisp = GetProcAddress(hUser32, "DispatchMessageW");
            if (pDisp && !originalDispatchMessageW) {
                auto mhDisp = MH_CreateHook(pDisp, reinterpret_cast<void*>(dispatchMessageWHook), reinterpret_cast<void**>(&originalDispatchMessageW));
                eosDiag::getHookStatusReporter().report("User32.DispatchMessageW", mhDisp == MH_OK ? "OK" : "FAILED", mhDisp == MH_OK ? S_OK : E_FAIL);
            }
        }
        if (g_lockDiagEnabled) {
            tryAttachOpcDsPlayLockHooks();
        } else {
            eosDiag::getHookStatusReporter().report("Engine.OpcDSPlay.IAT.EnterCriticalSection", "DISABLED_BY_CONFIG", S_OK);
            eosDiag::getHookStatusReporter().report("Engine.OpcDSPlay.IAT.LeaveCriticalSection", "DISABLED_BY_CONFIG", S_OK);
        }
    }

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
            MH_CreateHook(bufV[12], reinterpret_cast<void*>(dsPlayHook), reinterpret_cast<void**>(&originalDsPlay));
            MH_CreateHook(bufV[13], reinterpret_cast<void*>(dsSetCurrentPositionHook), reinterpret_cast<void**>(&originalDsSetCurrentPosition));
            MH_CreateHook(bufV[15], reinterpret_cast<void*>(dsSetVolumeHook), reinterpret_cast<void**>(&originalDsSetVolume));
            MH_CreateHook(bufV[18], reinterpret_cast<void*>(dsStopHook), reinterpret_cast<void**>(&originalDsStop));
            buf->Release();
        }
        ds8->Release();
    }

    IBaseFilter* seamless = createSonyFilterInstance(L"SeamlessFilter.ax", L"{6AF73F1F-1266-46E6-9A36-EE75E4BA10BB}");
    if (!seamless) return false;
    ok = hookPinMethods(seamless,
                        seamlessReceiveHook, &originalSeamlessReceive,
                        seamlessEosHook, &originalSeamlessEos,
                        seamlessBeginFlushHook, &originalSeamlessBeginFlush,
                        seamlessEndFlushHook, &originalSeamlessEndFlush,
                        seamlessNewSegmentHook, &originalSeamlessNewSegment);
    seamless->Release();
    if (!ok) return false;

    IBaseFilter* eq = createSonyFilterInstance(L"OmgEqxlFilter.ax", L"{5C968488-5E35-4CFE-98A1-A51C838797DB}");
    if (!eq) return false;
    ok = hookPinMethods(eq,
                        eqReceiveHook, &originalEqReceive,
                        eqEosHook, &originalEqEos,
                        eqBeginFlushHook, &originalEqBeginFlush,
                        eqEndFlushHook, &originalEqEndFlush,
                        eqNewSegmentHook, &originalEqNewSegment);
    eq->Release();
    return ok;
}
