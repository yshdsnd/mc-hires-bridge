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

inline ComPtr<IPin> findUniquePin(IBaseFilter* filter, PIN_DIRECTION wanted) {
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

// Bypasses OmgEqxlFilter (EQ) so SeamlessFilter outputs directly to DirectSound
inline RouteResult applyEqBypass(IGraphBuilder* graph) {
    RouteResult result;
    if (!graph) return result;

    ComPtr<IMediaControl> control;
    OAFilterState state{};
    if (FAILED(graph->QueryInterface(IID_PPV_ARGS(&control))) ||
        control->GetState(0, &state) != S_OK || state != State_Stopped) {
        return result;
    }

    ComPtr<IBaseFilter> dseeFilter, seamFilter, eqFilter, dsFilter;
    ComPtr<IEnumFilters> enumFilters;
    if (FAILED(graph->EnumFilters(&enumFilters))) return result;

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

    // Must have Seamless and DirectSound
    if (!seamFilter || !dsFilter) return result;

    auto seamOut = findUniquePin(seamFilter.Get(), PINDIR_OUTPUT);
    auto dsIn = findUniquePin(dsFilter.Get(), PINDIR_INPUT);
    if (!seamOut || !dsIn) return result;

    // Check if Seamless is already connected directly to DirectSound
    ComPtr<IPin> currentPeer;
    if (SUCCEEDED(seamOut->ConnectedTo(&currentPeer)) && currentPeer.Get() == dsIn.Get()) {
        result.outcome = RouteOutcome::AlreadyBypassed;
        result.change = S_OK;

        AM_MEDIA_TYPE mt{};
        if (SUCCEEDED(dsIn->ConnectionMediaType(&mt)) && mt.pbFormat && mt.cbFormat >= 16) {
            auto* w = reinterpret_cast<WAVEFORMATEX*>(mt.pbFormat);
            result.outputRate = w->nSamplesPerSec;
            result.outputBits = w->wBitsPerSample;
            result.outputChannels = w->nChannels;
            if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
        }
        return result;
    }

    // If EQ is present, trace the edges to disconnect and reconnect Seamless -> DirectSound
    if (!eqFilter) return result;

    auto eqIn = findUniquePin(eqFilter.Get(), PINDIR_INPUT);
    auto eqOut = findUniquePin(eqFilter.Get(), PINDIR_OUTPUT);
    if (!eqIn || !eqOut) return result;

    // Check that Seamless -> EQ and EQ -> DirectSound are currently connected
    ComPtr<IPin> seamPeer, eqPeer;
    if (FAILED(seamOut->ConnectedTo(&seamPeer)) || seamPeer.Get() != eqIn.Get()) return result;
    if (FAILED(eqOut->ConnectedTo(&eqPeer)) || eqPeer.Get() != dsIn.Get()) return result;

    // Save existing media types for rollback
    auto edgeSeamEq = std::make_unique<RouteEdge>();
    edgeSeamEq->out = seamOut; edgeSeamEq->in = eqIn;
    seamOut->ConnectionMediaType(&edgeSeamEq->type.value);

    auto edgeEqDs = std::make_unique<RouteEdge>();
    edgeEqDs->out = eqOut; edgeEqDs->in = dsIn;
    eqOut->ConnectionMediaType(&edgeEqDs->type.value);

    // Save current seeking position
    ComPtr<IMediaSeeking> seeking;
    LONGLONG position = 0;
    if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&seeking)))) {
        seeking->GetCurrentPosition(&position);
    }

    // Disconnect Seamless -> EQ and EQ -> DirectSound
    graph->Disconnect(seamOut.Get());
    graph->Disconnect(eqIn.Get());
    graph->Disconnect(eqOut.Get());
    graph->Disconnect(dsIn.Get());

    // Connect Seamless directly to DirectSound
    HRESULT hr = graph->ConnectDirect(seamOut.Get(), dsIn.Get(), nullptr);
    if (SUCCEEDED(hr)) {
        AM_MEDIA_TYPE mt{};
        if (SUCCEEDED(dsIn->ConnectionMediaType(&mt)) && mt.pbFormat && mt.cbFormat >= 16) {
            auto* w = reinterpret_cast<WAVEFORMATEX*>(mt.pbFormat);
            result.outputRate = w->nSamplesPerSec;
            result.outputBits = w->wBitsPerSample;
            result.outputChannels = w->nChannels;
            if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
        }
        if (seeking) {
            seeking->SetPositions(&position, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning);
        }
        result.outcome = RouteOutcome::Bypassed;
        result.change = hr;
        return result;
    }

    // Rollback if direct connection failed
    graph->Disconnect(seamOut.Get());
    graph->Disconnect(dsIn.Get());

    HRESULT r1 = graph->ConnectDirect(edgeSeamEq->out.Get(), edgeSeamEq->in.Get(), &edgeSeamEq->type.value);
    HRESULT r2 = graph->ConnectDirect(edgeEqDs->out.Get(), edgeEqDs->in.Get(), &edgeEqDs->type.value);
    HRESULT rollback = SUCCEEDED(r1) ? r2 : r1;
    if (seeking) {
        seeking->SetPositions(&position, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning);
    }
    result.change = hr;
    result.restore = rollback;
    result.outcome = SUCCEEDED(rollback) ? RouteOutcome::Restored : RouteOutcome::RestoreFailed;
    return result;
}

} // namespace dseeUnified
