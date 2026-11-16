// Version-specific x86 runtime setting override used by the production bridge. No file patches.
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <intrin.h>
#include <cstdio>
#include <map>
#include <wrl/client.h>
#include "MinHook.h"
#include "bridge_status.h"
#include "bridge_startup.h"
#include "dsee_unified_config.h"
#include "dsee_unified_route.h"
#include "sync_tail_deferral.h"

using Microsoft::WRL::ComPtr;

using Setter = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, DWORD);
static Setter original = nullptr;

using DseeSetter = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, dseeUnified::DseeConfig);
static DseeSetter originalDsee = nullptr;

using Run = HRESULT(STDMETHODCALLTYPE*)(IMediaControl*);
using FilterRun = HRESULT(STDMETHODCALLTYPE*)(IMediaFilter*, REFERENCE_TIME);
static Run originalRun = nullptr;
static FilterRun originalFilterRun = nullptr;

using Pause = HRESULT(STDMETHODCALLTYPE*)(IMediaControl*);
using FilterPause = HRESULT(STDMETHODCALLTYPE*)(IMediaFilter*);
static Pause originalPause = nullptr;
static FilterPause originalFilterPause = nullptr;

static bool supportedFlacRate(DWORD rate) {
    return rate == 48000 || rate == 96000 || rate == 192000;
}

static HANDLE output = INVALID_HANDLE_VALUE;
static SRWLOCK mutex = SRWLOCK_INIT;
static HINSTANCE ownModule = nullptr;
static SRWLOCK sourcesLock = SRWLOCK_INIT;
static std::map<IBaseFilter*, bool> appliedSources;

static void record(const char* event, DWORD requested, DWORD applied, HRESULT hr) {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char line[512];
    auto n = sprintf_s(line, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\t%lu\t%lu\t%s\trequested=%lu applied=%lu hr=%08lx\r\n",
                       t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                       GetCurrentProcessId(), GetCurrentThreadId(), event, requested, applied, ULONG(hr));
    AcquireSRWLockExclusive(&mutex);
    DWORD written;
    if (n > 0 && output != INVALID_HANDLE_VALUE) {
        WriteFile(output, line, n, &written, nullptr);
    }
    ReleaseSRWLockExclusive(&mutex);
}

static void rememberSource(IUnknown* self, bool applied) {
    IBaseFilter* f = nullptr;
    if (SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&f)))) {
        AcquireSRWLockExclusive(&sourcesLock);
        if (appliedSources.size() >= 64 && appliedSources.find(f) == appliedSources.end()) {
            appliedSources.erase(appliedSources.begin());
        }
        appliedSources[f] = applied;
        ReleaseSRWLockExclusive(&sourcesLock);
        f->Release();
    }
}

static bool sourceApplied(IBaseFilter* f) {
    CLSID cls{}, flac;
    CLSIDFromString(L"{028A95AA-C956-4874-AD10-C657DB1B00F4}", &flac);
    if (FAILED(f->GetClassID(&cls))) return false;
    if (cls != flac) return false;
    AcquireSRWLockShared(&sourcesLock);
    auto it = appliedSources.find(f);
    bool applied = it != appliedSources.end() && it->second;
    ReleaseSRWLockShared(&sourcesLock);
    return applied;
}

// OpcFlac uses distinct calls for initial playback and seamless next-track preparation.
static bool knownCaller(void* returnAddress) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(returnAddress), &m)) return false;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(m, path, MAX_PATH);
    auto name = wcsrchr(path, L'\\');
    auto base = reinterpret_cast<ULONG_PTR>(m);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return _wcsicmp(name ? name + 1 : path, L"OpcFlac.dll") == 0 &&
           nt->FileHeader.TimeDateStamp == 0x63EB12E0 &&
           (reinterpret_cast<ULONG_PTR>(returnAddress) - base == 0x32A85 ||
            reinterpret_cast<ULONG_PTR>(returnAddress) - base == 0x31E7A);
}

static bool isNativeSupported24(IUnknown* self, DWORD* outRate = nullptr) {
    IBaseFilter* f = nullptr;
    if (FAILED(self->QueryInterface(IID_PPV_ARGS(&f)))) return false;
    IEnumPins* pins = nullptr;
    bool found = false;
    if (SUCCEEDED(f->EnumPins(&pins))) {
        IPin* p = nullptr;
        while (pins->Next(1, &p, nullptr) == S_OK) {
            PIN_DIRECTION direction;
            p->QueryDirection(&direction);
            IEnumMediaTypes* types = nullptr;
            if (direction == PINDIR_OUTPUT && SUCCEEDED(p->EnumMediaTypes(&types))) {
                AM_MEDIA_TYPE* mt = nullptr;
                while (types->Next(1, &mt, nullptr) == S_OK) {
                    if (mt->majortype == MEDIATYPE_Audio && mt->subtype == MEDIASUBTYPE_PCM &&
                        mt->formattype == FORMAT_WaveFormatEx && mt->cbFormat >= 16 && mt->pbFormat) {
                        WAVEFORMATEX wave{};
                        memcpy(&wave, mt->pbFormat, min(size_t(mt->cbFormat), sizeof(wave)));
                        if (wave.wBitsPerSample == 24 && wave.nChannels == 2) {
                            if (outRate) *outRate = wave.nSamplesPerSec;
                            found = true;
                        }
                    }
                    if (mt->cbFormat) CoTaskMemFree(mt->pbFormat);
                    if (mt->pUnk) mt->pUnk->Release();
                    CoTaskMemFree(mt);
                    if (found) break;
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

// FLAC 24-bit Output Setter Hook (OpcFlac.dll slot setter)
static HRESULT STDMETHODCALLTYPE setterHook(IUnknown* self, DWORD value) {
    auto caller = _ReturnAddress();
    bool known = knownCaller(caller);
    DWORD nativeRate = 0;
    bool isNative24 = isNativeSupported24(self, &nativeRate);
    bool dseeActive = bridgeStatus::dseeSharedRequested();
    bool rateEligible = isNative24 && dseeUnified::isFlacRateEligible24(nativeRate, dseeActive);
    bool eligible = (value == 16) && known && bridgeStatus::directSoundSelected() && rateEligible;

    dseeUnified::setActiveDecoderBits(eligible ? 24 : 16);

    if (!eligible) {
        auto hr = original(self, value);
        rememberSource(self, false);
        record("PASS_THROUGH", nativeRate ? nativeRate : value, value, hr);
        return hr;
    }

    auto hr = original(self, 24);
    record("SET_24", nativeRate ? nativeRate : value, 24, hr);
    rememberSource(self, SUCCEEDED(hr));
    if (FAILED(hr)) {
        auto fallback = original(self, value);
        record("SET_16_FALLBACK", nativeRate ? nativeRate : value, value, fallback);
        dseeUnified::setActiveDecoderBits(16);
        return fallback;
    }
    return hr;
}

// DSEE HX Configuration Setter Hook (OmgDseeHxFilter.ax slot 3)
static HRESULT STDMETHODCALLTYPE dseeSetterHook(IUnknown* self, dseeUnified::DseeConfig config) {
    void* caller = _ReturnAddress();
    dseeUnified::CodecType codec = dseeUnified::identifyCallerCodec(caller);

    record("DSEE_REQ_IN", config.inputRate, config.inputBits, HRESULT(config.mode));
    record("DSEE_REQ_OUT", config.outputRate, config.outputBits, HRESULT(codec));

    bool dsSelected = bridgeStatus::directSoundSelected();
    bool dseeRequested = bridgeStatus::dseeSharedRequested();

    // Hi-Res input (>= 88.2k) or non-DirectSound: pass through untouched to preserve bit-perfect Hi-Res
    if (!dsSelected || dseeUnified::isHiResRate(config.inputRate)) {
        auto hr = originalDsee(self, config);
        rememberSource(self, false);
        record("DSEE_PASS_THROUGH", config.outputRate, config.outputBits, hr);
        return hr;
    }

    // Issue #56: If UI DSEE setting is OFF or incoming request is mode == 0,
    // do NOT accept initial mode=1 and do NOT inherit stale upsampled formats (e.g. 96k/24b).
    // Reset outputRate and outputBits to match inputRate and inputBits (bit-perfect bypass).
    if (!dseeRequested || config.mode == 0) {
        dseeUnified::DseeConfig offConfig = dseeUnified::makeOffResetDseeConfig(config);
        auto hr = originalDsee(self, offConfig);
        rememberSource(self, false);
        record("DSEE_OFF_RESET", offConfig.outputRate, offConfig.outputBits, hr);
        return hr;
    }

    bool eligible = (config.mode == 1) &&
                    dseeUnified::isTargetInputRate(config.inputRate) &&
                    dseeUnified::isTargetInputBits(config.inputBits) &&
                    (config.channels == 2);

    if (!eligible) {
        auto hr = originalDsee(self, config);
        rememberSource(self, false);
        record("DSEE_PASS_THROUGH", config.outputRate, config.outputBits, hr);
        return hr;
    }

    WORD decBits = (codec == dseeUnified::CodecType::Flac) ? dseeUnified::getActiveDecoderBits().load() : static_cast<WORD>(config.inputBits);
    if (codec != dseeUnified::CodecType::Flac) {
        dseeUnified::resetActiveDecoderBits();
    }

    dseeUnified::HarmonizeMode mode = dseeUnified::initHarmonizeMode();
    dseeUnified::DseeConfig adjusted{};
    if (!dseeUnified::computeUnifiedDseeConfig(config, adjusted, mode, decBits)) {
        auto hr = originalDsee(self, config);
        rememberSource(self, false);
        record("DSEE_PASS_THROUGH", config.outputRate, config.outputBits, hr);
        return hr;
    }

    auto hr = originalDsee(self, adjusted);
    record(mode == dseeUnified::HarmonizeMode::Family4x ? "DSEE_UNIFIED_SET_4X" : "DSEE_UNIFIED_SET_2X",
           adjusted.outputRate, adjusted.outputBits, hr);

    if (FAILED(hr)) {
        auto fallback = originalDsee(self, config);
        rememberSource(self, false);
        record("DSEE_FALLBACK", config.outputRate, config.outputBits, fallback);
        return fallback;
    }

    rememberSource(self, true);
    return hr;
}

// DirectShow Graph Preparation: Apply EQ bypass for DSEE ON, or restore EQ routing for DSEE OFF
static HRESULT prepareGraph(IUnknown* object) {
    if (!bridgeStatus::directSoundSelected() || !object) return S_OK;

    ComPtr<IGraphBuilder> graph;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&graph)))) return S_OK;

    bool dseeActive = bridgeStatus::dseeSharedRequested();
    auto routeRes = dseeUnified::applyEqBypass(graph.Get(), dseeActive);
    const char* eventName = (routeRes.outcome == dseeUnified::RouteOutcome::Bypassed) ? "DSEE_EQ_BYPASS" :
                            (routeRes.outcome == dseeUnified::RouteOutcome::AlreadyBypassed) ? "DSEE_EQ_ALREADY_BYPASSED" :
                            (routeRes.outcome == dseeUnified::RouteOutcome::Restored) ? "DSEE_EQ_RESTORED" :
                            (routeRes.outcome == dseeUnified::RouteOutcome::RestoreFailed) ? "DSEE_EQ_RESTORE_FAILED" :
                            "DSEE_EQ_SKIPPED";
    record(eventName, routeRes.outputRate, routeRes.outputBits, routeRes.change);

    return S_OK;
}

static bridgeStatus::Format pcmFormat(const AM_MEDIA_TYPE& type) {
    if (type.majortype != MEDIATYPE_Audio || type.formattype != FORMAT_WaveFormatEx ||
        type.cbFormat < 16 || !type.pbFormat) return {};
    WAVEFORMATEX wave{};
    memcpy(&wave, type.pbFormat, min(size_t(type.cbFormat), sizeof(wave)));
    return {wave.nSamplesPerSec, wave.wBitsPerSample, wave.nChannels};
}

static void freeMediaType(AM_MEDIA_TYPE& type) {
    CoTaskMemFree(type.pbFormat);
    if (type.pUnk) type.pUnk->Release();
}

static const wchar_t* fileCodec(IBaseFilter* filter) {
    ComPtr<IFileSourceFilter> source;
    if (FAILED(filter->QueryInterface(IID_PPV_ARGS(&source)))) return L"";
    LPOLESTR path = nullptr;
    AM_MEDIA_TYPE type{};
    const auto hr = source->GetCurFile(&path, &type);
    const wchar_t* codec = L"";
    if (SUCCEEDED(hr) && path) {
        if (const auto ext = wcsrchr(path, L'.')) {
            if (!_wcsicmp(ext, L".flac")) codec = L"FLAC";
            else if (!_wcsicmp(ext, L".wav")) codec = L"WAV";
            else if (!_wcsicmp(ext, L".mp3")) codec = L"MP3";
            else if (!_wcsicmp(ext, L".aac")) codec = L"AAC";
            else if (!_wcsicmp(ext, L".alac")) codec = L"ALAC";
            else if (!_wcsicmp(ext, L".m4a") || !_wcsicmp(ext, L".mp4")) codec = L"M4A";
        }
    }
    CoTaskMemFree(path);
    freeMediaType(type);
    return codec;
}

static void updateStatus(IUnknown* object, HRESULT result) {
    bridgeStatus::Playback playback;
    bool source = false, output24 = false, dseeActive = false;
    ComPtr<IFilterGraph> graph;
    ComPtr<IEnumFilters> filters;

    if (SUCCEEDED(result) && bridgeStatus::directSoundSelected() &&
        SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&graph))) && SUCCEEDED(graph->EnumFilters(&filters))) {
        playback.route = bridgeStatus::Route::Normal;
        ComPtr<IBaseFilter> filter;
        while (filters->Next(1, filter.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
            CLSID cls{};
            if (FAILED(filter->GetClassID(&cls))) continue;
            if (sourceApplied(filter.Get())) source = true;
            if (!*playback.codec) playback.codec = fileCodec(filter.Get());

            if (cls == CLSID_DSoundRender) {
                ComPtr<IEnumPins> pins;
                if (SUCCEEDED(filter->EnumPins(&pins))) {
                    ComPtr<IPin> pin;
                    while (pins->Next(1, pin.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
                        AM_MEDIA_TYPE type{};
                        if (SUCCEEDED(pin->ConnectionMediaType(&type))) {
                            playback.output = pcmFormat(type);
                            const auto format = playback.output;
                            output24 = (format.bits == 24 && format.channels == 2 &&
                                        (format.rate == 48000 || format.rate == 88200 ||
                                         format.rate == 96000 || format.rate == 192000));
                            freeMediaType(type);
                        }
                    }
                }
            }
        }

        dseeUnified::GraphInspection insp{};
        if (dseeUnified::inspectGraphFormats(reinterpret_cast<IGraphBuilder*>(graph.Get()), insp)) {
            if (insp.dseeIn.sampleRate > 0) {
                playback.input = {insp.dseeIn.sampleRate, insp.dseeIn.bitDepth, insp.dseeIn.channels};
            } else if (insp.decodePcm.sampleRate > 0) {
                playback.input = {insp.decodePcm.sampleRate, insp.decodePcm.bitDepth, insp.decodePcm.channels};
            }

            dseeActive = dseeUnified::hasDseeOutput(insp);
        }

        if (dseeActive) {
            playback.route = bridgeStatus::Route::Dsee;
        } else if (source && output24) {
            playback.route = bridgeStatus::Route::Flac24;
            if (!*playback.codec) playback.codec = L"FLAC";
            if (!playback.input.rate) playback.input = playback.output;
        }
    }

    bridgeStatus::publish(playback);
    const auto next = ((source && output24) || dseeActive) ? bridgeStatus::State::On : bridgeStatus::State::Off;
    bridgeStatus::state.store(next);
    record(next == bridgeStatus::State::On ? "STATUS_ON" : "STATUS_OFF", 0, 0, result);
}

static HRESULT STDMETHODCALLTYPE runHook(IMediaControl* self) {
    prepareGraph(self);
    auto hr = originalRun(self);
    updateStatus(self, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE filterRunHook(IMediaFilter* self, REFERENCE_TIME time) {
    prepareGraph(self);
    auto hr = originalFilterRun(self, time);
    updateStatus(self, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE pauseHook(IMediaControl* self) {
    syncTailDeferral::drainTrackTailSynchronous(self);
    return originalPause(self);
}

static HRESULT STDMETHODCALLTYPE filterPauseHook(IMediaFilter* self) {
    syncTailDeferral::drainTrackTailSynchronous(self);
    return originalFilterPause(self);
}

static bool setupRunAndPauseHooks() {
    IGraphBuilder* graph = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph)))) return false;
    bool ok = false;
    IMediaControl* c = nullptr;
    IMediaFilter* f = nullptr;

    if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&c))) && SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&f)))) {
        auto runAddr = (*reinterpret_cast<void***>(c))[7];
        auto filterRunAddr = (*reinterpret_cast<void***>(f))[6];
        auto pauseAddr = (*reinterpret_cast<void***>(c))[8];
        auto filterPauseAddr = (*reinterpret_cast<void***>(f))[5];

        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(runAddr), &pinned);

        ok = (MH_CreateHook(runAddr, reinterpret_cast<void*>(runHook), reinterpret_cast<void**>(&originalRun)) == MH_OK) &&
             (MH_CreateHook(filterRunAddr, reinterpret_cast<void*>(filterRunHook), reinterpret_cast<void**>(&originalFilterRun)) == MH_OK) &&
             (MH_CreateHook(pauseAddr, reinterpret_cast<void*>(pauseHook), reinterpret_cast<void**>(&originalPause)) == MH_OK) &&
             (MH_CreateHook(filterPauseAddr, reinterpret_cast<void*>(filterPauseHook), reinterpret_cast<void**>(&originalFilterPause)) == MH_OK);
    }

    if (c) c->Release();
    if (f) f->Release();
    graph->Release();
    return ok;
}

static bool setupDseeHook() {
    auto module = LoadLibraryExW(LR"(C:\Program Files (x86)\Sony\Music Center\Sony.Earth\OmgDseeHxFilter.ax)",
                                nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return false;

    auto get = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(GetProcAddress(module, "DllGetClassObject"));
    ComPtr<IClassFactory> factory;
    ComPtr<IBaseFilter> filter;
    ComPtr<IUnknown> control;
    IID iid{};
    CLSIDFromString(L"{6AF76DBE-8634-4FD3-93ED-982EDE1BFDD5}", &iid);

    if (!get || FAILED(get(dseeUnified::dseeId, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&filter))) ||
        FAILED(filter->QueryInterface(iid, reinterpret_cast<void**>(control.GetAddressOf())))) {
        return false;
    }

    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto fn = (*reinterpret_cast<void***>(control.Get()))[3];

    return nt->FileHeader.TimeDateStamp == 0x63eb12c1 &&
           reinterpret_cast<BYTE*>(fn) - base == 0x36ca &&
           MH_CreateHook(fn, reinterpret_cast<void*>(dseeSetterHook), reinterpret_cast<void**>(&originalDsee)) == MH_OK;
}

static DWORD WINAPI start(void*) {
    bridgeStartup::Signal startup;
#ifndef _M_IX86
    return 2;
#else
    wchar_t temp[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    swprintf_s(path, L"%sFlac24Override-%lu.tsv", temp, GetCurrentProcessId());
    output = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return 1;
    record("START", 0, 0, S_OK);

    auto co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CLSID clsid;
    CLSIDFromString(L"{028A95AA-C956-4874-AD10-C657DB1B00F4}", &clsid);
    IID iid;
    CLSIDFromString(L"{1609A1C0-E0E7-4CEF-AA37-31CD15BE4ECD}", &iid);

    HMODULE module = LoadLibraryExW(L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\OmgFlacSrc.ax", nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    IClassFactory* factory = nullptr;
    IUnknown* source = nullptr;
    HRESULT hr = E_FAIL;
    if (module) {
        auto get = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(GetProcAddress(module, "DllGetClassObject"));
        if (get) hr = get(clsid, IID_PPV_ARGS(&factory));
        if (factory) {
            hr = factory->CreateInstance(nullptr, iid, reinterpret_cast<void**>(&source));
            factory->Release();
        }
    }

    if (source) {
        auto v = *reinterpret_cast<void***>(source);
        auto base = reinterpret_cast<ULONG_PTR>(module);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        bool known = nt->FileHeader.TimeDateStamp == 0x63EB12C3 &&
                     reinterpret_cast<ULONG_PTR>(v[7]) - base == 0x1860;
        if (known && MH_Initialize() == MH_OK) {
            auto status = MH_CreateHook(v[7], reinterpret_cast<void*>(setterHook), reinterpret_cast<void**>(&original));
            record("CREATE_HOOK", 0, 0, HRESULT(status));

            bool dseeOk = setupDseeHook();
            if (!dseeOk) record("DSEE_UNAVAILABLE", 0, 0, S_FALSE);

            bool ready = (status == MH_OK) && setupRunAndPauseHooks();
            if (ready) {
                HMODULE pinned = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                   reinterpret_cast<LPCWSTR>(v[7]), &pinned);
                status = MH_EnableHook(MH_ALL_HOOKS);
            } else {
                status = MH_ERROR_NOT_CREATED;
            }
            hr = (status == MH_OK) ? S_OK : E_FAIL;
        } else {
            hr = E_NOINTERFACE;
        }
        source->Release();
    }

    if (SUCCEEDED(co)) CoUninitialize();
    record(SUCCEEDED(hr) ? "READY" : "FAILED", 0, 0, hr);
    bridgeStatus::state.store(SUCCEEDED(hr) ? bridgeStatus::State::Ready : bridgeStatus::State::Off);
    startup.complete(SUCCEEDED(hr));
    bridgeStatus::runUi(ownModule);
    return SUCCEEDED(hr) ? 0 : 2;
#endif
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ownModule = module;
        DisableThreadLibraryCalls(module);
        auto t = CreateThread(nullptr, 0, start, nullptr, 0, nullptr);
        if (!t) return FALSE;
        CloseHandle(t);
    }
    return TRUE;
}
