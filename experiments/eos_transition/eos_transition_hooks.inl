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

using PinOneFn = HRESULT(STDMETHODCALLTYPE*)(IPin*);
using PinSegmentFn = HRESULT(STDMETHODCALLTYPE*)(IPin*, REFERENCE_TIME, REFERENCE_TIME, double);
using MemReceiveFn = HRESULT(STDMETHODCALLTYPE*)(IMemInputPin*, IMediaSample*);
using ControlStopFn = HRESULT(STDMETHODCALLTYPE*)(IMediaControl*);
using FilterStopFn = HRESULT(STDMETHODCALLTYPE*)(IMediaFilter*);
using SetPositionsFn = HRESULT(STDMETHODCALLTYPE*)(IMediaSeeking*, LONGLONG*, DWORD, LONGLONG*, DWORD);

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

static ControlStopFn originalControlStop = nullptr;
static FilterStopFn originalFilterStop = nullptr;
static SetPositionsFn originalSetPositions = nullptr;

static HRESULT STDMETHODCALLTYPE originalEqEndOfStream(IPin* pin) {
    return originalEqEos ? originalEqEos(pin) : S_OK;
}

static IPin* findEqInputPinInGraph(IPin* seamlessPin) {
    if (!seamlessPin) return nullptr;
    PIN_INFO pi{};
    if (FAILED(seamlessPin->QueryPinInfo(&pi)) || !pi.pFilter) return nullptr;
    FILTER_INFO fi{};
    HRESULT hr = pi.pFilter->QueryFilterInfo(&fi);
    pi.pFilter->Release();
    if (FAILED(hr) || !fi.pGraph) return nullptr;

    CLSID eqClsid{};
    CLSIDFromString(L"{5C968488-5E35-4CFE-98A1-A51C838797DB}", &eqClsid);
    IPin* foundPin = nullptr;
    IEnumFilters* filters = nullptr;
    if (SUCCEEDED(fi.pGraph->EnumFilters(&filters))) {
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
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onBeginFlush(key);
        ReleaseSRWLockExclusive(&eosLock);
        record("EOS_RESET_BEGIN_FLUSH", 0, 0, S_OK);
    }
    return originalSeamlessBeginFlush(self);
}

static HRESULT STDMETHODCALLTYPE seamlessEndFlushHook(IPin* self) {
    if (!inEosHook) {
        void* key = graphKeyFromPin(self);
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onEndFlush(key);
        ReleaseSRWLockExclusive(&eosLock);
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
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onBeginFlush(key);
        ReleaseSRWLockExclusive(&eosLock);
    }
    return originalEqBeginFlush(self);
}

static HRESULT STDMETHODCALLTYPE eqEndFlushHook(IPin* self) {
    if (!inEosHook) {
        void* key = graphKeyFromPin(self);
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onEndFlush(key);
        ReleaseSRWLockExclusive(&eosLock);
    }
    return originalEqEndFlush(self);
}

static HRESULT STDMETHODCALLTYPE eqNewSegmentHook(IPin* self, REFERENCE_TIME tStart, REFERENCE_TIME tStop, double rate) {
    return originalEqNewSegment(self, tStart, tStop, rate);
}

static HRESULT STDMETHODCALLTYPE controlStopHook(IMediaControl* self) {
    if (!inEosHook) {
        void* key = graphKeyFromObject(self);
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onStop(key);
        ReleaseSRWLockExclusive(&eosLock);
        record("EOS_RESET_STOP", 0, 0, S_OK);
    }
    return originalControlStop(self);
}

static HRESULT STDMETHODCALLTYPE filterStopHook(IMediaFilter* self) {
    if (!inEosHook) {
        void* key = graphKeyFromObject(self);
        AcquireSRWLockExclusive(&eosLock);
        eosTracker.onStop(key);
        ReleaseSRWLockExclusive(&eosLock);
        record("EOS_RESET_FILTER_STOP", 0, 0, S_OK);
    }
    return originalFilterStop(self);
}

static HRESULT STDMETHODCALLTYPE setPositionsHook(IMediaSeeking* self, LONGLONG* pCurrent, DWORD dwCurrentFlags,
                                                  LONGLONG* pStop, DWORD dwStopFlags) {
    if (!inEosHook) {
        bool modifiesPos = (dwCurrentFlags & AM_SEEKING_PositioningBitsMask) != AM_SEEKING_NoPositioning ||
                           (dwStopFlags & AM_SEEKING_PositioningBitsMask) != AM_SEEKING_NoPositioning;
        if (modifiesPos) {
            void* key = graphKeyFromObject(self);
            AcquireSRWLockExclusive(&eosLock);
            eosTracker.onSeek(key);
            ReleaseSRWLockExclusive(&eosLock);
            record("EOS_RESET_SEEK", pCurrent ? static_cast<DWORD>(*pCurrent / 10000) : 0, dwCurrentFlags, S_OK);
        }
    }
    return originalSetPositions(self, pCurrent, dwCurrentFlags, pStop, dwStopFlags);
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
    auto controlVtable = *reinterpret_cast<void***>(control);
    auto filterVtable = *reinterpret_cast<void***>(mediaFilter);
    bool ok = MH_CreateHook(controlVtable[9], reinterpret_cast<void*>(controlStopHook), reinterpret_cast<void**>(&originalControlStop)) == MH_OK &&
              MH_CreateHook(filterVtable[4], reinterpret_cast<void*>(filterStopHook), reinterpret_cast<void**>(&originalFilterStop)) == MH_OK;
    IMediaSeeking* seeking = nullptr;
    if (ok && SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&seeking)))) {
        auto seekVtable = *reinterpret_cast<void***>(seeking);
        ok = MH_CreateHook(seekVtable[11], reinterpret_cast<void*>(setPositionsHook), reinterpret_cast<void**>(&originalSetPositions)) == MH_OK;
        seeking->Release();
    } else {
        ok = false;
    }
    if (!ok) return false;

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
