#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../track_tail_check.h"

using Microsoft::WRL::ComPtr;

// Legacy DirectShow Sample Grabber interfaces (qedit.h).
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85") IGrabCallback : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE*, long) = 0;
};
MIDL_INTERFACE("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F") IGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*, long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(IGrabCallback*, long) = 0;
};

namespace {

void initPcmMediaType(AM_MEDIA_TYPE& mt, WAVEFORMATEX& wfx, DWORD rate, WORD bits, WORD channels) {
    std::memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = rate;
    wfx.wBitsPerSample = bits;
    wfx.nBlockAlign = static_cast<WORD>(channels * (bits / 8));
    wfx.nAvgBytesPerSec = rate * wfx.nBlockAlign;

    std::memset(&mt, 0, sizeof(mt));
    mt.majortype = MEDIATYPE_Audio;
    mt.subtype = MEDIASUBTYPE_PCM;
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = wfx.nBlockAlign;
    mt.formattype = FORMAT_WaveFormatEx;
    mt.cbFormat = sizeof(WAVEFORMATEX);
    mt.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    std::memcpy(mt.pbFormat, &wfx, sizeof(WAVEFORMATEX));
}

void copyMediaType(AM_MEDIA_TYPE& dst, const AM_MEDIA_TYPE& src) {
    dst = src;
    if (src.cbFormat && src.pbFormat) {
        dst.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src.cbFormat));
        std::memcpy(dst.pbFormat, src.pbFormat, src.cbFormat);
    }
    if (dst.pUnk) dst.pUnk->AddRef();
}

void freeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    if (mt.pUnk) mt.pUnk->Release();
    mt = {};
}

class SingleMediaTypeEnum : public IEnumMediaTypes {
    std::atomic<ULONG> refs_{1};
    AM_MEDIA_TYPE mt_{};
    ULONG pos_ = 0;
public:
    explicit SingleMediaTypeEnum(const AM_MEDIA_TYPE& mt, ULONG pos = 0) : pos_(pos) { copyMediaType(mt_, mt); }
    ~SingleMediaTypeEnum() { freeMediaType(mt_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** v) override {
        if (!v) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IEnumMediaTypes) {
            *v = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *v = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Next(ULONG c, AM_MEDIA_TYPE** out, ULONG* fetched) override {
        if (!out) return E_POINTER;
        if (fetched) *fetched = 0;
        if (c == 0) return S_OK;
        if (pos_ > 0) return S_FALSE;
        *out = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        if (!*out) return E_OUTOFMEMORY;
        copyMediaType(**out, mt_);
        pos_ = 1;
        if (fetched) *fetched = 1;
        return c == 1 ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG c) override { pos_ += c; return pos_ <= 1 ? S_OK : S_FALSE; }
    HRESULT STDMETHODCALLTYPE Reset() override { pos_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumMediaTypes** out) override {
        if (!out) return E_POINTER;
        *out = new SingleMediaTypeEnum(mt_, pos_);
        return S_OK;
    }
};

class SinglePinEnum : public IEnumPins {
    std::atomic<ULONG> refs_{1};
    ComPtr<IPin> pin_;
    ULONG pos_ = 0;
public:
    explicit SinglePinEnum(IPin* p, ULONG pos = 0) : pin_(p), pos_(pos) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** v) override {
        if (!v) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IEnumPins) {
            *v = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *v = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Next(ULONG c, IPin** out, ULONG* fetched) override {
        if (!out) return E_POINTER;
        if (fetched) *fetched = 0;
        if (c == 0) return S_OK;
        if (pos_ > 0 || !pin_) return S_FALSE;
        *out = pin_.Get();
        (*out)->AddRef();
        pos_ = 1;
        if (fetched) *fetched = 1;
        return c == 1 ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG c) override { pos_ += c; return pos_ <= 1 ? S_OK : S_FALSE; }
    HRESULT STDMETHODCALLTYPE Reset() override { pos_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumPins** out) override {
        if (!out) return E_POINTER;
        *out = new SinglePinEnum(pin_.Get(), pos_);
        return S_OK;
    }
};

class SimpleMediaSample : public IMediaSample {
    std::atomic<ULONG> refs_{1};
    std::vector<BYTE> buffer_;
    long actual_ = 0;
    REFERENCE_TIME start_ = 0, stop_ = 0;
    bool hasTime_ = false;
    BOOL sync_ = TRUE, preroll_ = FALSE, discont_ = FALSE;
public:
    explicit SimpleMediaSample(size_t size) : buffer_(size) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** v) override {
        if (!v) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IMediaSample) {
            *v = static_cast<IMediaSample*>(this);
            AddRef();
            return S_OK;
        }
        *v = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetPointer(BYTE** ppBuffer) override {
        if (!ppBuffer) return E_POINTER;
        *ppBuffer = buffer_.data();
        return S_OK;
    }
    long STDMETHODCALLTYPE GetSize() override { return static_cast<long>(buffer_.size()); }
    HRESULT STDMETHODCALLTYPE GetTime(REFERENCE_TIME* pStart, REFERENCE_TIME* pEnd) override {
        if (!pStart || !pEnd) return E_POINTER;
        if (!hasTime_) return VFW_E_SAMPLE_TIME_NOT_SET;
        *pStart = start_;
        *pEnd = stop_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTime(REFERENCE_TIME* pStart, REFERENCE_TIME* pEnd) override {
        if (!pStart) { hasTime_ = false; return S_OK; }
        start_ = *pStart;
        stop_ = pEnd ? *pEnd : start_;
        hasTime_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsSyncPoint() override { return sync_ ? S_OK : S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetSyncPoint(BOOL b) override { sync_ = b; return S_OK; }
    HRESULT STDMETHODCALLTYPE IsPreroll() override { return preroll_ ? S_OK : S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetPreroll(BOOL b) override { preroll_ = b; return S_OK; }
    long STDMETHODCALLTYPE GetActualDataLength() override { return actual_; }
    HRESULT STDMETHODCALLTYPE SetActualDataLength(long len) override {
        if (len < 0 || static_cast<size_t>(len) > buffer_.size()) return VFW_E_BUFFER_OVERFLOW;
        actual_ = len;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMediaType(AM_MEDIA_TYPE** ppMediaType) override {
        if (!ppMediaType) return E_POINTER;
        *ppMediaType = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE SetMediaType(AM_MEDIA_TYPE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE IsDiscontinuity() override { return discont_ ? S_OK : S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetDiscontinuity(BOOL b) override { discont_ = b; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMediaTime(LONGLONG*, LONGLONG*) override { return VFW_E_MEDIA_TIME_NOT_SET; }
    HRESULT STDMETHODCALLTYPE SetMediaTime(LONGLONG*, LONGLONG*) override { return S_OK; }
};

class SimpleAllocator : public IMemAllocator {
    std::atomic<ULONG> refs_{1};
    ALLOCATOR_PROPERTIES props_{8, 262144, 1, 0};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** v) override {
        if (!v) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IMemAllocator) {
            *v = static_cast<IMemAllocator*>(this);
            AddRef();
            return S_OK;
        }
        *v = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE SetProperties(ALLOCATOR_PROPERTIES* pReq, ALLOCATOR_PROPERTIES* pActual) override {
        if (!pReq || !pActual) return E_POINTER;
        props_.cBuffers = pReq->cBuffers > 0 ? pReq->cBuffers : 8;
        props_.cbBuffer = pReq->cbBuffer > 0 ? pReq->cbBuffer : 262144;
        props_.cbAlign = pReq->cbAlign > 0 ? pReq->cbAlign : 1;
        props_.cbPrefix = pReq->cbPrefix >= 0 ? pReq->cbPrefix : 0;
        *pActual = props_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetProperties(ALLOCATOR_PROPERTIES* pProps) override {
        if (!pProps) return E_POINTER;
        *pProps = props_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Decommit() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBuffer(IMediaSample** ppBuffer, REFERENCE_TIME*, REFERENCE_TIME*, DWORD) override {
        if (!ppBuffer) return E_POINTER;
        *ppBuffer = new SimpleMediaSample(static_cast<size_t>(props_.cbBuffer));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(IMediaSample*) override { return S_OK; }
};

class GrabRecorder : public IGrabCallback {
    std::atomic<ULONG> refs_{1};
public:
    WORD blockAlign = 4;
    std::vector<uint8_t> captured;
    std::vector<size_t> chunkFrames;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** v) override {
        if (!v) return E_POINTER;
        if (id == IID_IUnknown || id == __uuidof(IGrabCallback)) {
            *v = static_cast<IGrabCallback*>(this);
            AddRef();
            return S_OK;
        }
        *v = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE*, long) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample* sample) override {
        BYTE* ptr = nullptr;
        long len = sample->GetActualDataLength();
        if (len > 0 && SUCCEEDED(sample->GetPointer(&ptr)) && ptr) {
            captured.insert(captured.end(), ptr, ptr + len);
            if (blockAlign > 0) chunkFrames.push_back(static_cast<size_t>(len) / blockAlign);
        }
        return S_OK;
    }
};

class CustomSourcePin : public IPin {
    std::atomic<ULONG> refs_{1};
public:
    IBaseFilter* filter_ = nullptr;
    ComPtr<IPin> connected_;
    ComPtr<IMemInputPin> memInput_;
    ComPtr<IMemAllocator> allocator_;
    AM_MEDIA_TYPE mt_{};
    WAVEFORMATEX wfx_{};

    CustomSourcePin(IBaseFilter* parent, DWORD rate, WORD bits, WORD channels) : filter_(parent) {
        initPcmMediaType(mt_, wfx_, rate, bits, channels);
    }
    ~CustomSourcePin() { freeMediaType(mt_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** v) override {
        if (!v) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IPin) {
            *v = static_cast<IPin*>(this);
            AddRef();
            return S_OK;
        }
        *v = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }

    HRESULT STDMETHODCALLTYPE Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override {
        if (!pReceivePin) return E_POINTER;
        if (connected_) return VFW_E_ALREADY_CONNECTED;
        const AM_MEDIA_TYPE* useMt = pmt ? pmt : &mt_;
        HRESULT hr = pReceivePin->ReceiveConnection(this, useMt);
        if (FAILED(hr)) return hr;
        connected_ = pReceivePin;
        pReceivePin->QueryInterface(IID_PPV_ARGS(&memInput_));
        if (memInput_) {
            if (FAILED(memInput_->GetAllocator(&allocator_)) || !allocator_) {
                allocator_.Attach(new SimpleAllocator());
            }
            ALLOCATOR_PROPERTIES req{8, 262144, 1, 0}, act{};
            memInput_->GetAllocatorRequirements(&req);
            if (req.cBuffers < 8) req.cBuffers = 8;
            if (req.cbBuffer < 262144) req.cbBuffer = 262144;
            if (req.cbAlign < 1) req.cbAlign = 1;
            allocator_->SetProperties(&req, &act);
            memInput_->NotifyAllocator(allocator_.Get(), FALSE);
            allocator_->Commit();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE Disconnect() override {
        if (allocator_) allocator_->Decommit();
        allocator_.Reset();
        memInput_.Reset();
        connected_.Reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pPin) override {
        if (!pPin) return E_POINTER;
        if (!connected_) { *pPin = nullptr; return VFW_E_NOT_CONNECTED; }
        *pPin = connected_.Get();
        (*pPin)->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE* pmt) override {
        if (!pmt) return E_POINTER;
        if (!connected_) return VFW_E_NOT_CONNECTED;
        copyMediaType(*pmt, mt_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* pInfo) override {
        if (!pInfo) return E_POINTER;
        pInfo->pFilter = filter_;
        if (filter_) filter_->AddRef();
        pInfo->dir = PINDIR_OUTPUT;
        wcscpy_s(pInfo->achName, L"Out");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* pPinDir) override {
        if (!pPinDir) return E_POINTER;
        *pPinDir = PINDIR_OUTPUT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* Id) override {
        if (!Id) return E_POINTER;
        *Id = static_cast<LPWSTR>(CoTaskMemAlloc(8));
        wcscpy_s(*Id, 4, L"Out");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE* pmt) override {
        return (pmt && pmt->majortype == MEDIATYPE_Audio && pmt->subtype == MEDIASUBTYPE_PCM) ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new SingleMediaTypeEnum(mt_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndOfStream() override { return E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE BeginFlush() override { return E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE EndFlush() override { return E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    HRESULT pushPcm(const uint8_t* data, size_t totalFrames, size_t chunkFrames) {
        if (!connected_ || !memInput_ || !allocator_) return E_FAIL;
        WORD align = wfx_.nBlockAlign;
        REFERENCE_TIME totalRt = static_cast<REFERENCE_TIME>((totalFrames * 10000000ULL) / wfx_.nSamplesPerSec);
        connected_->NewSegment(0, totalRt, 1.0);
        size_t offset = 0;
        while (offset < totalFrames) {
            size_t count = (totalFrames - offset < chunkFrames) ? (totalFrames - offset) : chunkFrames;
            long byteLen = static_cast<long>(count * align);
            ComPtr<IMediaSample> sample;
            HRESULT hr = allocator_->GetBuffer(&sample, nullptr, nullptr, 0);
            if (FAILED(hr) || !sample) return FAILED(hr) ? hr : E_FAIL;
            BYTE* dst = nullptr;
            sample->GetPointer(&dst);
            std::memcpy(dst, data + offset * align, byteLen);
            sample->SetActualDataLength(byteLen);
            REFERENCE_TIME tStart = static_cast<REFERENCE_TIME>((offset * 10000000ULL) / wfx_.nSamplesPerSec);
            REFERENCE_TIME tEnd = static_cast<REFERENCE_TIME>(((offset + count) * 10000000ULL) / wfx_.nSamplesPerSec);
            sample->SetTime(&tStart, &tEnd);
            sample->SetSyncPoint(TRUE);
            sample->SetDiscontinuity(offset == 0 ? TRUE : FALSE);
            hr = memInput_->Receive(sample.Get());
            if (FAILED(hr)) return hr;
            offset += count;
        }
        return S_OK;
    }
};

class CustomSourceFilter : public IBaseFilter {
    std::atomic<ULONG> refs_{1};
    FILTER_STATE state_ = State_Stopped;
    IFilterGraph* graph_ = nullptr;
    ComPtr<IReferenceClock> clock_;
public:
    CustomSourcePin* pin_ = nullptr;
    CustomSourceFilter(DWORD rate, WORD bits, WORD channels) {
        pin_ = new CustomSourcePin(this, rate, bits, channels);
    }
    ~CustomSourceFilter() { pin_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** v) override {
        if (!v) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IPersist || id == IID_IMediaFilter || id == IID_IBaseFilter) {
            *v = static_cast<IBaseFilter*>(this);
            AddRef();
            return S_OK;
        }
        *v = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetClassID(CLSID* pClsID) override {
        if (!pClsID) return E_POINTER;
        CLSIDFromString(L"{E436EBB5-524F-11CE-9F53-0020AF0BA770}", pClsID);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override { state_ = State_Stopped; return S_OK; }
    HRESULT STDMETHODCALLTYPE Pause() override { state_ = State_Paused; return S_OK; }
    HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME) override { state_ = State_Running; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetState(DWORD, FILTER_STATE* State) override {
        if (!State) return E_POINTER;
        *State = state_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock* pClock) override { clock_ = pClock; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock** pClock) override {
        if (!pClock) return E_POINTER;
        *pClock = clock_.Get();
        if (*pClock) (*pClock)->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new SinglePinEnum(pin_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR Id, IPin** ppPin) override {
        if (!ppPin) return E_POINTER;
        if (Id && wcscmp(Id, L"Out") == 0) {
            *ppPin = pin_;
            pin_->AddRef();
            return S_OK;
        }
        *ppPin = nullptr;
        return VFW_E_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* pInfo) override {
        if (!pInfo) return E_POINTER;
        wcscpy_s(pInfo->achName, L"SyntheticPcmSource");
        pInfo->pGraph = graph_;
        if (graph_) graph_->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR) override {
        graph_ = pGraph;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR*) override { return E_NOTIMPL; }
};

struct DseeConfig {
    DWORD mode, channels, format, inputRate, outputRate, inputBits, outputBits, unknown, quality;
};
static_assert(sizeof(DseeConfig) == 36);

// Configure a standalone DSEE HX filter instance owned by this test process.
// Guard the private x86 ABI against the verified OmgDseeHxFilter.ax build.
HRESULT configureStandaloneDsee(IBaseFilter* filter, DWORD mode, DWORD rate, WORD bits) {
#ifndef _M_IX86
    (void)filter; (void)mode; (void)rate; (void)bits;
    return E_NOTIMPL;
#else
    GUID iid;
    CLSIDFromString(L"{6AF76DBE-8634-4FD3-93ED-982EDE1BFDD5}", &iid);
    ComPtr<IUnknown> ctrl;
    HRESULT hr = filter->QueryInterface(iid, reinterpret_cast<void**>(ctrl.GetAddressOf()));
    if (FAILED(hr)) return hr;
    auto fn = (*reinterpret_cast<void***>(ctrl.Get()))[3];
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(fn),
            &module)) {
        return E_FAIL;
    }
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x63eb12c1 || reinterpret_cast<BYTE*>(fn) - base != 0x36ca) {
        return E_NOINTERFACE;
    }
    using SetFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, DseeConfig);
    DseeConfig cfg{mode, 2, 16, rate, rate, bits, bits, 0, 0};
    return reinterpret_cast<SetFn>(fn)(ctrl.Get(), cfg);
#endif
}

HRESULT loadSonyFilter(const wchar_t* dllName, const wchar_t* clsidStr, IBaseFilter** outFilter) {
#ifndef _M_IX86
    (void)dllName; (void)clsidStr; (void)outFilter;
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
#else
    const wchar_t* earthDir = L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth";
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\%s", earthDir, dllName);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    SetDllDirectoryW(earthDir);
    HMODULE mod = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) return HRESULT_FROM_WIN32(GetLastError());
    auto getClass = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
        GetProcAddress(mod, "DllGetClassObject"));
    if (!getClass) return E_NOINTERFACE;
    CLSID clsid;
    CLSIDFromString(clsidStr, &clsid);
    ComPtr<IClassFactory> factory;
    HRESULT hr = getClass(clsid, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    return factory->CreateInstance(nullptr, IID_PPV_ARGS(outFilter));
#endif
}

ComPtr<IPin> findPin(IBaseFilter* filter, PIN_DIRECTION dir) {
    if (!filter) return nullptr;
    ComPtr<IEnumPins> enumPins;
    if (FAILED(filter->EnumPins(&enumPins))) return nullptr;
    ComPtr<IPin> pin;
    while (enumPins->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION d;
        if (SUCCEEDED(pin->QueryDirection(&d)) && d == dir) return pin;
        pin.Reset();
    }
    return nullptr;
}

bool drainEcComplete(IMediaEvent* ev) {
    if (!ev) return false;
    long code = 0;
    LONG_PTR p1 = 0, p2 = 0;
    while (ev->GetEvent(&code, &p1, &p2, 0) == S_OK) {
        ev->FreeEventParams(code, p1, p2);
        if (code == EC_COMPLETE) return true;
    }
    return false;
}

void printAnalysis(const char* label, const trackTail::Analysis& a) {
    std::printf(
        "ANALYSIS label=%s rate=%u bits=%u total_frames=%zu expected_track1=%zu "
        "track1_active=%zu track1_last=%zu missing_tail_frames=%zu missing_tail_ms=%.2f "
        "post_eos_silence=%zu next_track=%d next_start=%zu next_frames=%zu "
        "tail_complete=%d approx_300ms_missing=%d\n",
        label,
        a.rate,
        a.bits,
        a.totalFrames,
        a.expectedTrack1Frames,
        a.track1ActiveFrames,
        a.track1LastFrame,
        a.missingTailFrames,
        a.missingTailMs,
        a.postEosSilenceFrames,
        a.nextTrackDetected ? 1 : 0,
        a.nextTrackStartFrame,
        a.nextTrackFrames,
        a.tailComplete ? 1 : 0,
        a.approx300msMissing ? 1 : 0);
}

// Reassemble a stream across arbitrary block/byte boundaries to verify chunked delivery.
std::vector<uint8_t> deliverInBlocks(const std::vector<uint8_t>& src, size_t blockBytes) {
    std::vector<uint8_t> dst;
    dst.reserve(src.size());
    size_t offset = 0;
    while (offset < src.size()) {
        size_t n = (src.size() - offset < blockBytes) ? (src.size() - offset) : blockBytes;
        dst.insert(dst.end(), src.data() + offset, src.data() + offset + n);
        offset += n;
    }
    return dst;
}

int runAnalyzerTests() {
    const uint16_t bitDepths[] = {16, 24};
    const size_t trackLengths[] = {
        48000, // 1.000 s aligned
        64800, // 1.350 s aligned
        51733  // 1.07777 s prime/unaligned frame count
    };
    const size_t blockFrameSizes[] = {
        480,   // 10 ms standard block
        1024,  // power-of-two block
        1111,  // prime/unaligned block boundary
        24000  // 500 ms parser block
    };

    size_t passedCases = 0;

    for (uint16_t bits : bitDepths) {
        const uint16_t align = trackTail::blockAlign(bits);
        for (size_t length : trackLengths) {
            for (size_t blockFrames : blockFrameSizes) {
                // 1. Full tail present across different lengths and block boundaries.
                {
                    std::vector<uint8_t> pcm;
                    trackTail::appendSynthesizedTrack(pcm, trackTail::kDefaultRate, bits, length, false);
                    auto delivered = deliverInBlocks(pcm, blockFrames * align + 3); // unaligned byte chunks too
                    auto a = trackTail::analyzeStream(delivered.data(), delivered.size(), trackTail::kDefaultRate, bits, length);
                    if (!a.tailComplete || a.missingTailFrames != 0 || a.postEosSilenceFrames != 0 || a.nextTrackDetected) {
                        std::fprintf(stderr, "FAIL: full tail case bits=%u len=%zu block=%zu\n", bits, length, blockFrames);
                        return 1;
                    }
                    ++passedCases;
                }

                // 2. ~300 ms missing from tail (14,400 frames = 300.0 ms, and 15,232 frames = 317.3 ms).
                for (size_t missing : {trackTail::kFrames300MsAt48k, static_cast<size_t>(15232)}) {
                    std::vector<uint8_t> pcm;
                    trackTail::appendSynthesizedTrack(pcm, trackTail::kDefaultRate, bits, length, false);
                    pcm.resize((length - missing) * align);
                    auto delivered = deliverInBlocks(pcm, blockFrames * align);
                    auto a = trackTail::analyzeStream(delivered.data(), delivered.size(), trackTail::kDefaultRate, bits, length);
                    if (a.tailComplete || !a.approx300msMissing || a.missingTailFrames != missing) {
                        std::fprintf(stderr, "FAIL: 300ms missing case bits=%u len=%zu missing=%zu\n", bits, length, missing);
                        return 2;
                    }
                    ++passedCases;
                }

                // 3. ~300 ms missing from tail replaced by silence padding (must still fail tailComplete).
                {
                    std::vector<uint8_t> pcm;
                    trackTail::appendSynthesizedTrack(pcm, trackTail::kDefaultRate, bits, length, false);
                    pcm.resize((length - trackTail::kFrames300MsAt48k) * align);
                    trackTail::appendSilence(pcm, bits, trackTail::kFrames300MsAt48k);
                    auto delivered = deliverInBlocks(pcm, blockFrames * align);
                    auto a = trackTail::analyzeStream(delivered.data(), delivered.size(), trackTail::kDefaultRate, bits, length);
                    if (a.tailComplete || !a.approx300msMissing ||
                        a.missingTailFrames != trackTail::kFrames300MsAt48k ||
                        a.postEosSilenceFrames != trackTail::kFrames300MsAt48k) {
                        std::fprintf(stderr, "FAIL: 300ms silence-padded truncation bits=%u len=%zu\n", bits, length);
                        return 3;
                    }
                    ++passedCases;
                }

                // 4. Full tail + post-EOS silence padding (e.g. EQ 1152 or 5611 frames).
                for (size_t padFrames : {static_cast<size_t>(1152), static_cast<size_t>(5611)}) {
                    std::vector<uint8_t> pcm;
                    trackTail::appendSynthesizedTrack(pcm, trackTail::kDefaultRate, bits, length, false);
                    trackTail::appendSilence(pcm, bits, padFrames);
                    auto delivered = deliverInBlocks(pcm, blockFrames * align);
                    auto a = trackTail::analyzeStream(delivered.data(), delivered.size(), trackTail::kDefaultRate, bits, length);
                    if (!a.tailComplete || a.missingTailFrames != 0 || a.postEosSilenceFrames != padFrames || a.nextTrackDetected) {
                        std::fprintf(stderr, "FAIL: post-EOS silence case bits=%u len=%zu pad=%zu\n", bits, length, padFrames);
                        return 4;
                    }
                    ++passedCases;
                }

                // 5. Full tail + optional post-EOS silence padding + Next track start.
                for (size_t padFrames : {static_cast<size_t>(0), static_cast<size_t>(1152)}) {
                    const size_t nextFrames = 9600; // 200 ms of next track
                    std::vector<uint8_t> pcm;
                    trackTail::appendSynthesizedTrack(pcm, trackTail::kDefaultRate, bits, length, false);
                    if (padFrames > 0) trackTail::appendSilence(pcm, bits, padFrames);
                    trackTail::appendSynthesizedTrack(pcm, trackTail::kDefaultRate, bits, nextFrames, true);
                    auto delivered = deliverInBlocks(pcm, blockFrames * align);
                    auto a = trackTail::analyzeStream(delivered.data(), delivered.size(), trackTail::kDefaultRate, bits, length);
                    if (!a.tailComplete || a.missingTailFrames != 0 ||
                        a.postEosSilenceFrames != padFrames ||
                        !a.nextTrackDetected ||
                        a.nextTrackStartFrame != length + padFrames ||
                        a.nextTrackFrames != nextFrames) {
                        std::fprintf(stderr, "FAIL: next-track transition case bits=%u len=%zu pad=%zu\n", bits, length, padFrames);
                        return 5;
                    }
                    ++passedCases;
                }

                // 6. ~300 ms missing from Track 1 + immediate premature Next track start (must fail tailComplete).
                {
                    const size_t nextFrames = 9600;
                    std::vector<uint8_t> pcm;
                    trackTail::appendSynthesizedTrack(pcm, trackTail::kDefaultRate, bits, length, false);
                    pcm.resize((length - trackTail::kFrames300MsAt48k) * align);
                    trackTail::appendSynthesizedTrack(pcm, trackTail::kDefaultRate, bits, nextFrames, true);
                    auto delivered = deliverInBlocks(pcm, blockFrames * align);
                    auto a = trackTail::analyzeStream(delivered.data(), delivered.size(), trackTail::kDefaultRate, bits, length);
                    if (a.tailComplete || !a.approx300msMissing ||
                        a.missingTailFrames != trackTail::kFrames300MsAt48k ||
                        !a.nextTrackDetected ||
                        a.nextTrackStartFrame != (length - trackTail::kFrames300MsAt48k) ||
                        a.nextTrackFrames != nextFrames) {
                        std::fprintf(stderr, "FAIL: premature next-track transition bits=%u len=%zu\n", bits, length);
                        return 6;
                    }
                    ++passedCases;
                }
            }
        }
    }

    std::printf("ANALYZER_TESTS_PASSED cases=%zu\n", passedCases);
    return 0;
}

struct GraphRunResult {
    bool available = false;
    HRESULT connectHr = E_FAIL;
    size_t inputFrames = 0;
    size_t chunkFrames = 0;
    trackTail::Analysis preEos{};
    bool ecCompletePreEos = false;
    trackTail::Analysis postEos{};
    bool ecCompletePostEos = false;
};

GraphRunResult runSingleSonyGraph(
    const wchar_t* dllName,
    const wchar_t* clsidStr,
    bool isDsee,
    DWORD dseeMode,
    uint16_t bits,
    size_t totalFrames,
    size_t chunkFrames,
    bool sendEos) {
    GraphRunResult out{};
    out.inputFrames = totalFrames;
    out.chunkFrames = chunkFrames;

    ComPtr<IBaseFilter> flt;
    HRESULT hrLoad = loadSonyFilter(dllName, clsidStr, &flt);
    if (FAILED(hrLoad) || !flt) {
        out.connectHr = hrLoad;
        return out;
    }
    out.available = true;

    if (isDsee) {
        HRESULT hrCfg = configureStandaloneDsee(flt.Get(), dseeMode, trackTail::kDefaultRate, bits);
        if (FAILED(hrCfg)) {
            out.connectHr = hrCfg;
            return out;
        }
    }

    ComPtr<IGraphBuilder> graph;
    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
    if (FAILED(hr)) {
        out.connectHr = hr;
        return out;
    }

    CLSID grabId, nullId;
    CLSIDFromString(L"{C1F400A0-3F08-11D3-9F0B-006008039E37}", &grabId);
    CLSIDFromString(L"{C1F400A4-3F08-11D3-9F0B-006008039E37}", &nullId);
    ComPtr<IBaseFilter> grabFilter, nullRenderer;
    hr = CoCreateInstance(grabId, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&grabFilter));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(nullId, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&nullRenderer));
    if (FAILED(hr)) {
        out.connectHr = hr;
        return out;
    }

    auto* srcFilter = new CustomSourceFilter(trackTail::kDefaultRate, bits, trackTail::kStereoChannels);
    graph->AddFilter(srcFilter, L"Src");
    graph->AddFilter(flt.Get(), L"SonyFilter");
    graph->AddFilter(grabFilter.Get(), L"Grab");
    graph->AddFilter(nullRenderer.Get(), L"Null");

    auto fltIn = findPin(flt.Get(), PINDIR_INPUT);
    auto fltOut = findPin(flt.Get(), PINDIR_OUTPUT);
    hr = graph->ConnectDirect(srcFilter->pin_, fltIn.Get(), &srcFilter->pin_->mt_);
    if (SUCCEEDED(hr)) hr = graph->ConnectDirect(fltOut.Get(), findPin(grabFilter.Get(), PINDIR_INPUT).Get(), nullptr);
    if (SUCCEEDED(hr)) hr = graph->ConnectDirect(findPin(grabFilter.Get(), PINDIR_OUTPUT).Get(), findPin(nullRenderer.Get(), PINDIR_INPUT).Get(), nullptr);
    out.connectHr = hr;
    if (FAILED(hr)) {
        srcFilter->Release();
        return out;
    }

    const uint16_t align = trackTail::blockAlign(bits);
    auto* rec = new GrabRecorder();
    rec->blockAlign = align;
    ComPtr<IGrabber> grab;
    grabFilter.As(&grab);
    if (grab) grab->SetCallback(rec, 0);

    std::vector<uint8_t> pcm;
    trackTail::appendSynthesizedTrack(pcm, trackTail::kDefaultRate, bits, totalFrames, false);

    ComPtr<IMediaControl> ctrl;
    ComPtr<IMediaEvent> ev;
    graph.As(&ctrl);
    graph.As(&ev);
    ctrl->Run();

    srcFilter->pin_->pushPcm(pcm.data(), totalFrames, chunkFrames);
    out.preEos = trackTail::analyzeStream(rec->captured.data(), rec->captured.size(), trackTail::kDefaultRate, bits, totalFrames);
    out.ecCompletePreEos = drainEcComplete(ev.Get());

    if (sendEos) {
        fltIn->EndOfStream();
        long evCode = 0;
        HRESULT hrWait = ev->WaitForCompletion(1000, &evCode);
        out.ecCompletePostEos = (hrWait == S_OK && evCode == EC_COMPLETE) || drainEcComplete(ev.Get());
        out.postEos = trackTail::analyzeStream(rec->captured.data(), rec->captured.size(), trackTail::kDefaultRate, bits, totalFrames);
    }

    ctrl->Stop();
    if (grab) grab->SetCallback(nullptr, 0);
    rec->Release();
    srcFilter->Release();
    return out;
}

int runSonyGraphComparison(bool requireSony) {
    struct Case { size_t totalFrames; size_t chunkFrames; };
    const Case cases[] = {
        {48000, 24000}, // 1.0 s aligned, 500 ms blocks
        {48000, 480},   // 1.0 s aligned, 10 ms blocks
        {51733, 1111}   // unaligned length and unaligned block boundary
    };
    const uint16_t bitDepths[] = {16, 24};

    // Probe availability on the first filter.
    ComPtr<IBaseFilter> probeEq, probeDsee;
    HRESULT hrEq = loadSonyFilter(L"OmgEqxlFilter.ax", L"{5C968488-5E35-4CFE-98A1-A51C838797DB}", &probeEq);
    HRESULT hrDsee = loadSonyFilter(L"OmgDseeHxFilter.ax", L"{A481D160-AB4D-4B97-93FE-C1F4B4B781DF}", &probeDsee);
    if (FAILED(hrEq) || FAILED(hrDsee)) {
        std::printf("SONY_GRAPH_SKIPPED eq_hr=%08lx dsee_hr=%08lx\n", ULONG(hrEq), ULONG(hrDsee));
        return requireSony ? 10 : 0;
    }

    size_t compared = 0;
    for (const auto& c : cases) {
        for (uint16_t bits : bitDepths) {
            auto eq = runSingleSonyGraph(
                L"OmgEqxlFilter.ax", L"{5C968488-5E35-4CFE-98A1-A51C838797DB}",
                false, 0, bits, c.totalFrames, c.chunkFrames, true);
            auto dsee0 = runSingleSonyGraph(
                L"OmgDseeHxFilter.ax", L"{A481D160-AB4D-4B97-93FE-C1F4B4B781DF}",
                true, 0, bits, c.totalFrames, c.chunkFrames, true);
            auto dsee1 = runSingleSonyGraph(
                L"OmgDseeHxFilter.ax", L"{A481D160-AB4D-4B97-93FE-C1F4B4B781DF}",
                true, 1, bits, c.totalFrames, c.chunkFrames, true);

            if (FAILED(eq.connectHr) || FAILED(dsee0.connectHr) || FAILED(dsee1.connectHr)) {
                std::fprintf(stderr, "FAIL: Sony graph connect eq=%08lx dsee0=%08lx dsee1=%08lx\n",
                             ULONG(eq.connectHr), ULONG(dsee0.connectHr), ULONG(dsee1.connectHr));
                return 11;
            }

            std::printf(
                "SONY_COMPARE in=%zu chunk=%zu bits=%u | "
                "EQ pre_frames=%zu(missing=%.1fms,ec=%d) post_frames=%zu(active=%zu,pad=%zu,complete=%d,ec=%d) | "
                "DSEE_m0 pre_frames=%zu(missing=%.1fms,ec=%d) post_frames=%zu(active=%zu,pad=%zu,complete=%d,ec=%d) | "
                "DSEE_m1 pre_frames=%zu(missing=%.1fms,ec=%d) post_frames=%zu(active=%zu,pad=%zu,complete=%d,ec=%d)\n",
                c.totalFrames, c.chunkFrames, bits,
                eq.preEos.totalFrames, eq.preEos.missingTailMs, eq.ecCompletePreEos ? 1 : 0,
                eq.postEos.totalFrames, eq.postEos.track1LastFrame, eq.postEos.postEosSilenceFrames,
                eq.postEos.tailComplete ? 1 : 0, eq.ecCompletePostEos ? 1 : 0,
                dsee0.preEos.totalFrames, dsee0.preEos.missingTailMs, dsee0.ecCompletePreEos ? 1 : 0,
                dsee0.postEos.totalFrames, dsee0.postEos.track1LastFrame, dsee0.postEos.postEosSilenceFrames,
                dsee0.postEos.tailComplete ? 1 : 0, dsee0.ecCompletePostEos ? 1 : 0,
                dsee1.preEos.totalFrames, dsee1.preEos.missingTailMs, dsee1.ecCompletePreEos ? 1 : 0,
                dsee1.postEos.totalFrames, dsee1.postEos.track1LastFrame, dsee1.postEos.postEosSilenceFrames,
                dsee1.postEos.tailComplete ? 1 : 0, dsee1.ecCompletePostEos ? 1 : 0);

            // Verify pre-EOS behavior:
            // EQ buffers ~220-320 ms before EOS, so without EOS the tail is incomplete and EC_COMPLETE is absent.
            if (eq.preEos.tailComplete || eq.ecCompletePreEos || !eq.preEos.approx300msMissing) {
                std::fprintf(stderr, "FAIL: EQ pre-EOS expected ~300ms buffered tail\n");
                return 12;
            }
            if (dsee0.preEos.tailComplete || dsee0.ecCompletePreEos ||
                dsee1.preEos.tailComplete || dsee1.ecCompletePreEos) {
                std::fprintf(stderr, "FAIL: DSEE pre-EOS should not complete before EOS\n");
                return 13;
            }

            // Verify post-EOS behavior:
            // Both EQ and DSEE HX flush all active frames up to totalFrames and signal EC_COMPLETE.
            // EQ pads the final block with silence (postEosSilenceFrames > 0, totalFrames > inputFrames),
            // whereas DSEE HX outputs the exact input frame count with zero silence padding.
            if (!eq.postEos.tailComplete || !eq.ecCompletePostEos ||
                eq.postEos.track1LastFrame != c.totalFrames ||
                eq.postEos.postEosSilenceFrames == 0 ||
                eq.postEos.totalFrames != c.totalFrames + eq.postEos.postEosSilenceFrames) {
                std::fprintf(stderr, "FAIL: EQ post-EOS tail or padding mismatch\n");
                return 14;
            }
            for (const auto* dsee : {&dsee0, &dsee1}) {
                if (!dsee->postEos.tailComplete || !dsee->ecCompletePostEos ||
                    dsee->postEos.track1LastFrame != c.totalFrames ||
                    dsee->postEos.postEosSilenceFrames != 0 ||
                    dsee->postEos.totalFrames != c.totalFrames) {
                    std::fprintf(stderr, "FAIL: DSEE post-EOS frame count or tail mismatch\n");
                    return 15;
                }
            }
            ++compared;
        }
    }

    std::printf("SONY_GRAPH_COMPARISONS_PASSED cases=%zu\n", compared);
    return 0;
}

int analyzeExternalPcmFile(const wchar_t* path, uint16_t bits, size_t expectedFrames, bool expectNext) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "FAIL: cannot open PCM file\n");
        return 2;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        CloseHandle(h);
        std::fprintf(stderr, "FAIL: empty or invalid PCM file\n");
        return 2;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    bool ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got == buf.size();
    CloseHandle(h);
    if (!ok) return 2;

    auto a = trackTail::analyzeStream(buf.data(), buf.size(), trackTail::kDefaultRate, bits, expectedFrames);
    printAnalysis("file", a);
    if (!a.tailComplete) {
        std::printf("VERDICT FAIL_TAIL_MISSING\n");
        return 1;
    }
    if (expectNext && !a.nextTrackDetected) {
        std::printf("VERDICT FAIL_NEXT_TRACK_MISSING\n");
        return 1;
    }
    std::printf("VERDICT PASS\n");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int status = 0;
    if (argc <= 1 || wcscmp(argv[1], L"all") == 0) {
        status = runAnalyzerTests();
        if (status == 0) status = runSonyGraphComparison(false);
    } else if (wcscmp(argv[1], L"analyzer") == 0) {
        status = runAnalyzerTests();
    } else if (wcscmp(argv[1], L"sony") == 0) {
        status = runSonyGraphComparison(true);
    } else if (wcscmp(argv[1], L"full") == 0) {
        uint16_t bits = (argc > 2) ? static_cast<uint16_t>(_wtoi(argv[2])) : 24;
        size_t frames = (argc > 3) ? static_cast<size_t>(_wtoi64(argv[3])) : 48000;
        size_t padFrames = (argc > 4) ? static_cast<size_t>(_wtoi64(argv[4])) : 0;
        size_t nextFrames = (argc > 5) ? static_cast<size_t>(_wtoi64(argv[5])) : 0;
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, trackTail::kDefaultRate, bits, frames, false);
        if (padFrames > 0) trackTail::appendSilence(pcm, bits, padFrames);
        if (nextFrames > 0) trackTail::appendSynthesizedTrack(pcm, trackTail::kDefaultRate, bits, nextFrames, true);
        auto a = trackTail::analyzeStream(pcm.data(), pcm.size(), trackTail::kDefaultRate, bits, frames);
        printAnalysis("full", a);
        bool pass = a.tailComplete && (nextFrames == 0 || a.nextTrackDetected);
        std::printf("VERDICT %s\n", pass ? "PASS" : "FAIL_TAIL_MISSING");
        status = pass ? 0 : 1;
    } else if (wcscmp(argv[1], L"drop300ms") == 0) {
        uint16_t bits = (argc > 2) ? static_cast<uint16_t>(_wtoi(argv[2])) : 24;
        size_t frames = (argc > 3) ? static_cast<size_t>(_wtoi64(argv[3])) : 48000;
        if (frames <= trackTail::kFrames300MsAt48k) frames = 48000;
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, trackTail::kDefaultRate, bits, frames, false);
        pcm.resize((frames - trackTail::kFrames300MsAt48k) * trackTail::blockAlign(bits));
        auto a = trackTail::analyzeStream(pcm.data(), pcm.size(), trackTail::kDefaultRate, bits, frames);
        printAnalysis("drop300ms", a);
        std::printf("VERDICT %s\n", a.tailComplete ? "PASS" : "FAIL_TAIL_MISSING");
        status = a.tailComplete ? 0 : 1;
    } else if (wcscmp(argv[1], L"eq-no-eos") == 0) {
        uint16_t bits = (argc > 2) ? static_cast<uint16_t>(_wtoi(argv[2])) : 24;
        size_t frames = (argc > 3) ? static_cast<size_t>(_wtoi64(argv[3])) : 48000;
        auto eq = runSingleSonyGraph(
            L"OmgEqxlFilter.ax", L"{5C968488-5E35-4CFE-98A1-A51C838797DB}",
            false, 0, bits, frames, 480, false);
        if (!eq.available || FAILED(eq.connectHr)) {
            std::fprintf(stderr, "FAIL: OmgEqxlFilter.ax unavailable hr=%08lx\n", ULONG(eq.connectHr));
            status = 2;
        } else {
            printAnalysis("eq_no_eos", eq.preEos);
            std::printf("VERDICT %s\n", eq.preEos.tailComplete ? "PASS" : "FAIL_TAIL_MISSING");
            status = eq.preEos.tailComplete ? 0 : 1;
        }
    } else if (wcscmp(argv[1], L"analyze") == 0 && argc >= 5) {
        uint16_t bits = static_cast<uint16_t>(_wtoi(argv[3]));
        size_t expectedFrames = static_cast<size_t>(_wtoi64(argv[4]));
        bool expectNext = (argc > 5) && (_wtoi(argv[5]) != 0);
        status = analyzeExternalPcmFile(argv[2], bits, expectedFrames, expectNext);
    } else {
        std::fprintf(
            stderr,
            "Usage: track_tail_tests [all|analyzer|sony|full [16|24] [frames] [pad] [next]|"
            "drop300ms [16|24] [frames]|eq-no-eos [16|24] [frames]|"
            "analyze <file.pcm> <16|24> <expected_frames> [expect_next]]\n");
        status = 2;
    }

    if (SUCCEEDED(co)) CoUninitialize();
    return status;
}
