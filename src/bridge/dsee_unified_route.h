#pragma once
#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include "dsee_unified_config.h"

namespace dseeUnified {

using Microsoft::WRL::ComPtr;

inline const CLSID dseeId = {0xa481d160, 0xab4d, 0x4b97, {0x93, 0xfe, 0xc1, 0xf4, 0xb4, 0xb7, 0x81, 0xdf}};
inline const CLSID seamlessId = {0x6af73f1f, 0x1266, 0x46e6, {0x9a, 0x36, 0xee, 0x75, 0xe4, 0xba, 0x10, 0xbb}};
inline const CLSID eqId = {0x5c968488, 0x5e35, 0x4cfe, {0x98, 0xa1, 0xa5, 0x1c, 0x83, 0x87, 0x97, 0xdb}};
inline const CLSID flacId = {0x028a95aa, 0xc956, 0x4874, {0xad, 0x10, 0xc6, 0x57, 0xdb, 0x1b, 0x00, 0xf4}};
inline const CLSID wavParserId = {0xd9ce8cdc, 0xb4e2, 0x45b6, {0x8e, 0xe9, 0x09, 0xde, 0x2b, 0x40, 0x6e, 0x60}};

struct RouteType {
    AM_MEDIA_TYPE value{};
    RouteType() = default;
    RouteType(const RouteType&) = delete;
    RouteType& operator=(const RouteType&) = delete;
    ~RouteType() {
        if (value.cbFormat) CoTaskMemFree(value.pbFormat);
        if (value.pUnk) value.pUnk->Release();
    }
};

struct RouteEdge {
    ComPtr<IPin> out;
    ComPtr<IPin> in;
    RouteType type;
};

enum class RouteOutcome {
    Skipped,
    AlreadyBypassed,
    Bypassed,
    Restored,
    RestoreFailed
};

struct RouteResult {
    RouteOutcome outcome = RouteOutcome::Skipped;
    HRESULT change = S_FALSE;
    HRESULT restore = S_OK;
    DWORD outputRate = 0;
    WORD outputBits = 0;
    WORD outputChannels = 0;
};

struct FormatStage {
    bool present = false;
    bool connected = false;
    DWORD sampleRate = 0;
    WORD bitDepth = 0;
    WORD channels = 0;
};

struct FilterChainNode {
    CLSID clsid{};
    wchar_t filterName[64]{};
    wchar_t inPinName[32]{};
    wchar_t outPinName[32]{};
    DWORD inRate = 0;
    WORD inBits = 0;
    DWORD outRate = 0;
    WORD outBits = 0;
};

static constexpr size_t MAX_CHAIN_NODES = 8;

struct GraphInspection {
    FormatStage decodePcm;
    FormatStage dseeIn;
    FormatStage dseeOut;
    FormatStage seamlessIn;
    FormatStage seamlessOut;
    FormatStage eqIn;
    FormatStage eqOut;
    FormatStage renderIn;
    bool eqBypassed = false;
    bool isDirectSeamless = false;
    bool isDirectDsee = false;

    // Issue #46 Diagnostics: Peer Code 3 & Full Filter Chain Traversal
    CLSID renderPeerClsid{};
    wchar_t renderPeerFilterName[64]{};
    wchar_t renderPeerPinName[32]{};
    uint8_t chainLength = 0;
    FilterChainNode chain[MAX_CHAIN_NODES]{};
};

inline bool hasDsee2xOutput(const GraphInspection& graph) {
    const auto& input = graph.dseeIn;
    const auto& output = graph.dseeOut;
    const auto& renderer = graph.renderIn;
    return graph.eqBypassed && input.connected && output.connected && renderer.connected &&
           isTargetInputRate(input.sampleRate) && isTargetInputBits(input.bitDepth) &&
           input.channels == 2 && output.channels == 2 && renderer.channels == 2 &&
           output.sampleRate == input.sampleRate * 2 && renderer.sampleRate == output.sampleRate &&
           output.bitDepth == 24 && renderer.bitDepth == 24;
}

inline bool hasDsee4xOutput(const GraphInspection& graph) {
    const auto& input = graph.dseeIn;
    const auto& output = graph.dseeOut;
    const auto& renderer = graph.renderIn;
    return graph.eqBypassed && input.connected && output.connected && renderer.connected &&
           isTargetInputRate(input.sampleRate) && isTargetInputBits(input.bitDepth) &&
           input.channels == 2 && output.channels == 2 && renderer.channels == 2 &&
           output.sampleRate == input.sampleRate * 4 && renderer.sampleRate == output.sampleRate &&
           output.bitDepth == 24 && renderer.bitDepth == 24;
}

inline bool hasDseeOutput(const GraphInspection& graph) {
    return hasDsee2xOutput(graph) || hasDsee4xOutput(graph);
}

enum class DseeRouteKind {
    None,       // Negotiation failed / unbypassed / DSEE OFF
    Dsee2x,     // 44.1k -> 88.2k or 48k -> 96k (24-bit stereo)
    Dsee4x,     // 44.1k -> 176.4k or 48k -> 192k (24-bit stereo)
    NativeHiRes // Native 96k/192k direct passthrough
};

inline DseeRouteKind classifyDseeRoute(const GraphInspection& graph) {
    if (hasDsee2xOutput(graph)) return DseeRouteKind::Dsee2x;
    if (hasDsee4xOutput(graph)) return DseeRouteKind::Dsee4x;
    if (graph.renderIn.sampleRate >= 88200 &&
        (!graph.dseeIn.connected || graph.dseeIn.sampleRate >= 88200)) {
        return DseeRouteKind::NativeHiRes;
    }
    return DseeRouteKind::None;
}

inline ComPtr<IPin> findUniquePin(IBaseFilter* filter, PIN_DIRECTION wanted) {
    if (!filter) return {};
    ComPtr<IEnumPins> pins;
    if (FAILED(filter->EnumPins(&pins))) return {};
    ComPtr<IPin> p, found;
    while (pins->Next(1, &p, nullptr) == S_OK) {
        PIN_DIRECTION d;
        if (SUCCEEDED(p->QueryDirection(&d)) && d == wanted) {
            if (found) return {};
            found = p;
        }
        p.Reset();
    }
    return found;
}

inline bool readPinAudioFormat(IPin* pin, FormatStage& stage) {
    if (!pin) return false;
    stage.present = true;
    ComPtr<IPin> peer;
    if (SUCCEEDED(pin->ConnectedTo(&peer)) && peer) {
        stage.connected = true;
    }
    AM_MEDIA_TYPE mt{};
    if (SUCCEEDED(pin->ConnectionMediaType(&mt))) {
        if (mt.pbFormat && mt.cbFormat >= sizeof(WAVEFORMATEX)) {
            auto* w = reinterpret_cast<WAVEFORMATEX*>(mt.pbFormat);
            stage.sampleRate = w->nSamplesPerSec;
            stage.bitDepth = w->wBitsPerSample;
            stage.channels = w->nChannels;
        }
        if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
        if (mt.pUnk) mt.pUnk->Release();
        return stage.connected;
    }
    return false;
}

// Inspects negotiated audio formats across all DirectShow graph stages:
// Decode PCM -> DSEE In/Out -> Seamless In/Out -> EQ In/Out -> DirectSound In
// Also traces full linear filter chain and identifies DirectSound peer (Issue #46).
inline bool inspectGraphFormats(IGraphBuilder* graph, GraphInspection& out) {
    if (!graph) return false;
    out = GraphInspection{};

    ComPtr<IEnumFilters> enumFilters;
    if (FAILED(graph->EnumFilters(&enumFilters)) || !enumFilters) return false;

    ComPtr<IBaseFilter> decoderFilter, dseeFilter, seamFilter, eqFilter, dsFilter;
    ComPtr<IBaseFilter> f;
    while (enumFilters->Next(1, &f, nullptr) == S_OK) {
        CLSID id{};
        if (SUCCEEDED(f->GetClassID(&id))) {
            if (id == dseeId) dseeFilter = f;
            else if (id == seamlessId) seamFilter = f;
            else if (id == eqId) eqFilter = f;
            else if (id == CLSID_DSoundRender) dsFilter = f;
            else if (id == flacId || id == wavParserId) decoderFilter = f;
            else if (!decoderFilter) {
                // If not identified by known CLSID, check if output pin provides PCM audio
                auto outPin = findUniquePin(f.Get(), PINDIR_OUTPUT);
                FormatStage probe{};
                if (readPinAudioFormat(outPin.Get(), probe) && probe.sampleRate > 0) {
                    decoderFilter = f;
                }
            }
        }
        f.Reset();
    }

    if (decoderFilter) {
        auto outPin = findUniquePin(decoderFilter.Get(), PINDIR_OUTPUT);
        readPinAudioFormat(outPin.Get(), out.decodePcm);
    }
    if (dseeFilter) {
        auto inPin = findUniquePin(dseeFilter.Get(), PINDIR_INPUT);
        auto outPin = findUniquePin(dseeFilter.Get(), PINDIR_OUTPUT);
        readPinAudioFormat(inPin.Get(), out.dseeIn);
        readPinAudioFormat(outPin.Get(), out.dseeOut);
    }
    if (seamFilter) {
        auto inPin = findUniquePin(seamFilter.Get(), PINDIR_INPUT);
        auto outPin = findUniquePin(seamFilter.Get(), PINDIR_OUTPUT);
        readPinAudioFormat(inPin.Get(), out.seamlessIn);
        readPinAudioFormat(outPin.Get(), out.seamlessOut);
    }
    if (eqFilter) {
        auto inPin = findUniquePin(eqFilter.Get(), PINDIR_INPUT);
        auto outPin = findUniquePin(eqFilter.Get(), PINDIR_OUTPUT);
        readPinAudioFormat(inPin.Get(), out.eqIn);
        readPinAudioFormat(outPin.Get(), out.eqOut);
    }
    if (dsFilter) {
        auto inPin = findUniquePin(dsFilter.Get(), PINDIR_INPUT);
        readPinAudioFormat(inPin.Get(), out.renderIn);
        if (inPin) {
            ComPtr<IPin> currentPeer;
            if (SUCCEEDED(inPin->ConnectedTo(&currentPeer)) && currentPeer) {
                PIN_INFO pi{};
                if (SUCCEEDED(currentPeer->QueryPinInfo(&pi))) {
                    wcsncpy_s(out.renderPeerPinName, pi.achName, _TRUNCATE);
                    if (pi.pFilter) {
                        pi.pFilter->GetClassID(&out.renderPeerClsid);
                        FILTER_INFO fi{};
                        if (SUCCEEDED(pi.pFilter->QueryFilterInfo(&fi))) {
                            wcsncpy_s(out.renderPeerFilterName, fi.achName, _TRUNCATE);
                            if (fi.pGraph) fi.pGraph->Release();
                        }
                        pi.pFilter->Release();
                    }
                }
                auto seamOut = seamFilter ? findUniquePin(seamFilter.Get(), PINDIR_OUTPUT) : nullptr;
                auto dseeOut = dseeFilter ? findUniquePin(dseeFilter.Get(), PINDIR_OUTPUT) : nullptr;
                auto eqOut = eqFilter ? findUniquePin(eqFilter.Get(), PINDIR_OUTPUT) : nullptr;
                if (seamOut && currentPeer.Get() == seamOut.Get()) {
                    out.isDirectSeamless = true;
                    out.eqBypassed = true;
                } else if (dseeOut && currentPeer.Get() == dseeOut.Get()) {
                    out.isDirectDsee = true;
                    out.eqBypassed = true;
                } else if (eqOut && currentPeer.Get() == eqOut.Get()) {
                    out.eqBypassed = false;
                } else {
                    out.eqBypassed = true; // Peer Code 3 (other filter)
                }
            }
        }
    }
    if (!out.eqIn.connected) {
        out.eqBypassed = true;
    }

    // Traverse linear chain from decoderFilter to DirectSound (Issue #46)
    if (decoderFilter) {
        ComPtr<IBaseFilter> curr = decoderFilter;
        while (curr && out.chainLength < MAX_CHAIN_NODES) {
            auto& node = out.chain[out.chainLength];
            curr->GetClassID(&node.clsid);
            FILTER_INFO fi{};
            if (SUCCEEDED(curr->QueryFilterInfo(&fi))) {
                wcsncpy_s(node.filterName, fi.achName, _TRUNCATE);
                if (fi.pGraph) fi.pGraph->Release();
            }
            auto inPin = findUniquePin(curr.Get(), PINDIR_INPUT);
            if (inPin) {
                FormatStage st{};
                readPinAudioFormat(inPin.Get(), st);
                node.inRate = st.sampleRate;
                node.inBits = st.bitDepth;
                PIN_INFO pinfo{};
                if (SUCCEEDED(inPin->QueryPinInfo(&pinfo))) {
                    wcsncpy_s(node.inPinName, pinfo.achName, _TRUNCATE);
                    if (pinfo.pFilter) pinfo.pFilter->Release();
                }
            }
            auto outPin = findUniquePin(curr.Get(), PINDIR_OUTPUT);
            if (outPin) {
                FormatStage st{};
                readPinAudioFormat(outPin.Get(), st);
                node.outRate = st.sampleRate;
                node.outBits = st.bitDepth;
                PIN_INFO pinfo{};
                if (SUCCEEDED(outPin->QueryPinInfo(&pinfo))) {
                    wcsncpy_s(node.outPinName, pinfo.achName, _TRUNCATE);
                    if (pinfo.pFilter) pinfo.pFilter->Release();
                }
                out.chainLength++;
                ComPtr<IPin> nextPin;
                if (SUCCEEDED(outPin->ConnectedTo(&nextPin)) && nextPin) {
                    PIN_INFO nextInfo{};
                    if (SUCCEEDED(nextPin->QueryPinInfo(&nextInfo)) && nextInfo.pFilter) {
                        curr.Attach(nextInfo.pFilter);
                    } else {
                        curr = nullptr;
                    }
                } else {
                    curr = nullptr;
                }
            } else {
                out.chainLength++;
                curr = nullptr;
            }
        }
    }

    return true;
}

// Bypasses OmgEqxlFilter (EQ) and any intermediate downsampler so the highest-fidelity upstream
// output connects directly to DirectSoundRender.
// Supports:
// 1. DSEE active (rate >= 88.2k):
//    - If SeamlessFilter passes full rate (e.g. 96k in 48k FLAC), connects seamOut -> dsIn.
//    - Otherwise (e.g. 88.2k in 44.1k FLAC where Seamless/downsampler rejects/downsamples 88.2k),
//      connects dseeOut -> dsIn directly, maintaining 88.2kHz/24bit (Issue #46).
// 2. DSEE OFF or native Hi-Res: preserves seamless transitions via seamOut -> dsIn.
// Issue #56: Supports explicit dseeActive parameter. When DSEE HX is OFF (dseeActive == false),
// restores standard EQ routing (seamOut -> eqIn, eqOut -> dsIn) if previously bypassed.
inline RouteResult applyEqBypass(IGraphBuilder* graph, bool dseeActive = true) {
    RouteResult result;
    if (!graph) return result;

    ComPtr<IBaseFilter> decoderFilter, dseeFilter, seamFilter, eqFilter, dsFilter;
    ComPtr<IEnumFilters> enumFilters;
    if (FAILED(graph->EnumFilters(&enumFilters)) || !enumFilters) return result;

    ComPtr<IBaseFilter> f;
    while (enumFilters->Next(1, &f, nullptr) == S_OK) {
        CLSID id{};
        if (SUCCEEDED(f->GetClassID(&id))) {
            if (id == dseeId) dseeFilter = f;
            else if (id == seamlessId) seamFilter = f;
            else if (id == eqId) eqFilter = f;
            else if (id == CLSID_DSoundRender) dsFilter = f;
            else if (id == flacId || id == wavParserId) decoderFilter = f;
        }
        f.Reset();
    }

    if (!dsFilter) return result;
    auto dsIn = findUniquePin(dsFilter.Get(), PINDIR_INPUT);
    if (!dsIn) return result;

    auto eqIn = eqFilter ? findUniquePin(eqFilter.Get(), PINDIR_INPUT) : nullptr;
    auto eqOut = eqFilter ? findUniquePin(eqFilter.Get(), PINDIR_OUTPUT) : nullptr;
    auto seamOut = seamFilter ? findUniquePin(seamFilter.Get(), PINDIR_OUTPUT) : nullptr;
    auto dseeOut = dseeFilter ? findUniquePin(dseeFilter.Get(), PINDIR_OUTPUT) : nullptr;

    FormatStage dseeOutFmt{}, seamOutFmt{};
    if (dseeOut) readPinAudioFormat(dseeOut.Get(), dseeOutFmt);
    if (seamOut) readPinAudioFormat(seamOut.Get(), seamOutFmt);

    // Issue #56: If DSEE HX is OFF, EQ bypass is NOT required.
    // Restore the standard EQ route (seamOut -> eqIn and eqOut -> dsIn) if it was previously bypassed.
    if (!dseeActive) {
        if (!eqFilter || !eqIn || !eqOut || !seamOut) {
            // No EQ filter in graph (e.g. native Hi-Res) or missing pins: keep existing state.
            result.outcome = RouteOutcome::Skipped;
            return result;
        }

        ComPtr<IPin> currentDsPeer;
        if (SUCCEEDED(dsIn->ConnectedTo(&currentDsPeer)) && currentDsPeer) {
            if (currentDsPeer.Get() == eqOut.Get()) {
                // DirectSound is already connected to eqOut: perfectly normal unbypassed state.
                result.outcome = RouteOutcome::Skipped;
                result.change = S_OK;

                AM_MEDIA_TYPE mt{};
                if (SUCCEEDED(dsIn->ConnectionMediaType(&mt)) && mt.pbFormat && mt.cbFormat >= sizeof(WAVEFORMATEX)) {
                    auto* w = reinterpret_cast<WAVEFORMATEX*>(mt.pbFormat);
                    result.outputRate = w->nSamplesPerSec;
                    result.outputBits = w->wBitsPerSample;
                    result.outputChannels = w->nChannels;
                    if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
                    if (mt.pUnk) mt.pUnk->Release();
                }
                return result;
            }
        }

        // DirectSound was bypassed (connected directly to seamOut or dseeOut).
        // Rewire to restore standard EQ route: seamOut -> eqIn and eqOut -> dsIn.
        ComPtr<IMediaControl> control;
        OAFilterState state = State_Stopped;
        if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&control)))) {
            control->GetState(0, &state);
        }
        bool wasPaused = (state == State_Paused);
        if (wasPaused) {
            control->Stop();
        } else if (state != State_Stopped) {
            return result;
        }

        ComPtr<IMediaSeeking> seeking;
        LONGLONG position = 0;
        if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&seeking)))) {
            seeking->GetCurrentPosition(&position);
        }

        graph->Disconnect(dsIn.Get());
        if (currentDsPeer) graph->Disconnect(currentDsPeer.Get());

        ComPtr<IPin> p;
        if (SUCCEEDED(eqIn->ConnectedTo(&p)) && p) {
            graph->Disconnect(eqIn.Get());
            graph->Disconnect(p.Get());
        }
        if (SUCCEEDED(eqOut->ConnectedTo(&p)) && p) {
            graph->Disconnect(eqOut.Get());
            graph->Disconnect(p.Get());
        }

        HRESULT hr1 = graph->ConnectDirect(seamOut.Get(), eqIn.Get(), nullptr);
        HRESULT hr2 = graph->ConnectDirect(eqOut.Get(), dsIn.Get(), nullptr);

        if (SUCCEEDED(hr1) && SUCCEEDED(hr2)) {
            AM_MEDIA_TYPE mt{};
            if (SUCCEEDED(dsIn->ConnectionMediaType(&mt)) && mt.pbFormat && mt.cbFormat >= sizeof(WAVEFORMATEX)) {
                auto* w = reinterpret_cast<WAVEFORMATEX*>(mt.pbFormat);
                result.outputRate = w->nSamplesPerSec;
                result.outputBits = w->wBitsPerSample;
                result.outputChannels = w->nChannels;
                if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
                if (mt.pUnk) mt.pUnk->Release();
            }
            if (seeking) {
                seeking->SetPositions(&position, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning);
            }
            if (wasPaused) {
                control->Pause();
            }
            result.outcome = RouteOutcome::Restored;
            result.change = S_OK;
            return result;
        }

        result.outcome = RouteOutcome::RestoreFailed;
        result.change = FAILED(hr1) ? hr1 : hr2;
        return result;
    }

    ComPtr<IPin> upstreamEqOut;
    if (eqIn) {
        eqIn->ConnectedTo(&upstreamEqOut);
    }

    // Determine target upstream pin to feed DirectSound
    ComPtr<IPin> targetOut;
    if (dseeOut && dseeOutFmt.connected && dseeOutFmt.sampleRate >= 88200) {
        // DSEE HX is active and generating 88.2kHz / 96kHz / 176.4k / 192k
        if (seamOut && seamOutFmt.connected && seamOutFmt.sampleRate == dseeOutFmt.sampleRate && seamOutFmt.bitDepth >= 24) {
            targetOut = seamOut; // Seamless maintains full Hi-Res rate (e.g. 96k/24b)
        } else {
            targetOut = dseeOut; // Downsampler/Seamless cannot pass 88.2k; connect DSEE directly (Issue #46)
        }
    } else if (seamOut && seamOutFmt.connected && seamOutFmt.sampleRate > 0) {
        targetOut = seamOut;
    } else if (upstreamEqOut) {
        targetOut = upstreamEqOut;
    }

    if (!targetOut) return result;

    // Check if DirectSoundRender input is ALREADY connected directly to targetOut
    ComPtr<IPin> currentDsPeer;
    if (SUCCEEDED(dsIn->ConnectedTo(&currentDsPeer)) && currentDsPeer) {
        if (currentDsPeer.Get() == targetOut.Get()) {
            result.outcome = RouteOutcome::AlreadyBypassed;
            result.change = S_OK;

            AM_MEDIA_TYPE mt{};
            if (SUCCEEDED(dsIn->ConnectionMediaType(&mt)) && mt.pbFormat && mt.cbFormat >= sizeof(WAVEFORMATEX)) {
                auto* w = reinterpret_cast<WAVEFORMATEX*>(mt.pbFormat);
                result.outputRate = w->nSamplesPerSec;
                result.outputBits = w->wBitsPerSample;
                result.outputChannels = w->nChannels;
                if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
                if (mt.pUnk) mt.pUnk->Release();
            }
            return result;
        }
    }

    // DirectShow filter state handling:
    ComPtr<IMediaControl> control;
    OAFilterState state = State_Stopped;
    if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&control)))) {
        control->GetState(0, &state);
    }
    bool wasPaused = (state == State_Paused);
    if (wasPaused) {
        control->Stop();
    } else if (state != State_Stopped) {
        return result; // Cannot rewire while running
    }

    // Save current seeking position
    ComPtr<IMediaSeeking> seeking;
    LONGLONG position = 0;
    if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&seeking)))) {
        seeking->GetCurrentPosition(&position);
    }

    // Save existing connections & media types for rollback
    ComPtr<IPin> targetDownstreamIn;
    targetOut->ConnectedTo(&targetDownstreamIn);
    RouteType targetType, dsType;
    targetOut->ConnectionMediaType(&targetType.value);
    if (currentDsPeer) {
        currentDsPeer->ConnectionMediaType(&dsType.value);
    }

    // Disconnect targetOut and its downstream peer
    graph->Disconnect(targetOut.Get());
    if (targetDownstreamIn) graph->Disconnect(targetDownstreamIn.Get());

    // Disconnect dsIn and its peer
    graph->Disconnect(dsIn.Get());
    if (currentDsPeer) graph->Disconnect(currentDsPeer.Get());

    // Disconnect EQ pins if still connected
    if (eqIn) {
        ComPtr<IPin> p;
        if (SUCCEEDED(eqIn->ConnectedTo(&p)) && p) {
            graph->Disconnect(eqIn.Get());
            graph->Disconnect(p.Get());
        }
    }
    if (eqOut) {
        ComPtr<IPin> p;
        if (SUCCEEDED(eqOut->ConnectedTo(&p)) && p) {
            graph->Disconnect(eqOut.Get());
            graph->Disconnect(p.Get());
        }
    }

    // Connect targetOut directly to DirectSound
    HRESULT hr = graph->ConnectDirect(targetOut.Get(), dsIn.Get(), nullptr);
    if (FAILED(hr) && targetType.value.cbFormat > 0) {
        hr = graph->ConnectDirect(targetOut.Get(), dsIn.Get(), &targetType.value);
    }

    if (SUCCEEDED(hr)) {
        AM_MEDIA_TYPE mt{};
        if (SUCCEEDED(dsIn->ConnectionMediaType(&mt)) && mt.pbFormat && mt.cbFormat >= sizeof(WAVEFORMATEX)) {
            auto* w = reinterpret_cast<WAVEFORMATEX*>(mt.pbFormat);
            result.outputRate = w->nSamplesPerSec;
            result.outputBits = w->wBitsPerSample;
            result.outputChannels = w->nChannels;
            if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
            if (mt.pUnk) mt.pUnk->Release();
        }
        if (seeking) {
            seeking->SetPositions(&position, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning);
        }
        if (wasPaused) {
            control->Pause();
        }
        result.outcome = RouteOutcome::Bypassed;
        result.change = hr;
        return result;
    }

    // Rollback if direct connection failed
    graph->Disconnect(targetOut.Get());
    graph->Disconnect(dsIn.Get());
    if (targetDownstreamIn) {
        graph->ConnectDirect(targetOut.Get(), targetDownstreamIn.Get(), &targetType.value);
    }
    if (currentDsPeer) {
        graph->ConnectDirect(currentDsPeer.Get(), dsIn.Get(), &dsType.value);
    }
    if (seeking) {
        seeking->SetPositions(&position, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning);
    }
    if (wasPaused) {
        control->Pause();
    }
    result.change = hr;
    result.outcome = RouteOutcome::Restored;
    return result;
}

} // namespace dseeUnified
