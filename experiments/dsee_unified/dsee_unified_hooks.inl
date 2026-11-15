#pragma once
// DSEE Unified 24-bit hooks for Issue #42.
// Included only when EXPERIMENT_DSEE_UNIFIED is defined.
#include "dsee_unified_config.h"
#include "dsee_unified_route.h"

namespace dseeUnified {

using DseeSetter = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, DseeConfig);
static DseeSetter originalDsee = nullptr;
static HarmonizeMode g_harmonizeMode = HarmonizeMode::Family2x;
static GraphTracker g_graphTracker;
static thread_local bool g_inPrepareGraph = false;

inline void setHarmonizeMode(HarmonizeMode mode) {
    g_harmonizeMode = mode;
}

inline HarmonizeMode getHarmonizeMode() {
    return g_harmonizeMode;
}

inline GraphTracker& getGraphTracker() {
    return g_graphTracker;
}

static HRESULT STDMETHODCALLTYPE dseeUnifiedSetterHook(IUnknown* self, DseeConfig config) {
    void* caller = _ReturnAddress();
    CodecType codec = identifyCallerCodec(caller);

    record("DSEE_REQ_IN", config.inputRate, config.inputBits, HRESULT(config.mode));
    record("DSEE_REQ_OUT", config.outputRate, config.outputBits, HRESULT(codec));

    bool eligible = bridgeStatus::directSoundSelected() &&
                    (config.mode == 1) &&
                    isTargetInputRate(config.inputRate) &&
                    isTargetInputBits(config.inputBits) &&
                    (config.channels == 2);

    if (!eligible) {
        // DSEE HX OFF (mode == 0) or Hi-Res rate (>= 88.2k): pass through untouched to preserve bit-perfect Hi-Res
        auto hr = originalDsee(self, config);
        rememberSource(self, false);
        record("DSEE_PASS_THROUGH", config.outputRate, config.outputBits, hr);
        return hr;
    }

    DseeConfig adjusted{};
    if (!computeUnifiedDseeConfig(config, adjusted, g_harmonizeMode)) {
        auto hr = originalDsee(self, config);
        rememberSource(self, false);
        record("DSEE_PASS_THROUGH", config.outputRate, config.outputBits, hr);
        return hr;
    }

    auto hr = originalDsee(self, adjusted);
    record("DSEE_UNIFIED_SET", adjusted.outputRate, adjusted.outputBits, hr);

    if (FAILED(hr)) {
        auto fallback = originalDsee(self, config);
        rememberSource(self, false);
        record("DSEE_FALLBACK", config.outputRate, config.outputBits, fallback);
        return fallback;
    }

    rememberSource(self, true);
    return hr;
}

static HRESULT prepareDseeGraph(IUnknown* object) {
    if (!bridgeStatus::directSoundSelected() || !object) return S_OK;

    ComPtr<IGraphBuilder> graph;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&graph)))) return S_OK;

    // Apply EQ bypass so 24-bit stream can reach DirectSound cleanly without EQ rejection
    auto routeRes = applyEqBypass(graph.Get());
    const char* eventName = (routeRes.outcome == RouteOutcome::Bypassed) ? "DSEE_EQ_BYPASS" :
                            (routeRes.outcome == RouteOutcome::AlreadyBypassed) ? "DSEE_EQ_ALREADY_BYPASSED" :
                            (routeRes.outcome == RouteOutcome::Restored) ? "DSEE_EQ_RESTORED" :
                            (routeRes.outcome == RouteOutcome::RestoreFailed) ? "DSEE_EQ_RESTORE_FAILED" :
                            "DSEE_EQ_SKIPPED";
    record(eventName, routeRes.outputRate, routeRes.outputBits, routeRes.change);

    // Track graph lifecycle: recreated vs reused
    if (routeRes.outputRate > 0) {
        auto trans = g_graphTracker.onGraphEvent(graph.Get(), routeRes.outputRate, routeRes.outputBits, CodecType::Unknown);
        if (trans == GraphTransitionType::Recreated) {
            record("GRAPH_RECREATED", routeRes.outputRate, routeRes.outputBits, S_OK);
        } else if (trans == GraphTransitionType::ReusedSameFormat) {
            record("GRAPH_REUSED", routeRes.outputRate, routeRes.outputBits, S_OK);
        } else if (trans == GraphTransitionType::ReusedFormatSwitched) {
            record("GRAPH_REUSED", routeRes.outputRate, routeRes.outputBits, S_FALSE);
        }
    }

    return S_OK;
}

struct DseeScope {
    bool outer;
    DseeScope() : outer(!g_inPrepareGraph) { g_inPrepareGraph = true; }
    ~DseeScope() { if (outer) g_inPrepareGraph = false; }
};

static bool setupDseeUnifiedHook() {
    HMODULE module = LoadLibraryExW(L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\OmgDseeHxFilter.ax",
                                   nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return false;

    auto get = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
        GetProcAddress(module, "DllGetClassObject"));
    if (!get) return false;

    ComPtr<IClassFactory> factory;
    ComPtr<IUnknown> control;
    IID iid;
    CLSIDFromString(L"{6AF76DBE-8634-4FD3-93ED-982EDE1BFDD5}", &iid);

    if (FAILED(get(dseeId, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateInstance(nullptr, iid, reinterpret_cast<void**>(control.GetAddressOf())))) {
        return false;
    }

    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto fn = (*reinterpret_cast<void***>(control.Get()))[3];

    // Verify known PE header timestamp and slot 3 function offset
    if (nt->FileHeader.TimeDateStamp != 0x63EB12C1 || reinterpret_cast<BYTE*>(fn) - base != 0x36CA) {
        return false;
    }

    auto status = MH_CreateHook(fn, reinterpret_cast<void*>(dseeUnifiedSetterHook),
                                reinterpret_cast<void**>(&originalDsee));
    record("CREATE_DSEE_UNIFIED_HOOK", 0, 0, HRESULT(status));

    if (status == MH_OK) {
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(fn), &pinned);
    }
    return status == MH_OK;
}

} // namespace dseeUnified
