#pragma once
// DSEE Unified 24-bit hooks for Issue #42 and #43.
// Included only when EXPERIMENT_DSEE_UNIFIED is defined.
#include "dsee_unified_config.h"
#include "dsee_unified_route.h"

namespace dseeUnified {

using DseeSetter = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, DseeConfig);
static DseeSetter originalDsee = nullptr;
static HarmonizeMode g_harmonizeMode = HarmonizeMode::Target96k;
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

inline void recordGraphInspection(const GraphInspection& insp, uint32_t gen) {
    record("GRAPH_GEN", gen, insp.isDirectSeamless ? 1 : 0, S_OK);
    record("GRAPH_DECODE_PCM", insp.decodePcm.sampleRate, insp.decodePcm.bitDepth, HRESULT(insp.decodePcm.channels));
    if (insp.dseeIn.present) {
        record("GRAPH_DSEE_IN", insp.dseeIn.sampleRate, insp.dseeIn.bitDepth, HRESULT(insp.dseeIn.channels));
        record("GRAPH_DSEE_OUT", insp.dseeOut.sampleRate, insp.dseeOut.bitDepth, HRESULT(insp.dseeOut.channels));
    } else {
        record("GRAPH_DSEE_ABSENT", gen, 0, S_FALSE);
    }
    record("GRAPH_SEAMLESS_IN", insp.seamlessIn.sampleRate, insp.seamlessIn.bitDepth, HRESULT(insp.seamlessIn.channels));
    record("GRAPH_SEAMLESS_OUT", insp.seamlessOut.sampleRate, insp.seamlessOut.bitDepth, HRESULT(insp.seamlessOut.channels));
    if (insp.eqIn.present && insp.eqIn.connected) {
        record("GRAPH_EQ_IN", insp.eqIn.sampleRate, insp.eqIn.bitDepth, HRESULT(insp.eqIn.channels));
        record("GRAPH_EQ_OUT", insp.eqOut.sampleRate, insp.eqOut.bitDepth, HRESULT(insp.eqOut.channels));
    } else if (insp.eqIn.present && !insp.eqIn.connected) {
        record("GRAPH_EQ_BYPASSED", gen, 0, S_OK);
    } else {
        record("GRAPH_EQ_ABSENT", gen, 0, S_OK);
    }
    record("GRAPH_RENDER_IN", insp.renderIn.sampleRate, insp.renderIn.bitDepth, HRESULT(insp.renderIn.channels));
    DWORD peerCode = insp.isDirectSeamless ? 1 : (insp.isDirectDsee ? 2 : (insp.eqBypassed ? 3 : 0));
    record("GRAPH_RENDER_PEER", peerCode, 0, S_OK);

    // Issue #54: Unified multi-stage format chain for this track generation
    // Parameters: requested = (decodeBits << 16) | dseeInBits, applied = (dseeOutBits << 16) | renderInBits
    record("GRAPH_FORMAT_CHAIN", (DWORD(insp.decodePcm.bitDepth) << 16) | DWORD(insp.dseeIn.bitDepth),
           (DWORD(insp.dseeOut.bitDepth) << 16) | DWORD(insp.renderIn.bitDepth), S_OK);

    // Issue #46 Diagnostics: Peer Code 3 & Full Filter Chain Traversal
    if (!InlineIsEqualGUID(insp.renderPeerClsid, GUID_NULL)) {
        record("GRAPH_PEER_CLSID_1", insp.renderPeerClsid.Data1, (DWORD(insp.renderPeerClsid.Data2) << 16) | insp.renderPeerClsid.Data3, S_OK);
        record("GRAPH_PEER_CLSID_2", *reinterpret_cast<const DWORD*>(&insp.renderPeerClsid.Data4[0]), *reinterpret_cast<const DWORD*>(&insp.renderPeerClsid.Data4[4]), S_OK);
    }
    for (uint8_t i = 0; i < insp.chainLength && i < MAX_CHAIN_NODES; ++i) {
        record("GRAPH_CHAIN_NODE", i, insp.chain[i].inRate, HRESULT(insp.chain[i].outRate));
        record("GRAPH_CHAIN_BITS", i, insp.chain[i].inBits, HRESULT(insp.chain[i].outBits));
        record("GRAPH_CHAIN_CLSID", i, insp.chain[i].clsid.Data1, S_OK);
    }
}

static HRESULT STDMETHODCALLTYPE dseeUnifiedSetterHook(IUnknown* self, DseeConfig config) {
    void* caller = _ReturnAddress();
    CodecType codec = identifyCallerCodec(caller);
    g_harmonizeMode = initHarmonizeModeFromEnv();

    record("DSEE_REQ_IN", config.inputRate, config.inputBits, HRESULT(config.mode));
    record("DSEE_REQ_OUT", config.outputRate, config.outputBits, HRESULT(codec));

    bool dsSelected = bridgeStatus::directSoundSelected();
    bool dseeRequested = bridgeStatus::dseeSharedRequested();

    // Hi-Res input (>= 88.2k) or non-DirectSound: pass through untouched to preserve bit-perfect Hi-Res
    if (!dsSelected || isHiResRate(config.inputRate)) {
        auto hr = originalDsee(self, config);
        rememberSource(self, false);
        record("DSEE_PASS_THROUGH", config.outputRate, config.outputBits, hr);
        return hr;
    }

    // Issue #56: If UI DSEE setting is OFF or incoming request is mode == 0,
    // do NOT accept initial mode=1 and do NOT inherit stale upsampled formats (e.g. 96k/24b).
    // Reset outputRate and outputBits to match inputRate and inputBits (bit-perfect bypass).
    if (!dseeRequested || config.mode == 0) {
        DseeConfig offConfig = makeOffResetDseeConfig(config);
        auto hr = originalDsee(self, offConfig);
        rememberSource(self, false);
        record("DSEE_OFF_RESET", offConfig.outputRate, offConfig.outputBits, hr);
        return hr;
    }

    bool eligible = (config.mode == 1) &&
                    isTargetInputRate(config.inputRate) &&
                    isTargetInputBits(config.inputBits) &&
                    (config.channels == 2);

    if (!eligible) {
        auto hr = originalDsee(self, config);
        rememberSource(self, false);
        record("DSEE_PASS_THROUGH", config.outputRate, config.outputBits, hr);
        return hr;
    }

    WORD decBits = (codec == CodecType::Flac) ? getActiveDecoderBits().load() : static_cast<WORD>(config.inputBits);
    if (codec != CodecType::Flac) {
        resetActiveDecoderBits();
    }
    DseeConfig adjusted{};
    if (!computeUnifiedDseeConfig(config, adjusted, g_harmonizeMode, decBits)) {
        auto hr = originalDsee(self, config);
        rememberSource(self, false);
        record("DSEE_PASS_THROUGH", config.outputRate, config.outputBits, hr);
        return hr;
    }

    if (config.inputBits != adjusted.inputBits) {
        record("DSEE_IN_BITS_SYNC", config.inputBits, adjusted.inputBits, S_OK);
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

    // Issue #56: Synchronize EQ bypass and restoration with UI DSEE setting.
    // When DSEE is ON in UI, apply EQ bypass for 24-bit stream.
    // When DSEE is OFF in UI, restore standard EQ routing (seamOut -> eqIn -> eqOut -> dsIn).
    bool dseeActive = bridgeStatus::dseeSharedRequested();
    auto routeRes = applyEqBypass(graph.Get(), dseeActive);
    const char* eventName = (routeRes.outcome == RouteOutcome::Bypassed) ? "DSEE_EQ_BYPASS" :
                            (routeRes.outcome == RouteOutcome::AlreadyBypassed) ? "DSEE_EQ_ALREADY_BYPASSED" :
                            (routeRes.outcome == RouteOutcome::Restored) ? "DSEE_EQ_RESTORED" :
                            (routeRes.outcome == RouteOutcome::RestoreFailed) ? "DSEE_EQ_RESTORE_FAILED" :
                            "DSEE_EQ_SKIPPED";
    record(eventName, routeRes.outputRate, routeRes.outputBits, routeRes.change);

#ifdef EXPERIMENT_EOS_TRANSITION
    bool bypassed = (routeRes.outcome == RouteOutcome::Bypassed || routeRes.outcome == RouteOutcome::AlreadyBypassed);
    onEosEqBypassed(graph.Get(), bypassed);
#endif

    DWORD trackedRate = routeRes.outputRate;
    WORD trackedBits = routeRes.outputBits;

    // Inspect and record negotiated audio formats on all 5 graph stages for this generation
    GraphInspection insp{};
    bool inspected = inspectGraphFormats(graph.Get(), insp);

    if (trackedRate == 0 && inspected) {
        trackedRate = insp.renderIn.sampleRate;
        trackedBits = insp.renderIn.bitDepth;
    }

    // Track graph lifecycle: recreated vs reused
    if (trackedRate > 0) {
        auto trans = g_graphTracker.onGraphEvent(graph.Get(), trackedRate, trackedBits, CodecType::Unknown);
        if (trans == GraphTransitionType::Recreated) {
            record("GRAPH_RECREATED", trackedRate, trackedBits, S_OK);
        } else if (trans == GraphTransitionType::ReusedSameFormat) {
            record("GRAPH_REUSED", trackedRate, trackedBits, S_OK);
        } else if (trans == GraphTransitionType::ReusedFormatSwitched) {
            record("GRAPH_REUSED", trackedRate, trackedBits, S_FALSE);
        }
    }

    if (inspected) {
        recordGraphInspection(insp, g_graphTracker.generation);
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
