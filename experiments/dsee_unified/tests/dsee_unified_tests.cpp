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
    CHECK(dseeUnified::identifyCodecFromModule(L"OpcWavEx.dll") == dseeUnified::CodecType::Wav);
    CHECK(dseeUnified::identifyCodecFromModule(L"OpcCd3.dll") == dseeUnified::CodecType::Mp3);
    CHECK(dseeUnified::identifyCodecFromModule(L"Unknown.dll") == dseeUnified::CodecType::Unknown);
    CHECK(dseeUnified::identifyCodecFromModule(nullptr) == dseeUnified::CodecType::Unknown);

    CHECK(std::strcmp(dseeUnified::codecName(dseeUnified::CodecType::Flac), "FLAC") == 0);
    CHECK(std::strcmp(dseeUnified::codecName(dseeUnified::CodecType::Aac), "AAC") == 0);
    CHECK(std::strcmp(dseeUnified::codecName(dseeUnified::CodecType::Wav), "WAV") == 0);
    CHECK(std::strcmp(dseeUnified::codecName(dseeUnified::CodecType::Mp3), "MP3") == 0);
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

    MockPin(PIN_DIRECTION d, IBaseFilter* f) : dir(d), filter(f) {
        mt.majortype = MEDIATYPE_Audio;
        mt.subtype = MEDIASUBTYPE_PCM;
        mt.formattype = FORMAT_WaveFormatEx;
        mt.cbFormat = sizeof(WAVEFORMATEX);
        mt.pbFormat = reinterpret_cast<BYTE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        auto* w = reinterpret_cast<WAVEFORMATEX*>(mt.pbFormat);
        w->wFormatTag = WAVE_FORMAT_PCM;
        w->nChannels = 2;
        w->nSamplesPerSec = 88200;
        w->wBitsPerSample = 24;
        w->nBlockAlign = 6;
        w->nAvgBytesPerSec = 88200 * 6;
    }
    ~MockPin() {
        if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
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
        pInfo->achName[0] = L'\0';
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
    std::vector<IPin*> pins;

    MockFilter(const CLSID& id) : clsid(id) {}
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
    HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* pInfo) override { return E_NOTIMPL; }
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
