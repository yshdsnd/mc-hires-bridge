// Headless test suite for Issue #42:
// DSEE HX ON unified 24-bit output across 44.1kHz/48kHz PCM and codecs.
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>
#include "dsee_unified_config.h"
#include "dsee_unified_route.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::printf("  [FAIL] line %d: %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

// Test 1: Input Determination & Rate Selection
void testConfigDeterminationAndRateSelection() {
    std::printf("[TEST 1] Input Determination & Rate Selection...\n");

    // 1A: 44.1kHz / 16bit input with DSEE HX ON (mode = 1) -> Family2x (88.2kHz / 24bit)
    {
        dseeUnified::DseeConfig req{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x));
        CHECK(adj.mode == 1);
        CHECK(adj.channels == 2);
        CHECK(adj.inputRate == 44100);
        CHECK(adj.outputRate == 88200);
        CHECK(adj.outputBits == 24);
        CHECK(adj.format == 16);
    }

    // 1B: 48kHz / 16bit input with DSEE HX ON (mode = 1) -> Family2x (96kHz / 24bit)
    {
        dseeUnified::DseeConfig req{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x));
        CHECK(adj.mode == 1);
        CHECK(adj.channels == 2);
        CHECK(adj.inputRate == 48000);
        CHECK(adj.outputRate == 96000);
        CHECK(adj.outputBits == 24);
    }

    // 1C: 44.1kHz / 24bit input with DSEE HX ON (mode = 1) -> Family2x (88.2kHz / 24bit)
    {
        dseeUnified::DseeConfig req{1, 2, 16, 44100, 44100, 24, 24, 0, 0};
        dseeUnified::DseeConfig adj{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x));
        CHECK(adj.mode == 1);
        CHECK(adj.outputRate == 88200);
        CHECK(adj.outputBits == 24);
    }

    // 1D: 48kHz / 24bit input with DSEE HX ON (mode = 1) -> Family2x (96kHz / 24bit)
    {
        dseeUnified::DseeConfig req{1, 2, 16, 48000, 48000, 24, 24, 0, 0};
        dseeUnified::DseeConfig adj{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x));
        CHECK(adj.mode == 1);
        CHECK(adj.outputRate == 96000);
        CHECK(adj.outputBits == 24);
    }

    // 1E: Native 24-bit mode (no rate expansion, 24-bit quantization expansion)
    {
        dseeUnified::DseeConfig req44{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj44{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(req44, adj44, dseeUnified::HarmonizeMode::Native24b));
        CHECK(adj44.outputRate == 44100);
        CHECK(adj44.outputBits == 24);

        dseeUnified::DseeConfig req48{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj48{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(req48, adj48, dseeUnified::HarmonizeMode::Native24b));
        CHECK(adj48.outputRate == 48000);
        CHECK(adj48.outputBits == 24);
    }

    // 1F: Family 4x mode
    {
        dseeUnified::DseeConfig req44{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj44{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(req44, adj44, dseeUnified::HarmonizeMode::Family4x));
        CHECK(adj44.outputRate == 176400);
        CHECK(adj44.outputBits == 24);

        dseeUnified::DseeConfig req48{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj48{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(req48, adj48, dseeUnified::HarmonizeMode::Family4x));
        CHECK(adj48.outputRate == 192000);
        CHECK(adj48.outputBits == 24);
    }

    // 1G: Rejections for non-stereo / non-target rates
    {
        dseeUnified::DseeConfig monoReq{1, 1, 16, 44100, 44100, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj{};
        CHECK(!dseeUnified::computeUnifiedDseeConfig(monoReq, adj)); // Mono rejected

        dseeUnified::DseeConfig quadReq{1, 4, 16, 44100, 44100, 16, 16, 0, 0};
        CHECK(!dseeUnified::computeUnifiedDseeConfig(quadReq, adj)); // Multi-channel rejected

        dseeUnified::DseeConfig oddRateReq{1, 2, 16, 32000, 32000, 16, 16, 0, 0};
        CHECK(!dseeUnified::computeUnifiedDseeConfig(oddRateReq, adj)); // 32kHz rejected
    }
}

// Test 2: Codec Identification
void testCodecCallerIdentification() {
    std::printf("[TEST 2] Codec Caller Identification...\n");

    CHECK(dseeUnified::identifyCodecFromModule(L"OpcFlac.dll") == dseeUnified::CodecType::Flac);
    CHECK(dseeUnified::identifyCodecFromModule(L"opcflac.dll") == dseeUnified::CodecType::Flac);
    CHECK(dseeUnified::identifyCodecFromModule(L"OpcMp4.dll") == dseeUnified::CodecType::Aac);
    CHECK(dseeUnified::identifyCodecFromModule(L"OpcAac.dll") == dseeUnified::CodecType::Aac);
    CHECK(dseeUnified::identifyCodecFromModule(L"OpcAlac.dll") == dseeUnified::CodecType::Alac);
    CHECK(dseeUnified::identifyCodecFromModule(L"OpcWavEx.dll") == dseeUnified::CodecType::Wav);
    CHECK(dseeUnified::identifyCodecFromModule(L"OpcCd3.dll") == dseeUnified::CodecType::Mp3);
    CHECK(dseeUnified::identifyCodecFromModule(L"OpcMp3.dll") == dseeUnified::CodecType::Mp3);
    CHECK(dseeUnified::identifyCodecFromModule(L"OpcApe.dll") == dseeUnified::CodecType::Flac);
    CHECK(dseeUnified::identifyCodecFromModule(L"Unknown.dll") == dseeUnified::CodecType::Unknown);
    CHECK(dseeUnified::identifyCodecFromModule(nullptr) == dseeUnified::CodecType::Unknown);

    CHECK(std::strcmp(dseeUnified::codecName(dseeUnified::CodecType::Flac), "FLAC") == 0);
    CHECK(std::strcmp(dseeUnified::codecName(dseeUnified::CodecType::Aac), "AAC") == 0);
    CHECK(std::strcmp(dseeUnified::codecName(dseeUnified::CodecType::Alac), "ALAC") == 0);
    CHECK(std::strcmp(dseeUnified::codecName(dseeUnified::CodecType::Wav), "WAV") == 0);
    CHECK(std::strcmp(dseeUnified::codecName(dseeUnified::CodecType::Mp3), "MP3") == 0);
    CHECK(std::strcmp(dseeUnified::codecName(dseeUnified::CodecType::Cd), "CD") == 0);
}

// Test 3: Preservation of Hi-Res & DSEE OFF (Requirement 5)
void testPreservationOfHiResAndBypass() {
    std::printf("[TEST 3] Preservation of Hi-Res & DSEE OFF...\n");

    // DSEE HX OFF (mode = 0) with 44.1k/16b -> must pass through untouched
    {
        dseeUnified::DseeConfig offReq{0, 2, 16, 44100, 44100, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj{};
        CHECK(!dseeUnified::computeUnifiedDseeConfig(offReq, adj));
    }

    // Native Hi-Res 96kHz / 24bit -> must pass through untouched
    {
        dseeUnified::DseeConfig hires96{1, 2, 16, 96000, 96000, 24, 24, 0, 0};
        dseeUnified::DseeConfig adj{};
        CHECK(!dseeUnified::computeUnifiedDseeConfig(hires96, adj));
    }

    // Native Hi-Res 192kHz / 24bit -> must pass through untouched
    {
        dseeUnified::DseeConfig hires192{1, 2, 16, 192000, 192000, 24, 24, 0, 0};
        dseeUnified::DseeConfig adj{};
        CHECK(!dseeUnified::computeUnifiedDseeConfig(hires192, adj));
    }

    // Hi-Res rate predicate check
    CHECK(dseeUnified::isHiResRate(88200));
    CHECK(dseeUnified::isHiResRate(96000));
    CHECK(dseeUnified::isHiResRate(192000));
    CHECK(!dseeUnified::isHiResRate(44100));
    CHECK(!dseeUnified::isHiResRate(48000));
}

// Test 4: Common 96k vs Family 2x Negotiation & Constraint Verification (Requirement 3)
void testHarmonizationNegotiationConstraints() {
    std::printf("[TEST 4] Harmonization Negotiation Constraints (Requirement 3)...\n");

    // When mode is Common96k:
    // Both 44.1k and 48k request 96000 outputRate:
    dseeUnified::DseeConfig req44{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
    dseeUnified::DseeConfig adj44{};
    CHECK(dseeUnified::computeUnifiedDseeConfig(req44, adj44, dseeUnified::HarmonizeMode::Common96k));
    CHECK(adj44.outputRate == 96000);
    CHECK(adj44.outputBits == 24);

    dseeUnified::DseeConfig req48{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
    dseeUnified::DseeConfig adj48{};
    CHECK(dseeUnified::computeUnifiedDseeConfig(req48, adj48, dseeUnified::HarmonizeMode::Common96k));
    CHECK(adj48.outputRate == 96000);
    CHECK(adj48.outputBits == 24);

    // Documented observed DSP constraint:
    // OmgDseeHxFilter internally clamps 44.1kHz with requested 96000 to 176,400 Hz (4x integer multiple).
    // Therefore, Family2x (44.1k->88.2k, 48k->96k) or Native24b (44.1k->44.1k, 48k->48k) provides
    // consistent harmonic alignment without non-integer cross-family resampling mismatch.
}

// Test 5: Graph Tracker Lifecycle (Requirement 8: GRAPH_RECREATED vs GRAPH_REUSED)
void testGraphTrackerLifecycle() {
    std::printf("[TEST 5] Graph Tracker Lifecycle (Requirement 8)...\n");

    dseeUnified::GraphTracker tracker;
    void* graphA = reinterpret_cast<void*>(0x1000);
    void* graphB = reinterpret_cast<void*>(0x2000);

    // Initial track: 44.1kHz / 16bit FLAC
    auto t1 = tracker.onGraphEvent(graphA, 88200, 24, dseeUnified::CodecType::Flac);
    CHECK(t1 == dseeUnified::GraphTransitionType::Initial);
    CHECK(tracker.generation == 1);
    CHECK(tracker.recreationCount == 0);
    CHECK(tracker.reuseCount == 0);

    // Track 2: same graph reused, same format (88.2k / 24bit FLAC)
    auto t2 = tracker.onGraphEvent(graphA, 88200, 24, dseeUnified::CodecType::Flac);
    CHECK(t2 == dseeUnified::GraphTransitionType::ReusedSameFormat);
    CHECK(tracker.generation == 1);
    CHECK(tracker.reuseCount == 1);

    // Track 3: same graph reused, format switched (to 96kHz / 24bit AAC)
    auto t3 = tracker.onGraphEvent(graphA, 96000, 24, dseeUnified::CodecType::Aac);
    CHECK(t3 == dseeUnified::GraphTransitionType::ReusedFormatSwitched);
    CHECK(tracker.generation == 1);
    CHECK(tracker.reuseCount == 2);

    // Track 4: graph recreated (new graph pointer graphB, e.g. manual user skip or codec teardown)
    auto t4 = tracker.onGraphEvent(graphB, 96000, 24, dseeUnified::CodecType::Aac);
    CHECK(t4 == dseeUnified::GraphTransitionType::Recreated);
    CHECK(tracker.generation == 2);
    CHECK(tracker.recreationCount == 1);

    // Track 5: graphB reused, format switched (96k AAC -> 88.2k MP3)
    auto t5 = tracker.onGraphEvent(graphB, 88200, 24, dseeUnified::CodecType::Mp3);
    CHECK(t5 == dseeUnified::GraphTransitionType::ReusedFormatSwitched);
    CHECK(tracker.generation == 2);
    CHECK(tracker.reuseCount == 3);

    // Reset verification
    tracker.reset();
    CHECK(tracker.generation == 0);
    CHECK(tracker.activeGraph == nullptr);
}

// Test 6: Mock DirectShow Graph & EQ Bypass Verification
// Mock implementations to test applyEqBypass routing and rollback
class MockPin : public IPin {
public:
    ULONG ref = 1;
    PIN_DIRECTION dir;
    IBaseFilter* filter = nullptr;
    IPin* connectedTo = nullptr;
    AM_MEDIA_TYPE mt{};
    std::wstring pinName;

    MockPin(PIN_DIRECTION d, IBaseFilter* f, DWORD rate = 88200, WORD bits = 24, const wchar_t* pn = L"")
        : dir(d), filter(f), pinName(pn ? pn : L"") {
        mt.majortype = MEDIATYPE_Audio;
        mt.subtype = MEDIASUBTYPE_PCM;
        mt.formattype = FORMAT_WaveFormatEx;
        mt.cbFormat = sizeof(WAVEFORMATEX);
        mt.pbFormat = reinterpret_cast<BYTE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        auto* w = reinterpret_cast<WAVEFORMATEX*>(mt.pbFormat);
        w->wFormatTag = WAVE_FORMAT_PCM;
        w->nChannels = 2;
        w->nSamplesPerSec = rate;
        w->wBitsPerSample = bits;
        w->nBlockAlign = (bits / 8) * 2;
        w->nAvgBytesPerSec = rate * w->nBlockAlign;
    }
    ~MockPin() {
        if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
    }

    void setFormat(DWORD rate, WORD bits) {
        if (mt.pbFormat && mt.cbFormat >= sizeof(WAVEFORMATEX)) {
            auto* w = reinterpret_cast<WAVEFORMATEX*>(mt.pbFormat);
            w->nSamplesPerSec = rate;
            w->wBitsPerSample = bits;
            w->nBlockAlign = (bits / 8) * 2;
            w->nAvgBytesPerSec = rate * w->nBlockAlign;
        }
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPin) {
            *ppv = static_cast<IPin*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref;
        if (r == 0) delete this;
        return r;
    }

    // IPin
    HRESULT STDMETHODCALLTYPE Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override {
        connectedTo = pConnector;
        if (pmt && pmt->pbFormat && pmt->cbFormat >= sizeof(WAVEFORMATEX)) {
            if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
            mt = *pmt;
            mt.pbFormat = reinterpret_cast<BYTE*>(CoTaskMemAlloc(pmt->cbFormat));
            std::memcpy(mt.pbFormat, pmt->pbFormat, pmt->cbFormat);
        } else if (!pmt && pConnector && dir == PINDIR_INPUT) {
            AM_MEDIA_TYPE peerMt{};
            if (SUCCEEDED(pConnector->ConnectionMediaType(&peerMt)) && peerMt.pbFormat && peerMt.cbFormat >= sizeof(WAVEFORMATEX)) {
                if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
                mt = peerMt;
                mt.pbFormat = reinterpret_cast<BYTE*>(CoTaskMemAlloc(peerMt.cbFormat));
                std::memcpy(mt.pbFormat, peerMt.pbFormat, peerMt.cbFormat);
                CoTaskMemFree(peerMt.pbFormat);
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Disconnect() override {
        connectedTo = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pPin) override {
        if (!pPin) return E_POINTER;
        if (!connectedTo) return VFW_E_NOT_CONNECTED;
        *pPin = connectedTo;
        connectedTo->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE* pmt) override {
        if (!pmt) return E_POINTER;
        *pmt = mt;
        if (mt.cbFormat) {
            pmt->pbFormat = reinterpret_cast<BYTE*>(CoTaskMemAlloc(mt.cbFormat));
            std::memcpy(pmt->pbFormat, mt.pbFormat, mt.cbFormat);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* pInfo) override {
        if (!pInfo) return E_POINTER;
        pInfo->pFilter = filter;
        if (filter) filter->AddRef();
        pInfo->dir = dir;
        wcsncpy_s(pInfo->achName, pinName.c_str(), _TRUNCATE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* pPinDir) override {
        if (!pPinDir) return E_POINTER;
        *pPinDir = dir;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* Id) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE* pmt) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes** ppEnum) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin** apPin, ULONG* nPin) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndOfStream() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE BeginFlush() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EndFlush() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override { return S_OK; }
};

class MockEnumPins : public IEnumPins {
public:
    ULONG ref = 1;
    std::vector<IPin*> pins;
    size_t index = 0;

    MockEnumPins(const std::vector<IPin*>& p) : pins(p) {
        for (auto* pin : pins) pin->AddRef();
    }
    ~MockEnumPins() {
        for (auto* pin : pins) pin->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
        if (!ppPins) return E_POINTER;
        ULONG fetched = 0;
        while (fetched < cPins && index < pins.size()) {
            ppPins[fetched] = pins[index];
            pins[index]->AddRef();
            ++fetched;
            ++index;
        }
        if (pcFetched) *pcFetched = fetched;
        return fetched == cPins ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG cPins) override { index += cPins; return S_OK; }
    HRESULT STDMETHODCALLTYPE Reset() override { index = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumPins** ppEnum) override { return E_NOTIMPL; }
};

class MockFilter : public IBaseFilter {
public:
    ULONG ref = 1;
    CLSID clsid;
    std::wstring filterName;
    std::vector<IPin*> pins;

    MockFilter(const CLSID& id, const wchar_t* name = L"") : clsid(id), filterName(name ? name : L"") {}
    ~MockFilter() {
        for (auto* p : pins) p->Release();
    }

    void addPin(MockPin* p) {
        pins.push_back(p);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
            *ppv = static_cast<IBaseFilter*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetClassID(CLSID* pClassID) override {
        if (!pClassID) return E_POINTER;
        *pClassID = clsid;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Pause() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME tStart) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override {
        if (!State) return E_POINTER;
        *State = State_Stopped;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock* pClock) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock** pClock) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new MockEnumPins(pins);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR Id, IPin** ppPin) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* pInfo) override {
        if (!pInfo) return E_POINTER;
        pInfo->pGraph = nullptr;
        wcsncpy_s(pInfo->achName, filterName.c_str(), _TRUNCATE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR* pVendorInfo) override { return E_NOTIMPL; }
};

class MockEnumFilters : public IEnumFilters {
public:
    ULONG ref = 1;
    std::vector<IBaseFilter*> filters;
    size_t index = 0;

    MockEnumFilters(const std::vector<IBaseFilter*>& f) : filters(f) {
        for (auto* filter : filters) filter->AddRef();
    }
    ~MockEnumFilters() {
        for (auto* filter : filters) filter->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumFilters) {
            *ppv = static_cast<IEnumFilters*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG cFilters, IBaseFilter** ppFilters, ULONG* pcFetched) override {
        if (!ppFilters) return E_POINTER;
        ULONG fetched = 0;
        while (fetched < cFilters && index < filters.size()) {
            ppFilters[fetched] = filters[index];
            filters[index]->AddRef();
            ++fetched;
            ++index;
        }
        if (pcFetched) *pcFetched = fetched;
        return fetched == cFilters ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG cFilters) override { index += cFilters; return S_OK; }
    HRESULT STDMETHODCALLTYPE Reset() override { index = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumFilters** ppEnum) override { return E_NOTIMPL; }
};

class MockGraph : public IGraphBuilder, public IMediaControl {
public:
    ULONG ref = 1;
    std::vector<IBaseFilter*> filters;
    IPin* failOutPin = nullptr;
    IPin* failInPin = nullptr;

    ~MockGraph() {
        for (auto* f : filters) f->Release();
    }

    void addFilter(MockFilter* f) {
        f->AddRef();
        filters.push_back(f);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IFilterGraph || riid == IID_IGraphBuilder) {
            *ppv = static_cast<IGraphBuilder*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IMediaControl) {
            *ppv = static_cast<IMediaControl*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref;
        if (r == 0) delete this;
        return r;
    }

    // IDispatch
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* pctinfo) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID riid, LPOLESTR* rgszNames, UINT cNames, LCID lcid, DISPID* rgDispId) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS* pDispParams, VARIANT* pVarResult, EXCEPINFO* pExcepInfo, UINT* puArgErr) override { return E_NOTIMPL; }

    // IMediaControl
    HRESULT STDMETHODCALLTYPE Run() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Pause() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Stop() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetState(LONG msTimeout, OAFilterState* pfs) override {
        if (!pfs) return E_POINTER;
        *pfs = State_Stopped;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RenderFile(BSTR strFile) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddSourceFilter(BSTR strFilename, IDispatch** ppUnk) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_FilterCollection(IDispatch** ppUnk) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_RegFilterCollection(IDispatch** ppUnk) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE StopWhenReady() override { return E_NOTIMPL; }

    // IFilterGraph
    HRESULT STDMETHODCALLTYPE AddFilter(IBaseFilter* pFilter, LPCWSTR pName) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RemoveFilter(IBaseFilter* pFilter) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFilters(IEnumFilters** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new MockEnumFilters(filters);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindFilterByName(LPCWSTR pName, IBaseFilter** ppFilter) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ConnectDirect(IPin* ppinOut, IPin* ppinIn, const AM_MEDIA_TYPE* pmt) override {
        if (failOutPin && failInPin && ppinOut == failOutPin && ppinIn == failInPin) return E_FAIL;
        ppinOut->ReceiveConnection(ppinIn, pmt);
        ppinIn->ReceiveConnection(ppinOut, pmt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reconnect(IPin* ppin) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Disconnect(IPin* ppin) override {
        if (ppin) ppin->Disconnect();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDefaultSyncSource() override { return S_OK; }

    // IGraphBuilder
    HRESULT STDMETHODCALLTYPE Connect(IPin* ppinOut, IPin* ppinIn) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Render(IPin* ppinOut) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RenderFile(LPCWSTR lpcwstrFile, LPCWSTR lpcwstrPlayList) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddSourceFilter(LPCWSTR lpcwstrFileName, LPCWSTR lpcwstrFilterName, IBaseFilter** ppFilter) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetLogFile(DWORD_PTR hFile) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Abort() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShouldOperationContinue() override { return S_OK; }
};

void testDirectShowEqBypassMock() {
    std::printf("[TEST 6] Mock DirectShow Graph & EQ Bypass Verification...\n");

    // Case 6A: Successful EQ bypass
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        auto* seam = new MockFilter(dseeUnified::seamlessId);
        auto* seamOut = new MockPin(PINDIR_OUTPUT, seam);
        seam->addPin(seamOut);
        graph->addFilter(seam);
        seam->Release();

        auto* eq = new MockFilter(dseeUnified::eqId);
        auto* eqIn = new MockPin(PINDIR_INPUT, eq);
        auto* eqOut = new MockPin(PINDIR_OUTPUT, eq);
        eq->addPin(eqIn);
        eq->addPin(eqOut);
        graph->addFilter(eq);
        eq->Release();

        auto* ds = new MockFilter(CLSID_DSoundRender);
        auto* dsIn = new MockPin(PINDIR_INPUT, ds);
        ds->addPin(dsIn);
        graph->addFilter(ds);
        ds->Release();

        // Connect Seamless -> EQ and EQ -> DirectSound
        seamOut->ReceiveConnection(eqIn, nullptr);
        eqIn->ReceiveConnection(seamOut, nullptr);
        eqOut->ReceiveConnection(dsIn, nullptr);
        dsIn->ReceiveConnection(eqOut, nullptr);

        // Apply bypass
        auto res = dseeUnified::applyEqBypass(graph.Get());
        CHECK(res.outcome == dseeUnified::RouteOutcome::Bypassed);
        CHECK(res.change == S_OK);
        CHECK(res.outputRate == 88200);
        CHECK(res.outputBits == 24);
        CHECK(res.outputChannels == 2);

        // Verify Seamless is now connected directly to DirectSound
        Microsoft::WRL::ComPtr<IPin> peer;
        CHECK(SUCCEEDED(seamOut->ConnectedTo(&peer)) && peer.Get() == dsIn);
        peer.Reset();
        CHECK(SUCCEEDED(dsIn->ConnectedTo(&peer)) && peer.Get() == seamOut);

        // Calling applyEqBypass again should report AlreadyBypassed
        auto res2 = dseeUnified::applyEqBypass(graph.Get());
        CHECK(res2.outcome == dseeUnified::RouteOutcome::AlreadyBypassed);
        CHECK(res2.outputRate == 88200);
        CHECK(res2.outputBits == 24);
    }

    // Case 6B: Direct connection failure and rollback
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        auto* seam = new MockFilter(dseeUnified::seamlessId);
        auto* seamOut = new MockPin(PINDIR_OUTPUT, seam);
        seam->addPin(seamOut);
        graph->addFilter(seam);
        seam->Release();

        auto* eq = new MockFilter(dseeUnified::eqId);
        auto* eqIn = new MockPin(PINDIR_INPUT, eq);
        auto* eqOut = new MockPin(PINDIR_OUTPUT, eq);
        eq->addPin(eqIn);
        eq->addPin(eqOut);
        graph->addFilter(eq);
        eq->Release();

        auto* ds = new MockFilter(CLSID_DSoundRender);
        auto* dsIn = new MockPin(PINDIR_INPUT, ds);
        ds->addPin(dsIn);
        graph->addFilter(ds);
        ds->Release();

        seamOut->ReceiveConnection(eqIn, nullptr);
        eqIn->ReceiveConnection(seamOut, nullptr);
        eqOut->ReceiveConnection(dsIn, nullptr);
        dsIn->ReceiveConnection(eqOut, nullptr);

        // Simulate failure when connecting directly to DirectSound
        graph->failOutPin = seamOut;
        graph->failInPin = dsIn;

        auto res = dseeUnified::applyEqBypass(graph.Get());
        CHECK(res.outcome == dseeUnified::RouteOutcome::Restored);
        CHECK(res.change == E_FAIL);
        CHECK(res.restore == S_OK);

        // Verify rollback: Seamless -> EQ and EQ -> DirectSound are reconnected
        Microsoft::WRL::ComPtr<IPin> peer;
        CHECK(SUCCEEDED(seamOut->ConnectedTo(&peer)) && peer.Get() == eqIn);
        peer.Reset();
        CHECK(SUCCEEDED(eqOut->ConnectedTo(&peer)) && peer.Get() == dsIn);
    }

    // Case 6C: Topology B - DSEE connected directly to EQ (SeamlessFilter omitted / format switch)
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        auto* dsee = new MockFilter(dseeUnified::dseeId);
        auto* dseeOut = new MockPin(PINDIR_OUTPUT, dsee, 88200, 24);
        dsee->addPin(dseeOut);
        graph->addFilter(dsee);
        dsee->Release();

        auto* eq = new MockFilter(dseeUnified::eqId);
        auto* eqIn = new MockPin(PINDIR_INPUT, eq, 88200, 24);
        auto* eqOut = new MockPin(PINDIR_OUTPUT, eq, 88200, 24);
        eq->addPin(eqIn);
        eq->addPin(eqOut);
        graph->addFilter(eq);
        eq->Release();

        auto* ds = new MockFilter(CLSID_DSoundRender);
        auto* dsIn = new MockPin(PINDIR_INPUT, ds, 88200, 24);
        ds->addPin(dsIn);
        graph->addFilter(ds);
        ds->Release();

        // Connect DSEE -> EQ and EQ -> DirectSound
        dseeOut->ReceiveConnection(eqIn, nullptr);
        eqIn->ReceiveConnection(dseeOut, nullptr);
        eqOut->ReceiveConnection(dsIn, nullptr);
        dsIn->ReceiveConnection(eqOut, nullptr);

        // Apply bypass
        auto res = dseeUnified::applyEqBypass(graph.Get());
        CHECK(res.outcome == dseeUnified::RouteOutcome::Bypassed);
        CHECK(res.change == S_OK);
        CHECK(res.outputRate == 88200);
        CHECK(res.outputBits == 24);

        // Verify DSEE is now connected directly to DirectSound
        Microsoft::WRL::ComPtr<IPin> peer;
        CHECK(SUCCEEDED(dseeOut->ConnectedTo(&peer)) && peer.Get() == dsIn);
        peer.Reset();
        CHECK(SUCCEEDED(dsIn->ConnectedTo(&peer)) && peer.Get() == dseeOut);

        // Calling applyEqBypass again should report AlreadyBypassed
        auto res2 = dseeUnified::applyEqBypass(graph.Get());
        CHECK(res2.outcome == dseeUnified::RouteOutcome::AlreadyBypassed);
        CHECK(res2.outputRate == 88200);
        CHECK(res2.outputBits == 24);
    }
}

// Test 7: Live Sony Filter Negotiation Verification (if OmgDseeHxFilter.ax is present)
void testLiveSonyFilterIfAvailable() {
    std::printf("[TEST 7] Live Sony Filter Negotiation Verification...\n");

    HMODULE module = LoadLibraryExW(L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\OmgDseeHxFilter.ax",
                                   nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        std::printf("  [SKIP] OmgDseeHxFilter.ax not found on system (optional on non-Sony test env).\n");
        return;
    }

    auto get = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
        GetProcAddress(module, "DllGetClassObject"));
    CHECK(get != nullptr);

    Microsoft::WRL::ComPtr<IClassFactory> factory;
    Microsoft::WRL::ComPtr<IUnknown> control;
    IID iid;
    CLSIDFromString(L"{6AF76DBE-8634-4FD3-93ED-982EDE1BFDD5}", &iid);

    HRESULT hr = get(dseeUnified::dseeId, IID_PPV_ARGS(&factory));
    CHECK(SUCCEEDED(hr));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateInstance(nullptr, iid, reinterpret_cast<void**>(control.GetAddressOf()));
        CHECK(SUCCEEDED(hr));
    }

    if (control) {
        using DseeSetter = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, dseeUnified::DseeConfig);
        auto fn = reinterpret_cast<DseeSetter>((*reinterpret_cast<void***>(control.Get()))[3]);
        CHECK(fn != nullptr);

        // Live test 7A: 44.1kHz / 16bit input -> Family2x (88.2kHz / 24bit)
        dseeUnified::DseeConfig req44{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj44{};
        dseeUnified::computeUnifiedDseeConfig(req44, adj44, dseeUnified::HarmonizeMode::Family2x);
        HRESULT hr44 = fn(control.Get(), adj44);
        CHECK(SUCCEEDED(hr44));

        // Live test 7B: 48kHz / 16bit input -> Family2x (96kHz / 24bit)
        dseeUnified::DseeConfig req48{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj48{};
        dseeUnified::computeUnifiedDseeConfig(req48, adj48, dseeUnified::HarmonizeMode::Family2x);
        HRESULT hr48 = fn(control.Get(), adj48);
        CHECK(SUCCEEDED(hr48));

        // Live test 7C: 44.1kHz / 24bit input -> Family2x (88.2kHz / 24bit)
        dseeUnified::DseeConfig req44_24{1, 2, 16, 44100, 44100, 24, 24, 0, 0};
        dseeUnified::DseeConfig adj44_24{};
        dseeUnified::computeUnifiedDseeConfig(req44_24, adj44_24, dseeUnified::HarmonizeMode::Family2x);
        HRESULT hr44_24 = fn(control.Get(), adj44_24);
        CHECK(SUCCEEDED(hr44_24));

        // Live test 7D: 48kHz / 24bit input -> Family2x (96kHz / 24bit)
        dseeUnified::DseeConfig req48_24{1, 2, 16, 48000, 48000, 24, 24, 0, 0};
        dseeUnified::DseeConfig adj48_24{};
        dseeUnified::computeUnifiedDseeConfig(req48_24, adj48_24, dseeUnified::HarmonizeMode::Family2x);
        HRESULT hr48_24 = fn(control.Get(), adj48_24);
        CHECK(SUCCEEDED(hr48_24));

        // Live test 7E: DSEE OFF pass-through (mode = 0)
        dseeUnified::DseeConfig offReq{0, 2, 16, 44100, 44100, 16, 16, 0, 0};
        HRESULT hrOff = fn(control.Get(), offReq);
        CHECK(SUCCEEDED(hrOff));
    }

    control.Reset();
    factory.Reset();
    FreeLibrary(module);
}

static bool testPinAcceptFormat(IPin* pin, DWORD sampleRate, WORD bits, WORD channels = 2) {
    if (!pin) return false;
    AM_MEDIA_TYPE mt{};
    mt.majortype = MEDIATYPE_Audio;
    mt.subtype = MEDIASUBTYPE_PCM;
    mt.formattype = FORMAT_WaveFormatEx;
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = channels * (bits / 8);

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = sampleRate;
    wfx.wBitsPerSample = bits;
    wfx.nBlockAlign = channels * (bits / 8);
    wfx.nAvgBytesPerSec = sampleRate * wfx.nBlockAlign;
    wfx.cbSize = 0;

    mt.cbFormat = sizeof(wfx);
    mt.pbFormat = reinterpret_cast<BYTE*>(&wfx);

    HRESULT hr = pin->QueryAccept(&mt);
    return hr == S_OK;
}

struct CFactoryTemplateProbe {
    const WCHAR* m_Name;
    const CLSID* m_ClsID;
    void* m_lpfnNew;
    void* m_lpfnInit;
    void* m_pAMovieSetup_Filter;
};

static void scanFilterTemplates(const wchar_t* dllName) {
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\%s", dllName);
    HMODULE mod = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) return;

    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(mod) + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    BYTE* textStart = nullptr;
    BYTE* textEnd = nullptr;
    BYTE* rdataStart = nullptr;
    BYTE* rdataEnd = nullptr;

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            textStart = reinterpret_cast<BYTE*>(mod) + sec[i].VirtualAddress;
            textEnd = textStart + sec[i].Misc.VirtualSize;
        } else if (memcmp(sec[i].Name, ".rdata", 6) == 0) {
            rdataStart = reinterpret_cast<BYTE*>(mod) + sec[i].VirtualAddress;
            rdataEnd = rdataStart + sec[i].Misc.VirtualSize;
        }
    }

    BYTE* modStart = reinterpret_cast<BYTE*>(mod);
    BYTE* modEnd = modStart + nt->OptionalHeader.SizeOfImage;

    if (rdataStart && rdataEnd && textStart && textEnd) {
        for (BYTE* p = rdataStart; p + sizeof(CFactoryTemplateProbe) <= rdataEnd; p += 4) {
            auto t = reinterpret_cast<const CFactoryTemplateProbe*>(p);
            auto namePtr = reinterpret_cast<const BYTE*>(t->m_Name);
            auto clsidPtr = reinterpret_cast<const BYTE*>(t->m_ClsID);
            auto newPtr = reinterpret_cast<const BYTE*>(t->m_lpfnNew);

            if (namePtr >= rdataStart && namePtr < rdataEnd &&
                clsidPtr >= rdataStart && clsidPtr < rdataEnd &&
                newPtr >= textStart && newPtr < textEnd) {
                // Check if name is valid wide string
                auto ws = reinterpret_cast<const wchar_t*>(namePtr);
                size_t len = 0;
                bool ok = true;
                while (len < 64 && ws[len] != 0) {
                    if (ws[len] < 32 || ws[len] > 126) { ok = false; break; }
                    len++;
                }
                if (ok && len >= 3 && ws[len] == 0) {
                    LPOLESTR clsidStr = nullptr;
                    StringFromCLSID(*reinterpret_cast<const CLSID*>(clsidPtr), &clsidStr);
                    std::printf("  [%ls] Found: '%ls', CLSID: %ls\n", dllName, ws, clsidStr ? clsidStr : L"(null)");
                    if (clsidStr) CoTaskMemFree(clsidStr);
                }
            }
        }
    }
    FreeLibrary(mod);
}

void testLiveSonyFilterFormatsAcceptance() {
    std::printf("[TEST 7.5] Live Sony Filters Format Acceptance Probe...\n");

    scanFilterTemplates(L"OmgFsConvFilter.ax");
    scanFilterTemplates(L"OmgAudioMix.ax");
    scanFilterTemplates(L"SeamlessFilter.ax");
    scanFilterTemplates(L"OmgEqxlFilter.ax");
    scanFilterTemplates(L"OmgDseeHxFilter.ax");
    scanFilterTemplates(L"saliconv.ax");
    scanFilterTemplates(L"saloconv.ax");
    scanFilterTemplates(L"OmgSaEnvelope.ax");
    scanFilterTemplates(L"OmgNormalizer.ax");

    // 1. Probe SeamlessFilter.ax
    {
        HMODULE mod = LoadLibraryExW(L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\SeamlessFilter.ax",
                                     nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (mod) {
            auto get = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
                GetProcAddress(mod, "DllGetClassObject"));
            if (get) {
                Microsoft::WRL::ComPtr<IClassFactory> factory;
                Microsoft::WRL::ComPtr<IBaseFilter> filter;
                if (SUCCEEDED(get(dseeUnified::seamlessId, IID_PPV_ARGS(&factory))) && factory) {
                    factory->CreateInstance(nullptr, IID_PPV_ARGS(&filter));
                }
                if (filter) {
                    Microsoft::WRL::ComPtr<IEnumPins> ep;
                    filter->EnumPins(&ep);
                    Microsoft::WRL::ComPtr<IPin> p;
                    std::printf("  SeamlessFilter All Pins:\n");
                    while (ep->Next(1, &p, nullptr) == S_OK) {
                        PIN_INFO pi{};
                        p->QueryPinInfo(&pi);
                        std::printf("    Pin name: %ls, dir: %s\n", pi.achName, pi.dir == PINDIR_INPUT ? "INPUT" : "OUTPUT");
                        if (pi.pFilter) pi.pFilter->Release();
                        p.Reset();
                    }
                    auto inPin = dseeUnified::findUniquePin(filter.Get(), PINDIR_INPUT);
                    auto outPin = dseeUnified::findUniquePin(filter.Get(), PINDIR_OUTPUT);
                    std::printf("  SeamlessFilter Input Pin QueryAccept:\n");
                    std::printf("    44.1k/16b: %s\n", testPinAcceptFormat(inPin.Get(), 44100, 16) ? "ACCEPT" : "REJECT");
                    std::printf("    44.1k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 44100, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    88.2k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 88200, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    176.4k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 176400, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    48k/16b: %s\n", testPinAcceptFormat(inPin.Get(), 48000, 16) ? "ACCEPT" : "REJECT");
                    std::printf("    48k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 48000, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    96k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 96000, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    192k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 192000, 24) ? "ACCEPT" : "REJECT");
                }
            }
            FreeLibrary(mod);
        }
    }

    // 2. Probe OmgEqxlFilter.ax
    {
        HMODULE mod = LoadLibraryExW(L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\OmgEqxlFilter.ax",
                                     nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (mod) {
            auto get = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
                GetProcAddress(mod, "DllGetClassObject"));
            if (get) {
                Microsoft::WRL::ComPtr<IClassFactory> factory;
                Microsoft::WRL::ComPtr<IBaseFilter> filter;
                if (SUCCEEDED(get(dseeUnified::eqId, IID_PPV_ARGS(&factory))) && factory) {
                    factory->CreateInstance(nullptr, IID_PPV_ARGS(&filter));
                }
                if (filter) {
                    auto inPin = dseeUnified::findUniquePin(filter.Get(), PINDIR_INPUT);
                    std::printf("  OmgEqxlFilter Input Pin QueryAccept:\n");
                    std::printf("    44.1k/16b: %s\n", testPinAcceptFormat(inPin.Get(), 44100, 16) ? "ACCEPT" : "REJECT");
                    std::printf("    44.1k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 44100, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    88.2k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 88200, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    48k/16b: %s\n", testPinAcceptFormat(inPin.Get(), 48000, 16) ? "ACCEPT" : "REJECT");
                    std::printf("    48k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 48000, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    96k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 96000, 24) ? "ACCEPT" : "REJECT");
                }
            }
            FreeLibrary(mod);
        }
    }

    // 3. Probe OmgDseeHxFilter.ax
    {
        HMODULE mod = LoadLibraryExW(L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\OmgDseeHxFilter.ax",
                                     nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (mod) {
            auto get = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
                GetProcAddress(mod, "DllGetClassObject"));
            if (get) {
                Microsoft::WRL::ComPtr<IClassFactory> factory;
                Microsoft::WRL::ComPtr<IBaseFilter> filter;
                if (SUCCEEDED(get(dseeUnified::dseeId, IID_PPV_ARGS(&factory))) && factory) {
                    factory->CreateInstance(nullptr, IID_PPV_ARGS(&filter));
                }
                if (filter) {
                    Microsoft::WRL::ComPtr<IEnumPins> ep;
                    filter->EnumPins(&ep);
                    Microsoft::WRL::ComPtr<IPin> p;
                    std::printf("  SeamlessFilter All Pins:\n");
                    while (ep->Next(1, &p, nullptr) == S_OK) {
                        PIN_INFO pi{};
                        p->QueryPinInfo(&pi);
                        std::printf("    Pin name: %ls, dir: %s\n", pi.achName, pi.dir == PINDIR_INPUT ? "INPUT" : "OUTPUT");
                        if (pi.pFilter) pi.pFilter->Release();
                        p.Reset();
                    }
                    auto inPin = dseeUnified::findUniquePin(filter.Get(), PINDIR_INPUT);
                    auto outPin = dseeUnified::findUniquePin(filter.Get(), PINDIR_OUTPUT);
                    std::printf("  OmgDseeHxFilter Input Pin QueryAccept:\n");
                    std::printf("    44.1k/16b: %s\n", testPinAcceptFormat(inPin.Get(), 44100, 16) ? "ACCEPT" : "REJECT");
                    std::printf("    44.1k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 44100, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    88.2k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 88200, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    48k/16b: %s\n", testPinAcceptFormat(inPin.Get(), 48000, 16) ? "ACCEPT" : "REJECT");
                    std::printf("    48k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 48000, 24) ? "ACCEPT" : "REJECT");
                    std::printf("  OmgDseeHxFilter Output Pin QueryAccept:\n");
                    std::printf("    44.1k/24b: %s\n", testPinAcceptFormat(outPin.Get(), 44100, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    88.2k/24b: %s\n", testPinAcceptFormat(outPin.Get(), 88200, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    176.4k/24b: %s\n", testPinAcceptFormat(outPin.Get(), 176400, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    96k/24b: %s\n", testPinAcceptFormat(outPin.Get(), 96000, 24) ? "ACCEPT" : "REJECT");
                    std::printf("    192k/24b: %s\n", testPinAcceptFormat(outPin.Get(), 192000, 24) ? "ACCEPT" : "REJECT");
                }
            }
            FreeLibrary(mod);
        }
    }

    // 4. Probe DirectSoundRender
    {
        Microsoft::WRL::ComPtr<IBaseFilter> dsFilter;
        HRESULT hr = CoCreateInstance(CLSID_DSoundRender, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dsFilter));
        if (SUCCEEDED(hr) && dsFilter) {
            auto inPin = dseeUnified::findUniquePin(dsFilter.Get(), PINDIR_INPUT);
            std::printf("  DirectSoundRender Input Pin QueryAccept:\n");
            std::printf("    44.1k/16b:  %s\n", testPinAcceptFormat(inPin.Get(), 44100, 16) ? "ACCEPT" : "REJECT");
            std::printf("    44.1k/24b:  %s\n", testPinAcceptFormat(inPin.Get(), 44100, 24) ? "ACCEPT" : "REJECT");
            std::printf("    88.2k/24b:  %s\n", testPinAcceptFormat(inPin.Get(), 88200, 24) ? "ACCEPT" : "REJECT");
            std::printf("    96k/24b:    %s\n", testPinAcceptFormat(inPin.Get(), 96000, 24) ? "ACCEPT" : "REJECT");
            std::printf("    176.4k/24b: %s\n", testPinAcceptFormat(inPin.Get(), 176400, 24) ? "ACCEPT" : "REJECT");
            std::printf("    192k/24b:   %s\n", testPinAcceptFormat(inPin.Get(), 192000, 24) ? "ACCEPT" : "REJECT");
        }
    }
}

// Test 8: Decoder Bit Depth Synchronization & 48k Formats (Issue #43)
void testDecoderBitsSyncAndUnifiedFormats() {
    std::printf("[TEST 8] Decoder Bit Depth Synchronization & 48k Formats...\n");

    // 8A: 48kHz / 16bit input
    dseeUnified::setActiveDecoderBits(16);
    dseeUnified::DseeConfig req16{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
    dseeUnified::DseeConfig adj16{};
    CHECK(dseeUnified::computeUnifiedDseeConfig(req16, adj16, dseeUnified::HarmonizeMode::Target96k));
    CHECK(adj16.inputBits == 16);
    CHECK(adj16.outputRate == 96000);
    CHECK(adj16.outputBits == 24);

    // 8B: 48kHz / 24bit native FLAC where OpcFlac requested 16-bit inputBits
    dseeUnified::setActiveDecoderBits(24);
    dseeUnified::DseeConfig req24_mismatch{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
    dseeUnified::DseeConfig adj24{};
    CHECK(dseeUnified::computeUnifiedDseeConfig(req24_mismatch, adj24, dseeUnified::HarmonizeMode::Target96k));
    // Synchronized inputBits must be 24
    CHECK(adj24.inputBits == 24);
    CHECK(adj24.outputRate == 96000);
    CHECK(adj24.outputBits == 24);

    // Both reach identical renderer format
    CHECK(adj16.outputRate == adj24.outputRate);
    CHECK(adj16.outputBits == adj24.outputBits);

    // 8C: Native24b mode (48kHz / 24bit output)
    dseeUnified::DseeConfig adj16_native{};
    dseeUnified::DseeConfig adj24_native{};
    CHECK(dseeUnified::computeUnifiedDseeConfig(req16, adj16_native, dseeUnified::HarmonizeMode::Native24b, 16));
    CHECK(dseeUnified::computeUnifiedDseeConfig(req24_mismatch, adj24_native, dseeUnified::HarmonizeMode::Native24b, 24));
    CHECK(adj16_native.outputRate == 48000);
    CHECK(adj16_native.outputBits == 24);
    CHECK(adj24_native.outputRate == 48000);
    CHECK(adj24_native.outputBits == 24);
    CHECK(adj16_native.outputRate == adj24_native.outputRate);
}

// Test 9: Full Graph Format Inspection Coverage (Issue #43)
void testGraphInspectionCoverage() {
    std::printf("[TEST 9] Full Graph Format Inspection Coverage...\n");

    Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

    // Source (FLAC)
    CLSID flacClsid{};
    CLSIDFromString(L"{028A95AA-C956-4874-AD10-C657DB1B00F4}", &flacClsid);
    auto* src = new MockFilter(flacClsid);
    auto* srcOut = new MockPin(PINDIR_OUTPUT, src, 48000, 24);
    src->addPin(srcOut);
    graph->addFilter(src);
    src->Release();

    // DSEE
    auto* dsee = new MockFilter(dseeUnified::dseeId);
    auto* dseeIn = new MockPin(PINDIR_INPUT, dsee, 48000, 24);
    auto* dseeOut = new MockPin(PINDIR_OUTPUT, dsee, 96000, 24);
    dsee->addPin(dseeIn);
    dsee->addPin(dseeOut);
    graph->addFilter(dsee);
    dsee->Release();

    // Seamless
    auto* seam = new MockFilter(dseeUnified::seamlessId);
    auto* seamIn = new MockPin(PINDIR_INPUT, seam, 96000, 24);
    auto* seamOut = new MockPin(PINDIR_OUTPUT, seam, 96000, 24);
    seam->addPin(seamIn);
    seam->addPin(seamOut);
    graph->addFilter(seam);
    seam->Release();

    // EQ
    auto* eq = new MockFilter(dseeUnified::eqId);
    auto* eqIn = new MockPin(PINDIR_INPUT, eq, 96000, 24);
    auto* eqOut = new MockPin(PINDIR_OUTPUT, eq, 96000, 24);
    eq->addPin(eqIn);
    eq->addPin(eqOut);
    graph->addFilter(eq);
    eq->Release();

    // DirectSound
    auto* ds = new MockFilter(CLSID_DSoundRender);
    auto* dsIn = new MockPin(PINDIR_INPUT, ds, 96000, 24);
    ds->addPin(dsIn);
    graph->addFilter(ds);
    ds->Release();

    // Connect pipeline: FLAC -> DSEE -> Seamless -> EQ -> DirectSound
    srcOut->ReceiveConnection(dseeIn, nullptr);
    dseeIn->ReceiveConnection(srcOut, nullptr);
    dseeOut->ReceiveConnection(seamIn, nullptr);
    seamIn->ReceiveConnection(dseeOut, nullptr);
    seamOut->ReceiveConnection(eqIn, nullptr);
    eqIn->ReceiveConnection(seamOut, nullptr);
    eqOut->ReceiveConnection(dsIn, nullptr);
    dsIn->ReceiveConnection(eqOut, nullptr);

    // Inspect
    dseeUnified::GraphInspection insp{};
    CHECK(dseeUnified::inspectGraphFormats(graph.Get(), insp));

    // Verify 5 stages
    CHECK(insp.decodePcm.present && insp.decodePcm.connected);
    CHECK(insp.decodePcm.sampleRate == 48000 && insp.decodePcm.bitDepth == 24);

    CHECK(insp.dseeIn.present && insp.dseeIn.connected);
    CHECK(insp.dseeIn.sampleRate == 48000 && insp.dseeIn.bitDepth == 24);
    CHECK(insp.dseeOut.present && insp.dseeOut.connected);
    CHECK(insp.dseeOut.sampleRate == 96000 && insp.dseeOut.bitDepth == 24);

    CHECK(insp.seamlessIn.present && insp.seamlessIn.connected);
    CHECK(insp.seamlessIn.sampleRate == 96000 && insp.seamlessIn.bitDepth == 24);
    CHECK(insp.seamlessOut.present && insp.seamlessOut.connected);
    CHECK(insp.seamlessOut.sampleRate == 96000 && insp.seamlessOut.bitDepth == 24);

    CHECK(insp.eqIn.present && insp.eqIn.connected);
    CHECK(insp.eqIn.sampleRate == 96000 && insp.eqIn.bitDepth == 24);
    CHECK(insp.eqOut.present && insp.eqOut.connected);
    CHECK(insp.eqOut.sampleRate == 96000 && insp.eqOut.bitDepth == 24);

    CHECK(insp.renderIn.present && insp.renderIn.connected);
    CHECK(insp.renderIn.sampleRate == 96000 && insp.renderIn.bitDepth == 24);
    CHECK(!insp.isDirectSeamless);

    // Apply EQ bypass and inspect again
    auto bypassRes = dseeUnified::applyEqBypass(graph.Get());
    CHECK(bypassRes.outcome == dseeUnified::RouteOutcome::Bypassed);

    dseeUnified::GraphInspection inspBypassed{};
    CHECK(dseeUnified::inspectGraphFormats(graph.Get(), inspBypassed));
    CHECK(inspBypassed.isDirectSeamless);
    CHECK(inspBypassed.eqIn.present && !inspBypassed.eqIn.connected);
    CHECK(inspBypassed.renderIn.present && inspBypassed.renderIn.connected);
    CHECK(inspBypassed.renderIn.sampleRate == 96000 && inspBypassed.renderIn.bitDepth == 24);
}

// Test 10: Cross-Family (44.1k vs 48k) Alignment Boundary (Issue #43)
void testCrossFamilyLimitation() {
    std::printf("[TEST 10] Cross-Family (44.1k vs 48k) Alignment Boundary...\n");

    dseeUnified::DseeConfig req44{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
    dseeUnified::DseeConfig req48{1, 2, 16, 48000, 48000, 16, 16, 0, 0};

    // Family2x gives 88.2k vs 96k (different rates)
    dseeUnified::DseeConfig adj44_2x{}, adj48_2x{};
    CHECK(dseeUnified::computeUnifiedDseeConfig(req44, adj44_2x, dseeUnified::HarmonizeMode::Family2x));
    CHECK(dseeUnified::computeUnifiedDseeConfig(req48, adj48_2x, dseeUnified::HarmonizeMode::Family2x));
    CHECK(adj44_2x.outputRate != adj48_2x.outputRate);

    // Target96k gives 96k for 48k
    dseeUnified::DseeConfig adj48_96k{};
    CHECK(dseeUnified::computeUnifiedDseeConfig(req48, adj48_96k, dseeUnified::HarmonizeMode::Target96k));
    CHECK(adj48_96k.outputRate == 96000);
}

// Test 11: Issue #44 Negotiation Matrix & Family Mode Verification
void testIssue44NegotiationMatrix() {
    std::printf("[TEST 11] Issue #44 Negotiation Matrix & Family Modes...\n");

    // Formats matrix:
    // 44.1kHz / 16bit
    // 44.1kHz / 24bit
    // 48kHz / 16bit
    // 48kHz / 24bit
    // 96kHz / 24bit (Hi-Res native)
    // 192kHz / 24bit (Hi-Res native)

    dseeUnified::DseeConfig req44_16{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
    dseeUnified::DseeConfig req44_24{1, 2, 24, 44100, 44100, 24, 16, 0, 0};
    dseeUnified::DseeConfig req48_16{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
    dseeUnified::DseeConfig req48_24{1, 2, 24, 48000, 48000, 24, 16, 0, 0};
    dseeUnified::DseeConfig req96_24{1, 2, 24, 96000, 96000, 24, 16, 0, 0};
    dseeUnified::DseeConfig req192_24{1, 2, 24, 192000, 192000, 24, 16, 0, 0};

    // 1. Family2x Mode (Default standard output policy for Issue #44):
    // 44.1kHz family -> 88.2kHz / 24-bit
    // 48kHz family -> 96kHz / 24-bit
    {
        dseeUnified::DseeConfig out44_16{}, out44_24{}, out48_16{}, out48_24{};

        CHECK(dseeUnified::computeUnifiedDseeConfig(req44_16, out44_16, dseeUnified::HarmonizeMode::Family2x));
        CHECK(out44_16.outputRate == 88200);
        CHECK(out44_16.outputBits == 24);

        CHECK(dseeUnified::computeUnifiedDseeConfig(req44_24, out44_24, dseeUnified::HarmonizeMode::Family2x));
        CHECK(out44_24.outputRate == 88200);
        CHECK(out44_24.outputBits == 24);

        CHECK(dseeUnified::computeUnifiedDseeConfig(req48_16, out48_16, dseeUnified::HarmonizeMode::Family2x));
        CHECK(out48_16.outputRate == 96000);
        CHECK(out48_16.outputBits == 24);

        CHECK(dseeUnified::computeUnifiedDseeConfig(req48_24, out48_24, dseeUnified::HarmonizeMode::Family2x));
        CHECK(out48_24.outputRate == 96000);
        CHECK(out48_24.outputBits == 24);
    }

    // 2. Family4x Mode (Candidate output policy):
    // 44.1kHz family -> 176.4kHz / 24-bit
    // 48kHz family -> 192kHz / 24-bit
    {
        dseeUnified::DseeConfig out44_16{}, out44_24{}, out48_16{}, out48_24{};

        CHECK(dseeUnified::computeUnifiedDseeConfig(req44_16, out44_16, dseeUnified::HarmonizeMode::Family4x));
        CHECK(out44_16.outputRate == 176400);
        CHECK(out44_16.outputBits == 24);

        CHECK(dseeUnified::computeUnifiedDseeConfig(req44_24, out44_24, dseeUnified::HarmonizeMode::Family4x));
        CHECK(out44_24.outputRate == 176400);
        CHECK(out44_24.outputBits == 24);

        CHECK(dseeUnified::computeUnifiedDseeConfig(req48_16, out48_16, dseeUnified::HarmonizeMode::Family4x));
        CHECK(out48_16.outputRate == 192000);
        CHECK(out48_16.outputBits == 24);

        CHECK(dseeUnified::computeUnifiedDseeConfig(req48_24, out48_24, dseeUnified::HarmonizeMode::Family4x));
        CHECK(out48_24.outputRate == 192000);
        CHECK(out48_24.outputBits == 24);
    }

    // 3. Hi-Res Native (96kHz & 192kHz) pass-through:
    // DSEE HX should not process native hi-res formats (computeUnifiedDseeConfig returns false)
    {
        dseeUnified::DseeConfig out96{}, out192{};
        CHECK(!dseeUnified::computeUnifiedDseeConfig(req96_24, out96, dseeUnified::HarmonizeMode::Family2x));
        CHECK(!dseeUnified::computeUnifiedDseeConfig(req192_24, out192, dseeUnified::HarmonizeMode::Family2x));
        CHECK(!dseeUnified::computeUnifiedDseeConfig(req96_24, out96, dseeUnified::HarmonizeMode::Family4x));
        CHECK(!dseeUnified::computeUnifiedDseeConfig(req192_24, out192, dseeUnified::HarmonizeMode::Family4x));
    }

    // 4. Topology B Format Inspection (Direct DSEE -> DirectSound without SeamlessFilter):
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        auto* dsee = new MockFilter(dseeUnified::dseeId);
        auto* dseeIn = new MockPin(PINDIR_INPUT, dsee, 44100, 16);
        auto* dseeOut = new MockPin(PINDIR_OUTPUT, dsee, 88200, 24);
        dsee->addPin(dseeIn);
        dsee->addPin(dseeOut);
        graph->addFilter(dsee);
        dsee->Release();

        auto* ds = new MockFilter(CLSID_DSoundRender);
        auto* dsIn = new MockPin(PINDIR_INPUT, ds, 88200, 24);
        ds->addPin(dsIn);
        graph->addFilter(ds);
        ds->Release();

        // Connect DSEE directly to DirectSound (as after applyEqBypass in Topology B)
        dseeOut->ReceiveConnection(dsIn, nullptr);
        dsIn->ReceiveConnection(dseeOut, nullptr);

        dseeUnified::GraphInspection insp{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), insp));
        CHECK(insp.isDirectDsee);
        CHECK(insp.eqBypassed);
        CHECK(!insp.isDirectSeamless);
        CHECK(insp.dseeOut.sampleRate == 88200 && insp.dseeOut.bitDepth == 24);
        CHECK(insp.renderIn.sampleRate == 88200 && insp.renderIn.bitDepth == 24);
    }
}

void testDirectSoundRenderPinInspection() {
    std::printf("[TEST 12] Live DirectSoundRender & FilterGraph Key Inspection (Issue #45)...\n");

    // 1. Check FilterGraph COM canonical identity vs IGraphBuilder*
    {
        Microsoft::WRL::ComPtr<IGraphBuilder> graph;
        HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
        CHECK(SUCCEEDED(hr));
        if (SUCCEEDED(hr)) {
            Microsoft::WRL::ComPtr<IUnknown> unk;
            hr = graph->QueryInterface(IID_PPV_ARGS(&unk));
            CHECK(SUCCEEDED(hr));
            void* pGraph = graph.Get();
            void* pUnk = unk.Get();
            CHECK(pGraph != nullptr);
            CHECK(pUnk != nullptr);
            // On real DirectShow CFilterGraph, unk != pGraph due to multiple inheritance vtable layout.
            // Both must be accommodated by canonical key resolution.
        }
    }

    // 2. Check CLSID_DSoundRender pin and IMemInputPin
    {
        Microsoft::WRL::ComPtr<IBaseFilter> dsRender;
        HRESULT hr = CoCreateInstance(CLSID_DSoundRender, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dsRender));
        CHECK(SUCCEEDED(hr));
        if (SUCCEEDED(hr)) {
            Microsoft::WRL::ComPtr<IEnumPins> e;
            hr = dsRender->EnumPins(&e);
            CHECK(SUCCEEDED(hr));
            Microsoft::WRL::ComPtr<IPin> p;
            int inputPins = 0;
            while (e->Next(1, &p, nullptr) == S_OK) {
                PIN_DIRECTION dir{};
                p->QueryDirection(&dir);
                if (dir == PINDIR_INPUT) {
                    inputPins++;
                    Microsoft::WRL::ComPtr<IMemInputPin> mem;
                    HRESULT hrMem = p->QueryInterface(IID_PPV_ARGS(&mem));
                    CHECK(SUCCEEDED(hrMem));
                    CHECK(mem != nullptr);
                    if (mem) {
                        auto memVtable = *reinterpret_cast<void***>(mem.Get());
                        CHECK(memVtable[6] != nullptr); // Receive slot
                    }
                }
                p.Reset();
            }
            CHECK(inputPins == 1);
        }
    }
}

// Test 13: Issue #46 DSEE 88.2kHz Maintenance, Downsampler Bypass, & Full Chain Traversal
void testIssue46Dsee88kRendererAndChainTraversal() {
    std::printf("[TEST 13] Issue #46: DSEE 88.2kHz Maintenance & Peer Code 3 Diagnostics...\n");

    // Test 13A: 44.1kHz DSEE playback where downsampler was inserted before EQ
    // Graph topology before bypass:
    // Decoder (44.1k/16b) -> DSEE (88.2k/24b) -> Downsampler (44.1k/24b) -> EQ (44.1k/24b) -> DirectSoundRender
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        CLSID downsamplerId = { 0x12345678, 0x1234, 0x1234, { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0 } };

        auto* dec = new MockFilter(dseeUnified::flacId, L"OmgFlacSrc");
        auto* decOut = new MockPin(PINDIR_OUTPUT, dec, 44100, 16, L"Out");
        dec->addPin(decOut);
        graph->addFilter(dec);

        auto* dsee = new MockFilter(dseeUnified::dseeId, L"OmgDseeHxFilter");
        auto* dseeIn = new MockPin(PINDIR_INPUT, dsee, 44100, 16, L"In");
        auto* dseeOut = new MockPin(PINDIR_OUTPUT, dsee, 88200, 24, L"Out");
        dsee->addPin(dseeIn);
        dsee->addPin(dseeOut);
        graph->addFilter(dsee);

        auto* downsampler = new MockFilter(downsamplerId, L"OmgFsConvFilter");
        auto* downIn = new MockPin(PINDIR_INPUT, downsampler, 88200, 24, L"In");
        auto* downOut = new MockPin(PINDIR_OUTPUT, downsampler, 44100, 24, L"Out");
        downsampler->addPin(downIn);
        downsampler->addPin(downOut);
        graph->addFilter(downsampler);

        auto* eq = new MockFilter(dseeUnified::eqId, L"OmgEqxlFilter");
        auto* eqIn = new MockPin(PINDIR_INPUT, eq, 44100, 24, L"In");
        auto* eqOut = new MockPin(PINDIR_OUTPUT, eq, 44100, 24, L"Out");
        eq->addPin(eqIn);
        eq->addPin(eqOut);
        graph->addFilter(eq);

        auto* ds = new MockFilter(CLSID_DSoundRender, L"DirectSoundRender");
        auto* dsIn = new MockPin(PINDIR_INPUT, ds, 44100, 24, L"Audio Input pin (rendered)");
        ds->addPin(dsIn);
        graph->addFilter(ds);

        // Connect initial chain
        graph->ConnectDirect(decOut, dseeIn, nullptr);
        graph->ConnectDirect(dseeOut, downIn, nullptr);
        graph->ConnectDirect(downOut, eqIn, nullptr);
        graph->ConnectDirect(eqOut, dsIn, nullptr);

        // Verify initial graph inspection
        dseeUnified::GraphInspection inspBefore{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), inspBefore));
        CHECK(inspBefore.decodePcm.sampleRate == 44100 && inspBefore.decodePcm.bitDepth == 16);
        CHECK(inspBefore.dseeOut.sampleRate == 88200 && inspBefore.dseeOut.bitDepth == 24);
        CHECK(inspBefore.eqIn.sampleRate == 44100 && inspBefore.eqIn.bitDepth == 24);
        CHECK(inspBefore.renderIn.sampleRate == 44100 && inspBefore.renderIn.bitDepth == 24);
        CHECK(!inspBefore.eqBypassed);
        CHECK(wcscmp(inspBefore.renderPeerFilterName, L"OmgEqxlFilter") == 0);
        CHECK(inspBefore.chainLength == 5);
        CHECK(inspBefore.chain[0].clsid == dseeUnified::flacId);
        CHECK(inspBefore.chain[1].clsid == dseeUnified::dseeId);
        CHECK(inspBefore.chain[2].clsid == downsamplerId);
        CHECK(inspBefore.chain[2].inRate == 88200 && inspBefore.chain[2].outRate == 44100); // Discovers downsampler location!
        CHECK(inspBefore.chain[3].clsid == dseeUnified::eqId);
        CHECK(inspBefore.chain[4].clsid == CLSID_DSoundRender);

        // Apply EQ Bypass with Issue #46 logic:
        // Because DSEE is outputting 88.2k/24b, and Seamless is either absent or downsampled,
        // it must connect dseeOut DIRECTLY to dsIn!
        auto routeRes = dseeUnified::applyEqBypass(graph.Get());
        CHECK(routeRes.outcome == dseeUnified::RouteOutcome::Bypassed);
        CHECK(routeRes.change == S_OK);
        CHECK(routeRes.outputRate == 88200);
        CHECK(routeRes.outputBits == 24);

        // Verify direct DSEE connection
        Microsoft::WRL::ComPtr<IPin> currentPeer;
        CHECK(SUCCEEDED(dsIn->ConnectedTo(&currentPeer)) && currentPeer.Get() == dseeOut);

        // Inspect graph after bypass
        dseeUnified::GraphInspection inspAfter{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), inspAfter));
        CHECK(inspAfter.eqBypassed);
        CHECK(inspAfter.isDirectDsee);
        CHECK(!inspAfter.isDirectSeamless);
        CHECK(inspAfter.renderIn.sampleRate == 88200 && inspAfter.renderIn.bitDepth == 24);
        CHECK(inspAfter.renderPeerClsid == dseeUnified::dseeId);
        CHECK(wcscmp(inspAfter.renderPeerFilterName, L"OmgDseeHxFilter") == 0);

        DWORD peerCode = inspAfter.isDirectSeamless ? 1 : (inspAfter.isDirectDsee ? 2 : (inspAfter.eqBypassed ? 3 : 0));
        CHECK(peerCode == 2); // Peer Code 2 (Direct DSEE)

        // Calling applyEqBypass again should report AlreadyBypassed at 88.2kHz
        auto routeRes2 = dseeUnified::applyEqBypass(graph.Get());
        CHECK(routeRes2.outcome == dseeUnified::RouteOutcome::AlreadyBypassed);
        CHECK(routeRes2.outputRate == 88200);
        CHECK(routeRes2.outputBits == 24);
    }

    // Test 13B: 48kHz DSEE playback where SeamlessFilter successfully passes 96kHz
    // Graph topology:
    // Decoder (48k/24b) -> DSEE (96k/24b) -> Seamless (96k/24b) -> EQ (96k/24b) -> DirectSoundRender
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        auto* dec = new MockFilter(dseeUnified::flacId, L"OmgFlacSrc");
        auto* decOut = new MockPin(PINDIR_OUTPUT, dec, 48000, 24, L"Out");
        dec->addPin(decOut);
        graph->addFilter(dec);

        auto* dsee = new MockFilter(dseeUnified::dseeId, L"OmgDseeHxFilter");
        auto* dseeIn = new MockPin(PINDIR_INPUT, dsee, 48000, 24, L"In");
        auto* dseeOut = new MockPin(PINDIR_OUTPUT, dsee, 96000, 24, L"Out");
        dsee->addPin(dseeIn);
        dsee->addPin(dseeOut);
        graph->addFilter(dsee);

        auto* seam = new MockFilter(dseeUnified::seamlessId, L"SeamlessFilter");
        auto* seamIn = new MockPin(PINDIR_INPUT, seam, 96000, 24, L"In");
        auto* seamOut = new MockPin(PINDIR_OUTPUT, seam, 96000, 24, L"Out");
        seam->addPin(seamIn);
        seam->addPin(seamOut);
        graph->addFilter(seam);

        auto* eq = new MockFilter(dseeUnified::eqId, L"OmgEqxlFilter");
        auto* eqIn = new MockPin(PINDIR_INPUT, eq, 96000, 24, L"In");
        auto* eqOut = new MockPin(PINDIR_OUTPUT, eq, 96000, 24, L"Out");
        eq->addPin(eqIn);
        eq->addPin(eqOut);
        graph->addFilter(eq);

        auto* ds = new MockFilter(CLSID_DSoundRender, L"DirectSoundRender");
        auto* dsIn = new MockPin(PINDIR_INPUT, ds, 96000, 24, L"In");
        ds->addPin(dsIn);
        graph->addFilter(ds);

        graph->ConnectDirect(decOut, dseeIn, nullptr);
        graph->ConnectDirect(dseeOut, seamIn, nullptr);
        graph->ConnectDirect(seamOut, eqIn, nullptr);
        graph->ConnectDirect(eqOut, dsIn, nullptr);

        // Apply EQ Bypass: because seamOut matches DSEE 96kHz, seamOut connects to dsIn
        auto routeRes = dseeUnified::applyEqBypass(graph.Get());
        CHECK(routeRes.outcome == dseeUnified::RouteOutcome::Bypassed);
        CHECK(routeRes.outputRate == 96000);
        CHECK(routeRes.outputBits == 24);

        dseeUnified::GraphInspection insp{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), insp));
        CHECK(insp.isDirectSeamless);
        CHECK(!insp.isDirectDsee);
        CHECK(insp.renderIn.sampleRate == 96000 && insp.renderIn.bitDepth == 24);

        DWORD peerCode = insp.isDirectSeamless ? 1 : (insp.isDirectDsee ? 2 : (insp.eqBypassed ? 3 : 0));
        CHECK(peerCode == 1); // Peer Code 1 (Direct Seamless)
    }

    // Test 13C: DSEE OFF / Native Hi-Res Preservation (192kHz native)
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        auto* dec = new MockFilter(dseeUnified::flacId, L"OmgFlacSrc");
        auto* decOut = new MockPin(PINDIR_OUTPUT, dec, 192000, 24, L"Out");
        dec->addPin(decOut);
        graph->addFilter(dec);

        auto* seam = new MockFilter(dseeUnified::seamlessId, L"SeamlessFilter");
        auto* seamIn = new MockPin(PINDIR_INPUT, seam, 192000, 24, L"In");
        auto* seamOut = new MockPin(PINDIR_OUTPUT, seam, 192000, 24, L"Out");
        seam->addPin(seamIn);
        seam->addPin(seamOut);
        graph->addFilter(seam);

        auto* eq = new MockFilter(dseeUnified::eqId, L"OmgEqxlFilter");
        auto* eqIn = new MockPin(PINDIR_INPUT, eq, 192000, 24, L"In");
        auto* eqOut = new MockPin(PINDIR_OUTPUT, eq, 192000, 24, L"Out");
        eq->addPin(eqIn);
        eq->addPin(eqOut);
        graph->addFilter(eq);

        auto* ds = new MockFilter(CLSID_DSoundRender, L"DirectSoundRender");
        auto* dsIn = new MockPin(PINDIR_INPUT, ds, 192000, 24, L"In");
        ds->addPin(dsIn);
        graph->addFilter(ds);

        graph->ConnectDirect(decOut, seamIn, nullptr);
        graph->ConnectDirect(seamOut, eqIn, nullptr);
        graph->ConnectDirect(eqOut, dsIn, nullptr);

        auto routeRes = dseeUnified::applyEqBypass(graph.Get());
        CHECK(routeRes.outcome == dseeUnified::RouteOutcome::Bypassed);
        CHECK(routeRes.outputRate == 192000);
        CHECK(routeRes.outputBits == 24);

        dseeUnified::GraphInspection insp{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), insp));
        CHECK(insp.isDirectSeamless);
        CHECK(insp.renderIn.sampleRate == 192000 && insp.renderIn.bitDepth == 24);
        DWORD peerCode = insp.isDirectSeamless ? 1 : (insp.isDirectDsee ? 2 : (insp.eqBypassed ? 3 : 0));
        CHECK(peerCode == 1);
    }
}

// Test 15: Issue #49 Multi-Codec (FLAC, AAC, MP3, ALAC, WAV) Eligibility & Route Matrix
void testIssue49CodecEligibilityAndRouteMatrix() {
    std::printf("[TEST 15] Issue #49 Multi-Codec Eligibility & Route Matrix...\n");

    // 15A: Regression Prevention: Stale Decoder Bit Isolation Across Codec Switching
    // Simulate playing 24-bit FLAC first (sets activeDecoderBits to 24)
    dseeUnified::setActiveDecoderBits(24);
    CHECK(dseeUnified::getActiveDecoderBits().load() == 24);

    // FLAC 48kHz / 24bit calculation (using decBits = 24)
    {
        dseeUnified::DseeConfig reqFlac{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
        auto codecFlac = dseeUnified::CodecType::Flac;
        WORD decBits = (codecFlac == dseeUnified::CodecType::Flac)
            ? dseeUnified::getActiveDecoderBits().load()
            : static_cast<WORD>(reqFlac.inputBits);
        dseeUnified::DseeConfig adjFlac{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(reqFlac, adjFlac, dseeUnified::HarmonizeMode::Family2x, decBits));
        CHECK(adjFlac.inputBits == 24); // Synchronized to true FLAC decoded bits
        CHECK(adjFlac.outputRate == 96000);
        CHECK(adjFlac.outputBits == 24);
    }

    // Next track: AAC 44.1kHz / 16bit (OpcMp4.dll / OpcAac.dll)
    // The caller is AAC, so active decoder bits must NOT contaminate AAC
    {
        dseeUnified::DseeConfig reqAac{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
        auto codecAac = dseeUnified::CodecType::Aac;
        WORD decBits = (codecAac == dseeUnified::CodecType::Flac)
            ? dseeUnified::getActiveDecoderBits().load()
            : static_cast<WORD>(reqAac.inputBits);
        if (codecAac != dseeUnified::CodecType::Flac) {
            dseeUnified::resetActiveDecoderBits();
        }
        dseeUnified::DseeConfig adjAac{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(reqAac, adjAac, dseeUnified::HarmonizeMode::Family2x, decBits));
        // MUST remain 16-bit inputBits to prevent pin connection failure with OmgDseeHxFilter
        CHECK(adjAac.inputBits == 16);
        CHECK(adjAac.outputRate == 88200);
        CHECK(adjAac.outputBits == 24);
        // Active decoder bits reset
        CHECK(dseeUnified::getActiveDecoderBits().load() == 0);
    }

    // Next track: MP3 44.1kHz / 16bit (OpcCd3.dll / OpcMp3.dll)
    {
        dseeUnified::DseeConfig reqMp3{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
        auto codecMp3 = dseeUnified::CodecType::Mp3;
        WORD decBits = (codecMp3 == dseeUnified::CodecType::Flac)
            ? dseeUnified::getActiveDecoderBits().load()
            : static_cast<WORD>(reqMp3.inputBits);
        if (codecMp3 != dseeUnified::CodecType::Flac) {
            dseeUnified::resetActiveDecoderBits();
        }
        dseeUnified::DseeConfig adjMp3{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(reqMp3, adjMp3, dseeUnified::HarmonizeMode::Family2x, decBits));
        CHECK(adjMp3.inputBits == 16);
        CHECK(adjMp3.outputRate == 88200);
        CHECK(adjMp3.outputBits == 24);
    }

    // Next track: ALAC 44.1kHz / 24bit (OpcAlac.dll / OpcMp4.dll)
    {
        dseeUnified::DseeConfig reqAlac{1, 2, 16, 44100, 44100, 24, 24, 0, 0};
        auto codecAlac = dseeUnified::CodecType::Alac;
        WORD decBits = (codecAlac == dseeUnified::CodecType::Flac)
            ? dseeUnified::getActiveDecoderBits().load()
            : static_cast<WORD>(reqAlac.inputBits);
        dseeUnified::DseeConfig adjAlac{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(reqAlac, adjAlac, dseeUnified::HarmonizeMode::Family2x, decBits));
        CHECK(adjAlac.inputBits == 24);
        CHECK(adjAlac.outputRate == 88200);
        CHECK(adjAlac.outputBits == 24);
    }

    // Next track: WAV 48kHz / 16bit (OpcWavEx.dll)
    {
        dseeUnified::DseeConfig reqWav{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
        auto codecWav = dseeUnified::CodecType::Wav;
        WORD decBits = (codecWav == dseeUnified::CodecType::Flac)
            ? dseeUnified::getActiveDecoderBits().load()
            : static_cast<WORD>(reqWav.inputBits);
        dseeUnified::DseeConfig adjWav{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(reqWav, adjWav, dseeUnified::HarmonizeMode::Family2x, decBits));
        CHECK(adjWav.inputBits == 16);
        CHECK(adjWav.outputRate == 96000);
        CHECK(adjWav.outputBits == 24);
    }

    // 15B: Codec x Rate x BitDepth Matrix Verification (FLAC, AAC, MP3, ALAC, WAV)
    const dseeUnified::CodecType allCodecs[] = {
        dseeUnified::CodecType::Flac,
        dseeUnified::CodecType::Aac,
        dseeUnified::CodecType::Mp3,
        dseeUnified::CodecType::Alac,
        dseeUnified::CodecType::Wav
    };

    for (auto c : allCodecs) {
        // 44.1kHz / 16bit DSEE ON -> 88.2kHz / 24bit
        {
            dseeUnified::DseeConfig req{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
            dseeUnified::DseeConfig adj{};
            CHECK(dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x, 16));
            CHECK(adj.outputRate == 88200);
            CHECK(adj.outputBits == 24);
            CHECK(adj.inputBits == 16);
        }
        // 44.1kHz / 16bit DSEE OFF -> pass-through
        {
            dseeUnified::DseeConfig req{0, 2, 16, 44100, 44100, 16, 16, 0, 0};
            dseeUnified::DseeConfig adj{};
            CHECK(!dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x, 16));
        }

        // 48kHz / 16bit DSEE ON -> 96kHz / 24bit
        {
            dseeUnified::DseeConfig req{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
            dseeUnified::DseeConfig adj{};
            CHECK(dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x, 16));
            CHECK(adj.outputRate == 96000);
            CHECK(adj.outputBits == 24);
            CHECK(adj.inputBits == 16);
        }
        // 48kHz / 16bit DSEE OFF -> pass-through
        {
            dseeUnified::DseeConfig req{0, 2, 16, 48000, 48000, 16, 16, 0, 0};
            dseeUnified::DseeConfig adj{};
            CHECK(!dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x, 16));
        }

        // 44.1kHz / 24bit DSEE ON -> 88.2kHz / 24bit
        {
            dseeUnified::DseeConfig req{1, 2, 16, 44100, 44100, 24, 24, 0, 0};
            dseeUnified::DseeConfig adj{};
            CHECK(dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x, 24));
            CHECK(adj.outputRate == 88200);
            CHECK(adj.outputBits == 24);
            CHECK(adj.inputBits == 24);
        }
        // 44.1kHz / 24bit DSEE OFF -> pass-through
        {
            dseeUnified::DseeConfig req{0, 2, 16, 44100, 44100, 24, 24, 0, 0};
            dseeUnified::DseeConfig adj{};
            CHECK(!dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x, 24));
        }

        // 48kHz / 24bit DSEE ON -> 96kHz / 24bit
        {
            dseeUnified::DseeConfig req{1, 2, 16, 48000, 48000, 24, 24, 0, 0};
            dseeUnified::DseeConfig adj{};
            CHECK(dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x, 24));
            CHECK(adj.outputRate == 96000);
            CHECK(adj.outputBits == 24);
            CHECK(adj.inputBits == 24);
        }
        // 48kHz / 24bit DSEE OFF -> pass-through
        {
            dseeUnified::DseeConfig req{0, 2, 16, 48000, 48000, 24, 24, 0, 0};
            dseeUnified::DseeConfig adj{};
            CHECK(!dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x, 24));
        }

        // Native Hi-Res 96kHz / 24bit -> Pass-through untouched (preserving bit-perfect Hi-Res)
        {
            dseeUnified::DseeConfig req96{1, 2, 16, 96000, 96000, 24, 24, 0, 0};
            dseeUnified::DseeConfig adj96{};
            CHECK(!dseeUnified::computeUnifiedDseeConfig(req96, adj96, dseeUnified::HarmonizeMode::Family2x, 24));
            CHECK(dseeUnified::isHiResRate(req96.inputRate));
        }

        // Native Hi-Res 192kHz / 24bit -> Pass-through untouched (preserving bit-perfect Hi-Res)
        {
            dseeUnified::DseeConfig req192{1, 2, 16, 192000, 192000, 24, 24, 0, 0};
            dseeUnified::DseeConfig adj192{};
            CHECK(!dseeUnified::computeUnifiedDseeConfig(req192, adj192, dseeUnified::HarmonizeMode::Family2x, 24));
            CHECK(dseeUnified::isHiResRate(req192.inputRate));
        }
    }

    // 15C: DirectShow Mock Graph EQ Bypass with Non-FLAC (AAC/MP3/ALAC) Decoders
    // Verify that applyEqBypass works independently of the decoder CLSID
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        // Simulate AAC / MP4 Decoder Filter
        CLSID mp4DecoderClsid{};
        CLSIDFromString(L"{34B46F28-8547-49F1-8B40-D5E3C989C910}", &mp4DecoderClsid);
        auto* aacDec = new MockFilter(mp4DecoderClsid);
        auto* aacDecOut = new MockPin(PINDIR_OUTPUT, aacDec, 44100, 16);
        aacDec->addPin(aacDecOut);
        graph->addFilter(aacDec);
        aacDec->Release();

        // DSEE Filter (outputting 88.2kHz / 24bit)
        auto* dsee = new MockFilter(dseeUnified::dseeId);
        auto* dseeIn = new MockPin(PINDIR_INPUT, dsee, 44100, 16);
        auto* dseeOut = new MockPin(PINDIR_OUTPUT, dsee, 88200, 24);
        dsee->addPin(dseeIn);
        dsee->addPin(dseeOut);
        graph->addFilter(dsee);
        dsee->Release();

        // Seamless Filter
        auto* seam = new MockFilter(dseeUnified::seamlessId);
        auto* seamIn = new MockPin(PINDIR_INPUT, seam, 88200, 24);
        auto* seamOut = new MockPin(PINDIR_OUTPUT, seam, 88200, 24);
        seam->addPin(seamIn);
        seam->addPin(seamOut);
        graph->addFilter(seam);
        seam->Release();

        // EQ Filter
        auto* eq = new MockFilter(dseeUnified::eqId);
        auto* eqIn = new MockPin(PINDIR_INPUT, eq, 88200, 24);
        auto* eqOut = new MockPin(PINDIR_OUTPUT, eq, 88200, 24);
        eq->addPin(eqIn);
        eq->addPin(eqOut);
        graph->addFilter(eq);
        eq->Release();

        // DirectSound Renderer
        auto* ds = new MockFilter(CLSID_DSoundRender);
        auto* dsIn = new MockPin(PINDIR_INPUT, ds, 88200, 24);
        ds->addPin(dsIn);
        graph->addFilter(ds);
        ds->Release();

        // Connect graph: AAC -> DSEE -> Seamless -> EQ -> DirectSound
        graph->ConnectDirect(aacDecOut, dseeIn, nullptr);
        graph->ConnectDirect(dseeOut, seamIn, nullptr);
        graph->ConnectDirect(seamOut, eqIn, nullptr);
        graph->ConnectDirect(eqOut, dsIn, nullptr);

        // Apply EQ bypass
        auto routeRes = dseeUnified::applyEqBypass(graph.Get());
        CHECK(routeRes.outcome == dseeUnified::RouteOutcome::Bypassed);
        CHECK(routeRes.outputRate == 88200);
        CHECK(routeRes.outputBits == 24);

        // Verify DirectSound is now connected directly to Seamless output
        dseeUnified::GraphInspection insp{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), insp));
        CHECK(insp.isDirectSeamless);
        CHECK(!insp.eqIn.connected); // EQ input disconnected
        CHECK(insp.renderIn.sampleRate == 88200);
        CHECK(insp.renderIn.bitDepth == 24);
        DWORD peerCode = insp.isDirectSeamless ? 1 : (insp.isDirectDsee ? 2 : (insp.eqBypassed ? 3 : 0));
        CHECK(peerCode == 1);
    }

    // 15D: Direct DSEE Route (Peer Code 2) without Seamless for MP3 / CD Decoder
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        CLSID cdDecoderClsid{};
        CLSIDFromString(L"{8B6A3021-97D0-42E3-A9DF-5C286D491C5B}", &cdDecoderClsid);
        auto* mp3Dec = new MockFilter(cdDecoderClsid);
        auto* mp3DecOut = new MockPin(PINDIR_OUTPUT, mp3Dec, 44100, 16);
        mp3Dec->addPin(mp3DecOut);
        graph->addFilter(mp3Dec);
        mp3Dec->Release();

        // DSEE Filter (outputting 88.2kHz / 24bit)
        auto* dsee = new MockFilter(dseeUnified::dseeId);
        auto* dseeIn = new MockPin(PINDIR_INPUT, dsee, 44100, 16);
        auto* dseeOut = new MockPin(PINDIR_OUTPUT, dsee, 88200, 24);
        dsee->addPin(dseeIn);
        dsee->addPin(dseeOut);
        graph->addFilter(dsee);
        dsee->Release();

        // EQ Filter
        auto* eq = new MockFilter(dseeUnified::eqId);
        auto* eqIn = new MockPin(PINDIR_INPUT, eq, 88200, 24);
        auto* eqOut = new MockPin(PINDIR_OUTPUT, eq, 88200, 24);
        eq->addPin(eqIn);
        eq->addPin(eqOut);
        graph->addFilter(eq);
        eq->Release();

        // DirectSound Renderer
        auto* ds = new MockFilter(CLSID_DSoundRender);
        auto* dsIn = new MockPin(PINDIR_INPUT, ds, 88200, 24);
        ds->addPin(dsIn);
        graph->addFilter(ds);
        ds->Release();

        // Connect graph: MP3 -> DSEE -> EQ -> DirectSound
        graph->ConnectDirect(mp3DecOut, dseeIn, nullptr);
        graph->ConnectDirect(dseeOut, eqIn, nullptr);
        graph->ConnectDirect(eqOut, dsIn, nullptr);

        auto routeRes = dseeUnified::applyEqBypass(graph.Get());
        CHECK(routeRes.outcome == dseeUnified::RouteOutcome::Bypassed);

        dseeUnified::GraphInspection insp{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), insp));
        CHECK(insp.isDirectDsee);
        CHECK(!insp.eqIn.connected);
        DWORD peerCode = insp.isDirectSeamless ? 1 : (insp.isDirectDsee ? 2 : (insp.eqBypassed ? 3 : 0));
        CHECK(peerCode == 2);
    }
}

// Test 16: Issue #54 Verification Suite - 44.1kHz/24bit FLAC Entrance, DSEE Input, and Track Transitions
void testIssue54Flac44k24bInputAndTrackTransitions() {
    std::printf("[TEST 16] Issue #54: 44.1kHz/24bit FLAC DSEE 24-bit Input & Transition Isolation...\n");

    // 16A: Rate Eligibility Logic (isFlacRateEligible24)
    CHECK(dseeUnified::isFlacRateEligible24(44100, true));   // 44.1k + DSEE ON -> eligible (24-bit passed)
    CHECK(!dseeUnified::isFlacRateEligible24(44100, false));  // 44.1k + DSEE OFF -> NOT eligible (kept at 16-bit to protect EQ)
    CHECK(dseeUnified::isFlacRateEligible24(48000, true));   // 48k + DSEE ON -> eligible
    CHECK(dseeUnified::isFlacRateEligible24(48000, false));  // 48k + DSEE OFF -> eligible
    CHECK(dseeUnified::isFlacRateEligible24(96000, true));   // 96k + DSEE ON -> eligible
    CHECK(dseeUnified::isFlacRateEligible24(96000, false));  // 96k + DSEE OFF -> eligible
    CHECK(dseeUnified::isFlacRateEligible24(192000, true));  // 192k + DSEE ON -> eligible
    CHECK(dseeUnified::isFlacRateEligible24(192000, false)); // 192k + DSEE OFF -> eligible
    CHECK(!dseeUnified::isFlacRateEligible24(32000, true));  // Unsupported rate -> false

    // 16B: DSEE HX Configuration for 44.1kHz / 24-bit FLAC (DSEE ON)
    {
        dseeUnified::DseeConfig req{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj{};
        // When FLAC decoder indicates 24-bit:
        CHECK(dseeUnified::computeUnifiedDseeConfig(req, adj, dseeUnified::HarmonizeMode::Family2x, 24));
        CHECK(adj.inputRate == 44100);
        CHECK(adj.inputBits == 24);   // Preserved 24-bit input to DSEE HX
        CHECK(adj.outputRate == 88200); // Integer 2x within 44.1k family
        CHECK(adj.outputBits == 24);  // 24-bit output
    }

    // 16C: Multi-Track Transition & Stale Bit Depth Prevention
    // Track 1: 44.1kHz / 24-bit FLAC (DSEE ON)
    dseeUnified::setActiveDecoderBits(24);
    CHECK(dseeUnified::getActiveDecoderBits().load() == 24);
    {
        dseeUnified::DseeConfig req1{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
        WORD decBits1 = dseeUnified::getActiveDecoderBits().load();
        dseeUnified::DseeConfig adj1{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(req1, adj1, dseeUnified::HarmonizeMode::Family2x, decBits1));
        CHECK(adj1.inputBits == 24);
        CHECK(adj1.outputRate == 88200);
        CHECK(adj1.outputBits == 24);
    }

    // Track 2: 48kHz / 16-bit FLAC (DSEE ON)
    // Decoder recognizes 16-bit -> sets activeDecoderBits to 16
    dseeUnified::setActiveDecoderBits(16);
    CHECK(dseeUnified::getActiveDecoderBits().load() == 16);
    {
        dseeUnified::DseeConfig req2{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
        WORD decBits2 = dseeUnified::getActiveDecoderBits().load();
        dseeUnified::DseeConfig adj2{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(req2, adj2, dseeUnified::HarmonizeMode::Family2x, decBits2));
        CHECK(adj2.inputBits == 16);  // Must NOT leak 24-bit from Track 1!
        CHECK(adj2.outputRate == 96000);
        CHECK(adj2.outputBits == 24);
    }

    // Track 3: 44.1kHz / 16-bit FLAC (DSEE ON)
    dseeUnified::setActiveDecoderBits(16);
    CHECK(dseeUnified::getActiveDecoderBits().load() == 16);
    {
        dseeUnified::DseeConfig req3{1, 2, 16, 44100, 44100, 16, 16, 0, 0};
        WORD decBits3 = dseeUnified::getActiveDecoderBits().load();
        dseeUnified::DseeConfig adj3{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(req3, adj3, dseeUnified::HarmonizeMode::Family2x, decBits3));
        CHECK(adj3.inputBits == 16);  // Must remain 16-bit
        CHECK(adj3.outputRate == 88200);
        CHECK(adj3.outputBits == 24);
    }

    // Track 4: 48kHz / 16-bit AAC (OpcMp4.dll)
    // Non-FLAC caller causes reset of activeDecoderBits
    {
        dseeUnified::resetActiveDecoderBits();
        CHECK(dseeUnified::getActiveDecoderBits().load() == 0);
        dseeUnified::DseeConfig req4{1, 2, 16, 48000, 48000, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj4{};
        CHECK(dseeUnified::computeUnifiedDseeConfig(req4, adj4, dseeUnified::HarmonizeMode::Family2x, 0));
        CHECK(adj4.inputBits == 16);
        CHECK(adj4.outputRate == 96000);
        CHECK(adj4.outputBits == 24);
    }

    // Track 5: 44.1kHz / 24-bit FLAC with DSEE OFF
    // DSEE is OFF -> must NOT be eligible for 24-bit (falls back to 16-bit to preserve EQ path)
    {
        CHECK(!dseeUnified::isFlacRateEligible24(44100, false));
        dseeUnified::setActiveDecoderBits(16);
        dseeUnified::DseeConfig req5{0, 2, 16, 44100, 44100, 16, 16, 0, 0};
        dseeUnified::DseeConfig adj5{};
        CHECK(!dseeUnified::computeUnifiedDseeConfig(req5, adj5, dseeUnified::HarmonizeMode::Family2x, 16));
    }

    // 16D: DirectShow Mock Graph 4-Stage Format Inspection for 44.1kHz / 24-bit FLAC (DSEE ON)
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        auto* dec = new MockFilter(dseeUnified::flacId, L"OmgFlacSrc");
        auto* decOut = new MockPin(PINDIR_OUTPUT, dec, 44100, 24, L"Out");
        dec->addPin(decOut);
        graph->addFilter(dec);

        auto* dsee = new MockFilter(dseeUnified::dseeId, L"OmgDseeHxFilter");
        auto* dseeIn = new MockPin(PINDIR_INPUT, dsee, 44100, 24, L"In");
        auto* dseeOut = new MockPin(PINDIR_OUTPUT, dsee, 88200, 24, L"Out");
        dsee->addPin(dseeIn);
        dsee->addPin(dseeOut);
        graph->addFilter(dsee);

        auto* seam = new MockFilter(dseeUnified::seamlessId, L"SeamlessFilter");
        auto* seamIn = new MockPin(PINDIR_INPUT, seam, 44100, 16, L"In");
        auto* seamOut = new MockPin(PINDIR_OUTPUT, seam, 44100, 16, L"Out");
        seam->addPin(seamIn);
        seam->addPin(seamOut);
        graph->addFilter(seam);

        auto* eq = new MockFilter(dseeUnified::eqId, L"OmgEqxlFilter");
        auto* eqIn = new MockPin(PINDIR_INPUT, eq, 44100, 16, L"In");
        auto* eqOut = new MockPin(PINDIR_OUTPUT, eq, 44100, 16, L"Out");
        eq->addPin(eqIn);
        eq->addPin(eqOut);
        graph->addFilter(eq);

        auto* ds = new MockFilter(CLSID_DSoundRender, L"DirectSoundRender");
        auto* dsIn = new MockPin(PINDIR_INPUT, ds, 44100, 16, L"In");
        ds->addPin(dsIn);
        graph->addFilter(ds);

        graph->ConnectDirect(decOut, dseeIn, nullptr);
        graph->ConnectDirect(dseeOut, seamIn, nullptr);
        graph->ConnectDirect(seamOut, eqIn, nullptr);
        graph->ConnectDirect(eqOut, dsIn, nullptr);

        // Apply EQ bypass: because seamOut is 44.1k/16b and dseeOut is 88.2k/24b,
        // Peer Code 2 connects dseeOut directly to dsIn
        auto routeRes = dseeUnified::applyEqBypass(graph.Get());
        CHECK(routeRes.outcome == dseeUnified::RouteOutcome::Bypassed);
        CHECK(routeRes.outputRate == 88200);
        CHECK(routeRes.outputBits == 24);

        dseeUnified::GraphInspection insp{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), insp));

        // Verify the entire 4-stage format chain in the same generation:
        CHECK(insp.decodePcm.sampleRate == 44100 && insp.decodePcm.bitDepth == 24); // Decoder output: 44.1k/24b
        CHECK(insp.dseeIn.sampleRate == 44100 && insp.dseeIn.bitDepth == 24);       // DSEE input: 44.1k/24b (NO truncation!)
        CHECK(insp.dseeOut.sampleRate == 88200 && insp.dseeOut.bitDepth == 24);     // DSEE output: 88.2k/24b
        CHECK(insp.renderIn.sampleRate == 88200 && insp.renderIn.bitDepth == 24);   // Renderer input: 88.2k/24b
        CHECK(insp.isDirectDsee);                                                    // Peer Code 2 (Direct DSEE)
        CHECK(insp.eqBypassed);
    }
}

// Test 17: Issue #56 Verification Suite - DSEE OFF Reset, Stale Format Clearance, and EQ Route Restoration
void testIssue56DseeOffResetAndEqRestoration() {
    std::printf("[TEST 17] Issue #56: DSEE OFF Format Reset & EQ Route Restoration...\n");

    // 17A: DSEE Config Reset Logic (makeOffResetDseeConfig)
    // Scenario: Track previously had DSEE ON (96k/24b). Now UI is OFF or request is mode=0.
    {
        // 48kHz / 16bit FLAC: incoming req has stale 96k/24b output parameters
        dseeUnified::DseeConfig staleReq48{0, 2, 16, 48000, 96000, 16, 24, 0, 0};
        dseeUnified::DseeConfig reset48 = dseeUnified::makeOffResetDseeConfig(staleReq48);
        CHECK(reset48.mode == 0);
        CHECK(reset48.channels == 2);
        CHECK(reset48.inputRate == 48000);
        CHECK(reset48.outputRate == 48000); // Cleared stale 96k!
        CHECK(reset48.inputBits == 16);
        CHECK(reset48.outputBits == 16);   // Cleared stale 24b!
        CHECK(reset48.format == 16);

        // Even if Music Center sends initial mode=1 during song open,
        // when UI is OFF, makeOffResetDseeConfig forces mode=0 and exact input parameters:
        dseeUnified::DseeConfig initialMode1{1, 2, 16, 48000, 96000, 16, 24, 0, 0};
        dseeUnified::DseeConfig resetInitial = dseeUnified::makeOffResetDseeConfig(initialMode1);
        CHECK(resetInitial.mode == 0);
        CHECK(resetInitial.outputRate == 48000);
        CHECK(resetInitial.outputBits == 16);

        // 44.1kHz / 16bit
        dseeUnified::DseeConfig staleReq44{0, 2, 16, 44100, 88200, 16, 24, 0, 0};
        dseeUnified::DseeConfig reset44 = dseeUnified::makeOffResetDseeConfig(staleReq44);
        CHECK(reset44.mode == 0);
        CHECK(reset44.outputRate == 44100);
        CHECK(reset44.outputBits == 16);

        // 44.1kHz / 24bit FLAC (DSEE OFF -> bit-perfect native format matching)
        dseeUnified::DseeConfig req44_24{0, 2, 16, 44100, 88200, 24, 24, 0, 0};
        dseeUnified::DseeConfig reset44_24 = dseeUnified::makeOffResetDseeConfig(req44_24);
        CHECK(reset44_24.mode == 0);
        CHECK(reset44_24.outputRate == 44100);
        CHECK(reset44_24.outputBits == 24);
        CHECK(reset44_24.format == 24);
    }

    // 17B: DirectShow Mock Graph EQ Restoration (ON -> OFF Transition)
    // Setup a graph that was left in the DSEE ON state (EQ bypassed: seamOut directly connected to dsIn at 96k/24b)
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        auto* dec = new MockFilter(dseeUnified::flacId, L"OmgFlacSrc");
        auto* decOut = new MockPin(PINDIR_OUTPUT, dec, 48000, 16, L"Out");
        dec->addPin(decOut);
        graph->addFilter(dec);

        auto* dsee = new MockFilter(dseeUnified::dseeId, L"OmgDseeHxFilter");
        auto* dseeIn = new MockPin(PINDIR_INPUT, dsee, 48000, 16, L"In");
        auto* dseeOut = new MockPin(PINDIR_OUTPUT, dsee, 48000, 16, L"Out"); // Reset to 48k/16b
        dsee->addPin(dseeIn);
        dsee->addPin(dseeOut);
        graph->addFilter(dsee);

        auto* seam = new MockFilter(dseeUnified::seamlessId, L"SeamlessFilter");
        auto* seamIn = new MockPin(PINDIR_INPUT, seam, 48000, 16, L"In");
        auto* seamOut = new MockPin(PINDIR_OUTPUT, seam, 48000, 16, L"Out");
        seam->addPin(seamIn);
        seam->addPin(seamOut);
        graph->addFilter(seam);

        auto* eq = new MockFilter(dseeUnified::eqId, L"OmgEqxlFilter");
        auto* eqIn = new MockPin(PINDIR_INPUT, eq, 48000, 16, L"In");
        auto* eqOut = new MockPin(PINDIR_OUTPUT, eq, 48000, 16, L"Out");
        eq->addPin(eqIn);
        eq->addPin(eqOut);
        graph->addFilter(eq);

        auto* ds = new MockFilter(CLSID_DSoundRender, L"DirectSoundRender");
        auto* dsIn = new MockPin(PINDIR_INPUT, ds, 96000, 24, L"In"); // Left in 96k/24b from previous track!
        ds->addPin(dsIn);
        graph->addFilter(ds);

        // Currently bypassed: dec -> dsee -> seam -> dsIn (eq bypassed/disconnected)
        graph->ConnectDirect(decOut, dseeIn, nullptr);
        graph->ConnectDirect(dseeOut, seamIn, nullptr);
        graph->ConnectDirect(seamOut, dsIn, nullptr);

        // Verify it is currently reported as bypassed:
        dseeUnified::GraphInspection inspBefore{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), inspBefore));
        CHECK(inspBefore.eqBypassed);
        CHECK(inspBefore.isDirectSeamless);

        // Now user turned DSEE OFF: call applyEqBypass with dseeActive = false
        auto routeRes = dseeUnified::applyEqBypass(graph.Get(), false);
        CHECK(routeRes.outcome == dseeUnified::RouteOutcome::Restored);
        CHECK(routeRes.change == S_OK);
        CHECK(routeRes.outputRate == 48000); // Restored to 48kHz!
        CHECK(routeRes.outputBits == 16);   // Restored to 16-bit!

        // Inspect graph after restoration:
        dseeUnified::GraphInspection inspAfter{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), inspAfter));
        CHECK(!inspAfter.eqBypassed);        // EQ is NO LONGER bypassed!
        CHECK(!inspAfter.isDirectSeamless);  // DirectSound is no longer directly on Seamless
        CHECK(!inspAfter.isDirectDsee);
        CHECK(inspAfter.eqIn.connected);     // EQ input is reconnected!
        CHECK(inspAfter.eqOut.connected);    // EQ output is reconnected!
        CHECK(inspAfter.renderIn.sampleRate == 48000); // Renderer received 48kHz/16bit!
        CHECK(inspAfter.renderIn.bitDepth == 16);
    }

    // 17C: DirectShow Clean OFF State (Starting from OFF)
    // Setup a clean standard graph (seamOut -> eqIn -> eqOut -> dsIn)
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        auto* dec = new MockFilter(dseeUnified::flacId, L"OmgFlacSrc");
        auto* decOut = new MockPin(PINDIR_OUTPUT, dec, 48000, 16, L"Out");
        dec->addPin(decOut);
        graph->addFilter(dec);

        auto* dsee = new MockFilter(dseeUnified::dseeId, L"OmgDseeHxFilter");
        auto* dseeIn = new MockPin(PINDIR_INPUT, dsee, 48000, 16, L"In");
        auto* dseeOut = new MockPin(PINDIR_OUTPUT, dsee, 48000, 16, L"Out");
        dsee->addPin(dseeIn);
        dsee->addPin(dseeOut);
        graph->addFilter(dsee);

        auto* seam = new MockFilter(dseeUnified::seamlessId, L"SeamlessFilter");
        auto* seamIn = new MockPin(PINDIR_INPUT, seam, 48000, 16, L"In");
        auto* seamOut = new MockPin(PINDIR_OUTPUT, seam, 48000, 16, L"Out");
        seam->addPin(seamIn);
        seam->addPin(seamOut);
        graph->addFilter(seam);

        auto* eq = new MockFilter(dseeUnified::eqId, L"OmgEqxlFilter");
        auto* eqIn = new MockPin(PINDIR_INPUT, eq, 48000, 16, L"In");
        auto* eqOut = new MockPin(PINDIR_OUTPUT, eq, 48000, 16, L"Out");
        eq->addPin(eqIn);
        eq->addPin(eqOut);
        graph->addFilter(eq);

        auto* ds = new MockFilter(CLSID_DSoundRender, L"DirectSoundRender");
        auto* dsIn = new MockPin(PINDIR_INPUT, ds, 48000, 16, L"In");
        ds->addPin(dsIn);
        graph->addFilter(ds);

        // Standard route: dec -> dsee -> seam -> eq -> ds
        graph->ConnectDirect(decOut, dseeIn, nullptr);
        graph->ConnectDirect(dseeOut, seamIn, nullptr);
        graph->ConnectDirect(seamOut, eqIn, nullptr);
        graph->ConnectDirect(eqOut, dsIn, nullptr);

        // Call applyEqBypass with dseeActive = false:
        auto routeRes = dseeUnified::applyEqBypass(graph.Get(), false);
        CHECK(routeRes.outcome == dseeUnified::RouteOutcome::Skipped);
        CHECK(routeRes.outputRate == 48000);
        CHECK(routeRes.outputBits == 16);

        dseeUnified::GraphInspection insp{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), insp));
        CHECK(!insp.eqBypassed);
        CHECK(insp.renderIn.sampleRate == 48000);
        CHECK(insp.renderIn.bitDepth == 16);

        // 17D: OFF -> ON Transition (User turns DSEE ON on next track)
        // DSEE now outputs 96k/24b, and seamOut is 96k/24b:
        dseeOut->setFormat(96000, 24);
        seamOut->setFormat(96000, 24);
        auto routeResOn = dseeUnified::applyEqBypass(graph.Get(), true);
        CHECK(routeResOn.outcome == dseeUnified::RouteOutcome::Bypassed);
        CHECK(routeResOn.outputRate == 96000);
        CHECK(routeResOn.outputBits == 24);

        dseeUnified::GraphInspection inspOn{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), inspOn));
        CHECK(inspOn.eqBypassed);
        CHECK(inspOn.renderIn.sampleRate == 96000);
        CHECK(inspOn.renderIn.bitDepth == 24);
    }

    // 17E: 96kHz / 192kHz Native Hi-Res Path Non-Interference
    {
        Microsoft::WRL::ComPtr<MockGraph> graph = new MockGraph();

        auto* dec = new MockFilter(dseeUnified::flacId, L"OmgFlacSrc");
        auto* decOut = new MockPin(PINDIR_OUTPUT, dec, 192000, 24, L"Out");
        dec->addPin(decOut);
        graph->addFilter(dec);

        auto* dsee = new MockFilter(dseeUnified::dseeId, L"OmgDseeHxFilter");
        auto* dseeIn = new MockPin(PINDIR_INPUT, dsee, 192000, 24, L"In");
        auto* dseeOut = new MockPin(PINDIR_OUTPUT, dsee, 192000, 24, L"Out");
        dsee->addPin(dseeIn);
        dsee->addPin(dseeOut);
        graph->addFilter(dsee);

        auto* seam = new MockFilter(dseeUnified::seamlessId, L"SeamlessFilter");
        auto* seamIn = new MockPin(PINDIR_INPUT, seam, 192000, 24, L"In");
        auto* seamOut = new MockPin(PINDIR_OUTPUT, seam, 192000, 24, L"Out");
        seam->addPin(seamIn);
        seam->addPin(seamOut);
        graph->addFilter(seam);

        auto* ds = new MockFilter(CLSID_DSoundRender, L"DirectSoundRender");
        auto* dsIn = new MockPin(PINDIR_INPUT, ds, 192000, 24, L"In");
        ds->addPin(dsIn);
        graph->addFilter(ds);

        graph->ConnectDirect(decOut, dseeIn, nullptr);
        graph->ConnectDirect(dseeOut, seamIn, nullptr);
        graph->ConnectDirect(seamOut, dsIn, nullptr);

        // Hi-Res track with DSEE OFF (no EQ in graph):
        auto routeRes = dseeUnified::applyEqBypass(graph.Get(), false);
        CHECK(routeRes.outcome == dseeUnified::RouteOutcome::Skipped);

        dseeUnified::GraphInspection insp{};
        CHECK(dseeUnified::inspectGraphFormats(graph.Get(), insp));
        CHECK(insp.renderIn.sampleRate == 192000);
        CHECK(insp.renderIn.bitDepth == 24);
    }
}

} // namespace

int main() {
    std::printf("Starting dsee_unified_tests...\n");
    std::fflush(stdout);
    CoInitialize(nullptr);

    testConfigDeterminationAndRateSelection();
    std::fflush(stdout);
    testCodecCallerIdentification();
    std::fflush(stdout);
    testPreservationOfHiResAndBypass();
    std::fflush(stdout);
    testHarmonizationNegotiationConstraints();
    std::fflush(stdout);
    testGraphTrackerLifecycle();
    std::fflush(stdout);
    testDirectShowEqBypassMock();
    std::fflush(stdout);
    testLiveSonyFilterIfAvailable();
    std::fflush(stdout);
    testLiveSonyFilterFormatsAcceptance();
    std::fflush(stdout);
    testDecoderBitsSyncAndUnifiedFormats();
    std::fflush(stdout);
    testGraphInspectionCoverage();
    std::fflush(stdout);
    testCrossFamilyLimitation();
    std::fflush(stdout);
    testIssue44NegotiationMatrix();
    std::fflush(stdout);
    testDirectSoundRenderPinInspection();
    std::fflush(stdout);
    testIssue46Dsee88kRendererAndChainTraversal();
    std::fflush(stdout);
    testIssue49CodecEligibilityAndRouteMatrix();
    std::fflush(stdout);
    testIssue54Flac44k24bInputAndTrackTransitions();
    std::fflush(stdout);
    testIssue56DseeOffResetAndEqRestoration();
    std::fflush(stdout);

    CoUninitialize();

    std::printf("\n========================================\n");
    if (g_failures == 0) {
        std::printf("ALL DSEE UNIFIED TESTS PASSED (0 failures)\n");
        return 0;
    } else {
        std::printf("DSEE UNIFIED TESTS FAILED (%d failures)\n", g_failures);
        return 1;
    }
}
