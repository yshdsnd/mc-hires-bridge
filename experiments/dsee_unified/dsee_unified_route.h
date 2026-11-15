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
};

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

// Inspects negotiated audio formats across all 5 DirectShow graph stages:
// Decode PCM -> DSEE In/Out -> Seamless In/Out -> EQ In/Out -> DirectSound In
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
                    out.eqBypassed = true;
                }
            }
        }
    }
    if (!out.eqIn.connected) {
        out.eqBypassed = true;
    }
    return true;
}

// Bypasses OmgEqxlFilter (EQ) so upstream filter (SeamlessFilter, DSEE, or Source)
// connects directly to DirectSound.
// Supports both seamless transitions (Topology A: Seamless -> EQ -> DS) and
// format-change transitions (Topology B: DSEE -> EQ -> DS).
inline RouteResult applyEqBypass(IGraphBuilder* graph) {
    RouteResult result;
    if (!graph) return result;

    ComPtr<IBaseFilter> dseeFilter, seamFilter, eqFilter, dsFilter;
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
        }
        f.Reset();
    }

    if (!dsFilter) return result;
    auto dsIn = findUniquePin(dsFilter.Get(), PINDIR_INPUT);
    if (!dsIn) return result;

    auto eqIn = eqFilter ? findUniquePin(eqFilter.Get(), PINDIR_INPUT) : nullptr;
    auto eqOut = eqFilter ? findUniquePin(eqFilter.Get(), PINDIR_OUTPUT) : nullptr;

    // Check FIRST if DirectSoundRender input is ALREADY connected directly to an upstream filter (EQ already bypassed)
    ComPtr<IPin> currentDsPeer;
    if (SUCCEEDED(dsIn->ConnectedTo(&currentDsPeer)) && currentDsPeer) {
        if (!eqOut || currentDsPeer.Get() != eqOut.Get()) {
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

    // If EQ is not present or not connected to DirectSound, nothing to bypass
    if (!eqFilter || !eqIn || !eqOut) return result;

    // Determine which pin is feeding EQ (SeamlessFilter, DSEE, or Source)
    ComPtr<IPin> upstreamOut;
    if (FAILED(eqIn->ConnectedTo(&upstreamOut)) || !upstreamOut) return result;
    if (FAILED(eqOut->ConnectedTo(&currentDsPeer)) || currentDsPeer.Get() != dsIn.Get()) return result;

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

    // Save existing media types for rollback
    auto edgeUpstreamEq = std::make_unique<RouteEdge>();
    edgeUpstreamEq->out = upstreamOut; edgeUpstreamEq->in = eqIn;
    upstreamOut->ConnectionMediaType(&edgeUpstreamEq->type.value);

    auto edgeEqDs = std::make_unique<RouteEdge>();
    edgeEqDs->out = eqOut; edgeEqDs->in = dsIn;
    eqOut->ConnectionMediaType(&edgeEqDs->type.value);

    // Save current seeking position
    ComPtr<IMediaSeeking> seeking;
    LONGLONG position = 0;
    if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&seeking)))) {
        seeking->GetCurrentPosition(&position);
    }

    // Disconnect Upstream -> EQ and EQ -> DirectSound
    graph->Disconnect(upstreamOut.Get());
    graph->Disconnect(eqIn.Get());
    graph->Disconnect(eqOut.Get());
    graph->Disconnect(dsIn.Get());

    // Connect Upstream directly to DirectSound
    HRESULT hr = graph->ConnectDirect(upstreamOut.Get(), dsIn.Get(), nullptr);
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
    graph->Disconnect(upstreamOut.Get());
    graph->Disconnect(dsIn.Get());

    HRESULT r1 = graph->ConnectDirect(edgeUpstreamEq->out.Get(), edgeUpstreamEq->in.Get(), &edgeUpstreamEq->type.value);
    HRESULT r2 = graph->ConnectDirect(edgeEqDs->out.Get(), edgeEqDs->in.Get(), &edgeEqDs->type.value);
    HRESULT rollback = SUCCEEDED(r1) ? r2 : r1;
    if (seeking) {
        seeking->SetPositions(&position, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning);
    }
    if (wasPaused) {
        control->Pause();
    }
    result.change = hr;
    result.restore = rollback;
    result.outcome = SUCCEEDED(rollback) ? RouteOutcome::Restored : RouteOutcome::RestoreFailed;
    return result;
}

} // namespace dseeUnified
