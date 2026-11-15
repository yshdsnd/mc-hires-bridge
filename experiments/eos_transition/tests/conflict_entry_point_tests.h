#pragma once
// Headless verification of automatic track change vs manual operations conflict (Issue #30)
// Using actual COM control entry points (IMediaControl, IMediaSeeking, IPin, IMemInputPin, IDirectSoundBuffer).
// Reuses eosTransition::Tracker and trackTail::analyzeStream (#19 analyzer).

#include <windows.h>
#include <dshow.h>
#include <dsound.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdio>
#include "MinHook.h"
#include "track_tail_check.h"
#include "eos_transition.h"

namespace conflictTests {

// Forward declarations
class MockMediaSample;
class MockUpstreamPin;
class MockDownstreamPin;
class MockMediaControl;
class MockMediaSeekingEx;
class MockDirectSoundBufferEx;
class MockFilterGraphEx;
class ControlEntryHarness;

static ControlEntryHarness* s_activeHarness = nullptr;

enum class CallerContext {
    EngineAutoChange,
    UserManualOp
};

// 1. MockMediaSample
class MockMediaSample final : public IMediaSample {
    std::vector<BYTE> buffer_;
    long actualLen_ = 0;
    REFERENCE_TIME tStart_ = 0;
    REFERENCE_TIME tEnd_ = 0;
    bool hasTime_ = false;
    std::atomic<ULONG> ref_{1};

public:
    MockMediaSample(const void* data, size_t size, bool hasTime = false,
                    REFERENCE_TIME tStart = 0, REFERENCE_TIME tEnd = 0)
        : actualLen_(static_cast<long>(size)), tStart_(tStart), tEnd_(tEnd), hasTime_(hasTime) {
        buffer_.resize(size);
        if (data && size > 0) {
            std::memcpy(buffer_.data(), data, size);
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMediaSample) {
            *ppv = static_cast<IMediaSample*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetPointer(BYTE** ppBuffer) override {
        if (!ppBuffer) return E_POINTER;
        *ppBuffer = buffer_.data();
        return S_OK;
    }
    long STDMETHODCALLTYPE GetSize() override { return static_cast<long>(buffer_.size()); }
    HRESULT STDMETHODCALLTYPE GetTime(REFERENCE_TIME* pTimeStart, REFERENCE_TIME* pTimeEnd) override {
        if (!hasTime_) return VFW_E_SAMPLE_TIME_NOT_SET;
        if (pTimeStart) *pTimeStart = tStart_;
        if (pTimeEnd) *pTimeEnd = tEnd_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTime(REFERENCE_TIME* pTimeStart, REFERENCE_TIME* pTimeEnd) override {
        if (!pTimeStart || !pTimeEnd) { hasTime_ = false; return S_OK; }
        tStart_ = *pTimeStart;
        tEnd_ = *pTimeEnd;
        hasTime_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsSyncPoint() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetSyncPoint(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE IsPreroll() override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetPreroll(BOOL) override { return S_OK; }
    long STDMETHODCALLTYPE GetActualDataLength() override { return actualLen_; }
    HRESULT STDMETHODCALLTYPE SetActualDataLength(long lLen) override { actualLen_ = lLen; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMediaType(AM_MEDIA_TYPE**) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetMediaType(AM_MEDIA_TYPE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE IsDiscontinuity() override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetDiscontinuity(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMediaTime(LONGLONG*, LONGLONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMediaTime(LONGLONG*, LONGLONG*) override { return E_NOTIMPL; }
};

// 2. MockUpstreamPin
class MockUpstreamPin : public IPin, public IMemInputPin {
    std::atomic<ULONG> ref_{1};

public:
    int eosCallCount = 0;
    int beginFlushCallCount = 0;
    int endFlushCallCount = 0;
    int newSegmentCallCount = 0;
    int receiveCallCount = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPin) {
            *ppv = static_cast<IPin*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IMemInputPin) {
            *ppv = static_cast<IMemInputPin*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE Connect(IPin*, const AM_MEDIA_TYPE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Disconnect() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ConnectedTo(IPin**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* pPinDir) override {
        if (!pPinDir) return E_POINTER;
        *pPinDir = PINDIR_INPUT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryId(LPWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }

    __declspec(noinline) HRESULT STDMETHODCALLTYPE EndOfStream() override {
        ++eosCallCount;
        volatile int tag = 0x1111;
        (void)tag;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginFlush() override {
        ++beginFlushCallCount;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndFlush() override {
        ++endFlushCallCount;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override {
        ++newSegmentCallCount;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAllocator(IMemAllocator**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE NotifyAllocator(IMemAllocator*, BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Receive(IMediaSample*) override {
        ++receiveCallCount;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReceiveMultiple(IMediaSample**, long, long*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReceiveCanBlock() override { return S_OK; }
};

// 3. MockDownstreamPin
class MockDownstreamPin : public IPin, public IMemInputPin {
    std::atomic<ULONG> ref_{1};

public:
    int eosCallCount = 0;
    int beginFlushCallCount = 0;
    int endFlushCallCount = 0;
    int newSegmentCallCount = 0;
    int receiveCallCount = 0;
    uint64_t totalBytesReceived = 0;
    std::vector<BYTE> receivedPcm;
    REFERENCE_TIME lastStart = 0;
    REFERENCE_TIME lastEnd = 0;
    bool hasLastTime = false;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPin) {
            *ppv = static_cast<IPin*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IMemInputPin) {
            *ppv = static_cast<IMemInputPin*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE Connect(IPin*, const AM_MEDIA_TYPE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Disconnect() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ConnectedTo(IPin**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* pPinDir) override {
        if (!pPinDir) return E_POINTER;
        *pPinDir = PINDIR_INPUT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryId(LPWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }

    __declspec(noinline) HRESULT STDMETHODCALLTYPE EndOfStream() override {
        ++eosCallCount;
        volatile int tag = 0x2222;
        (void)tag;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginFlush() override {
        ++beginFlushCallCount;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndFlush() override {
        ++endFlushCallCount;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override {
        ++newSegmentCallCount;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAllocator(IMemAllocator**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE NotifyAllocator(IMemAllocator*, BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) override { return E_NOTIMPL; }

    __declspec(noinline) HRESULT STDMETHODCALLTYPE Receive(IMediaSample* pSample) override {
        ++receiveCallCount;
        if (!pSample) return E_POINTER;
        BYTE* ptr = nullptr;
        if (SUCCEEDED(pSample->GetPointer(&ptr)) && ptr) {
            long len = pSample->GetActualDataLength();
            if (len > 0) {
                totalBytesReceived += len;
                size_t oldSize = receivedPcm.size();
                receivedPcm.resize(oldSize + len);
                std::memcpy(receivedPcm.data() + oldSize, ptr, len);
            }
        }
        REFERENCE_TIME s = 0, e = 0;
        if (pSample->GetTime(&s, &e) == S_OK) {
            lastStart = s;
            lastEnd = e;
            hasLastTime = true;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReceiveMultiple(IMediaSample**, long, long*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReceiveCanBlock() override { return S_OK; }
};

// 4. MockMediaControl
class MockMediaControl : public IMediaControl {
    std::atomic<ULONG> ref_{1};

public:
    int pauseCallCount = 0;
    int stopCallCount = 0;
    int runCallCount = 0;
    OAFilterState state = State_Stopped;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == IID_IMediaControl) {
            *ppv = static_cast<IMediaControl*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE Run() override {
        ++runCallCount;
        state = State_Running;
        return S_OK;
    }
    __declspec(noinline) HRESULT STDMETHODCALLTYPE Pause() override {
        ++pauseCallCount;
        state = State_Paused;
        volatile int tag = 0x3333;
        (void)tag;
        return S_OK;
    }
    __declspec(noinline) HRESULT STDMETHODCALLTYPE Stop() override {
        ++stopCallCount;
        state = State_Stopped;
        volatile int tag = 0x4444;
        (void)tag;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetState(LONG, OAFilterState* pState) override {
        if (!pState) return E_POINTER;
        *pState = state;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RenderFile(BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddSourceFilter(BSTR, IDispatch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_FilterCollection(IDispatch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_RegFilterCollection(IDispatch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE StopWhenReady() override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
};

// 5. MockMediaSeeking
class MockMediaSeekingEx : public IMediaSeeking {
    std::atomic<ULONG> ref_{1};

public:
    LONGLONG currentPos = 0;
    LONGLONG stopPos = 0;
    LONGLONG duration = 0;
    int setPositionsCallCount = 0;
    int getStopPositionCallCount = 0;
    int getCurrentPositionCallCount = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMediaSeeking) {
            *ppv = static_cast<IMediaSeeking*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE GetCapabilities(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CheckCapabilities(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(const GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryPreferredFormat(GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTimeFormat(GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsUsingTimeFormat(const GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTimeFormat(const GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDuration(LONGLONG* pDuration) override {
        if (!pDuration) return E_POINTER;
        *pDuration = duration;
        return S_OK;
    }
    __declspec(noinline) HRESULT STDMETHODCALLTYPE GetStopPosition(LONGLONG* pStop) override {
        ++getStopPositionCallCount;
        if (!pStop) return E_POINTER;
        *pStop = stopPos;
        return S_OK;
    }
    __declspec(noinline) HRESULT STDMETHODCALLTYPE GetCurrentPosition(LONGLONG* pCurrent) override {
        ++getCurrentPositionCallCount;
        if (!pCurrent) return E_POINTER;
        *pCurrent = currentPos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ConvertTimeFormat(LONGLONG*, const GUID*, LONGLONG, const GUID*) override { return E_NOTIMPL; }
    __declspec(noinline) HRESULT STDMETHODCALLTYPE SetPositions(LONGLONG* pCurrent, DWORD dwCurrentFlags,
                                                              LONGLONG* pStop, DWORD dwStopFlags) override {
        ++setPositionsCallCount;
        DWORD currBits = dwCurrentFlags & AM_SEEKING_PositioningBitsMask;
        DWORD stopBits = dwStopFlags & AM_SEEKING_PositioningBitsMask;
        if (currBits == AM_SEEKING_AbsolutePositioning && pCurrent) {
            currentPos = *pCurrent;
        }
        if (stopBits == AM_SEEKING_AbsolutePositioning && pStop) {
            stopPos = *pStop;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPositions(LONGLONG* pCurrent, LONGLONG* pStop) override {
        if (pCurrent) *pCurrent = currentPos;
        if (pStop) *pStop = stopPos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAvailable(LONGLONG*, LONGLONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRate(double) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRate(double*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPreroll(LONGLONG*) override { return E_NOTIMPL; }
};

// 6. MockDirectSoundBuffer
class MockDirectSoundBufferEx : public IDirectSoundBuffer {
    std::atomic<ULONG> ref_{1};

public:
    int stopCallCount = 0;
    int playCallCount = 0;
    DWORD playPos = 0;
    DWORD writePos = 0;
    DWORD status = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDirectSoundBuffer) {
            *ppv = static_cast<IDirectSoundBuffer*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDSBCAPS) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentPosition(LPDWORD pdwCurrentPlayCursor, LPDWORD pdwCurrentWriteCursor) override {
        if (pdwCurrentPlayCursor) *pdwCurrentPlayCursor = playPos;
        if (pdwCurrentWriteCursor) *pdwCurrentWriteCursor = writePos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFormat(LPWAVEFORMATEX, DWORD, LPDWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVolume(LPLONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPan(LPLONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrequency(LPDWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStatus(LPDWORD pdwStatus) override {
        if (pdwStatus) *pdwStatus = status;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTSOUND, LPCDSBUFFERDESC) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Lock(DWORD, DWORD, LPVOID*, LPDWORD, LPVOID*, LPDWORD, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Play(DWORD, DWORD, DWORD) override {
        ++playCallCount;
        status = DSBSTATUS_PLAYING;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD dwNewPosition) override {
        playPos = dwNewPosition;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFormat(LPCWAVEFORMATEX) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetVolume(LONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPan(LONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetFrequency(DWORD) override { return E_NOTIMPL; }
    __declspec(noinline) HRESULT STDMETHODCALLTYPE Stop() override {
        ++stopCallCount;
        status = 0;
        volatile int tag = 0x5555;
        (void)tag;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock(LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Restore() override { return E_NOTIMPL; }
};

// 7. MockFilterGraph
class MockFilterGraphEx : public IGraphBuilder {
    std::atomic<ULONG> ref_{1};

public:
    MockMediaControl* control = nullptr;
    MockMediaSeekingEx* seeking = nullptr;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IFilterGraph || riid == IID_IGraphBuilder) {
            *ppv = static_cast<IGraphBuilder*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IMediaControl && control) {
            return control->QueryInterface(riid, ppv);
        }
        if (riid == IID_IMediaSeeking && seeking) {
            return seeking->QueryInterface(riid, ppv);
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE AddFilter(IBaseFilter*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RemoveFilter(IBaseFilter*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnumFilters(IEnumFilters**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindFilterByName(LPCWSTR, IBaseFilter**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ConnectDirect(IPin*, IPin*, const AM_MEDIA_TYPE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Reconnect(IPin*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Disconnect(IPin*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetDefaultSyncSource() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Connect(IPin*, IPin*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Render(IPin*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RenderFile(LPCWSTR, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE AddSourceFilter(LPCWSTR, LPCWSTR, IBaseFilter**) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetLogFile(DWORD_PTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Abort() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ShouldOperationContinue() override { return S_OK; }
};

// Function pointer signatures for MinHook targets
using ControlPauseFn = HRESULT(STDMETHODCALLTYPE*)(IMediaControl*);
using ControlStopFn = HRESULT(STDMETHODCALLTYPE*)(IMediaControl*);
using SetPositionsFn = HRESULT(STDMETHODCALLTYPE*)(IMediaSeeking*, LONGLONG*, DWORD, LONGLONG*, DWORD);
using GetPositionFn = HRESULT(STDMETHODCALLTYPE*)(IMediaSeeking*, LONGLONG*);
using MemReceiveFn = HRESULT(STDMETHODCALLTYPE*)(IMemInputPin*, IMediaSample*);
using PinOneFn = HRESULT(STDMETHODCALLTYPE*)(IPin*);
using DsStopFn = HRESULT(WINAPI*)(IDirectSoundBuffer*);

static ControlPauseFn s_origControlPause = nullptr;
static ControlStopFn s_origControlStop = nullptr;
static SetPositionsFn s_origSetPositions = nullptr;
static GetPositionFn s_origGetCurrentPosition = nullptr;
static GetPositionFn s_origGetStopPosition = nullptr;
static MemReceiveFn s_origMemReceive = nullptr;
static PinOneFn s_origUpstreamEos = nullptr;
static PinOneFn s_origDownstreamEos = nullptr;
static DsStopFn s_origDsStop = nullptr;

// Trampoline dispatchers
static HRESULT STDMETHODCALLTYPE hookControlPause(IMediaControl* self);
static HRESULT STDMETHODCALLTYPE hookControlStop(IMediaControl* self);
static HRESULT STDMETHODCALLTYPE hookSetPositions(IMediaSeeking* self, LONGLONG* pCurrent, DWORD dwCurrentFlags,
                                                  LONGLONG* pStop, DWORD dwStopFlags);
static HRESULT STDMETHODCALLTYPE hookGetCurrentPosition(IMediaSeeking* self, LONGLONG* pCurrent);
static HRESULT STDMETHODCALLTYPE hookGetStopPosition(IMediaSeeking* self, LONGLONG* pStop);
static HRESULT STDMETHODCALLTYPE hookMemReceive(IMemInputPin* self, IMediaSample* pSample);
static HRESULT STDMETHODCALLTYPE hookUpstreamEos(IPin* self);
static HRESULT STDMETHODCALLTYPE hookDownstreamEos(IPin* self);
static HRESULT WINAPI hookDsStop(IDirectSoundBuffer* self);

// 8. Test Harness encapsulating COM entry points and state machine
class ControlEntryHarness {
public:
    MockFilterGraphEx* mockGraph = nullptr;
    MockMediaControl* mockControl = nullptr;
    MockMediaSeekingEx* mockSeeking = nullptr;
    MockUpstreamPin* mockUpstreamPin = nullptr;
    MockDownstreamPin* mockDownstreamPin = nullptr;
    MockDirectSoundBufferEx* mockDsBuffer = nullptr;

    // Standard COM interface pointers for true vtable dispatch
    IMediaControl* pControl = nullptr;
    IMediaSeeking* pSeeking = nullptr;
    IPin* pUpstreamPin = nullptr;
    IPin* pDownstreamPin = nullptr;
    IMemInputPin* pDownstreamMem = nullptr;
    IDirectSoundBuffer* pDsBuffer = nullptr;

    eosTransition::Tracker tracker;
    CallerContext currentCallerContext = CallerContext::EngineAutoChange;

    DWORD deferralTimeoutMs = 500;
    bool inInternalEosDelivery = false;
    std::atomic<bool> deferralStarted{false};
    bool manualPauseObserved = false;
    bool stopCancelledDeferral = false;
    bool seekCancelledDeferral = false;
    bool trackChangeCancelledDeferral = false;
    int dsStopCallCount = 0;

    ControlEntryHarness() {
        mockControl = new MockMediaControl();
        mockSeeking = new MockMediaSeekingEx();
        mockUpstreamPin = new MockUpstreamPin();
        mockDownstreamPin = new MockDownstreamPin();
        mockDsBuffer = new MockDirectSoundBufferEx();
        mockGraph = new MockFilterGraphEx();
        mockGraph->control = mockControl;
        mockGraph->seeking = mockSeeking;

        pControl = static_cast<IMediaControl*>(mockControl);
        pSeeking = static_cast<IMediaSeeking*>(mockSeeking);
        pUpstreamPin = static_cast<IPin*>(mockUpstreamPin);
        pDownstreamPin = static_cast<IPin*>(mockDownstreamPin);
        pDownstreamMem = static_cast<IMemInputPin*>(mockDownstreamPin);
        pDsBuffer = static_cast<IDirectSoundBuffer*>(mockDsBuffer);

        s_activeHarness = this;
        installHooks();
    }

    ~ControlEntryHarness() {
        uninstallHooks();
        s_activeHarness = nullptr;

        mockGraph->Release();
        mockControl->Release();
        mockSeeking->Release();
        mockUpstreamPin->Release();
        mockDownstreamPin->Release();
        mockDsBuffer->Release();
    }

    void initializeGraph(DWORD rate = 48000, WORD channels = 2, WORD bits = 24) {
        tracker.onGraphActivated(mockGraph, bits == 24, rate, channels, bits);
    }

    void startSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate = 1.0) {
        tracker.onUpstreamNewSegment(mockGraph, start, stop, rate);
        mockSeeking->currentPos = start;
        mockSeeking->stopPos = stop;
        mockSeeking->duration = stop;
        tracker.onStopPositionReported(mockGraph, stop);
    }

    void setPreReadCandidate(bool eligible24Applied, DWORD nextRate, WORD nextChannels, WORD nextBits) {
        tracker.onPreReadFormatCandidate(mockGraph, eligible24Applied, nextRate, nextChannels, nextBits);
    }

    void setPreReadDecision(eosTransition::PreReadDecision decision) {
        tracker.onPreReadDecision(mockGraph, decision);
    }

    void manualTrackChange() {
        if (tracker.findGraph(mockGraph) && tracker.findGraph(mockGraph)->isPauseDeferralActive()) {
            tracker.cancelFormatChangePause(mockGraph, eosTransition::PauseDeferralStatus::CancelledTrackChange);
            trackChangeCancelledDeferral = true;
        }
        tracker.onManualTrackChange(mockGraph);
    }

    void feedUpstreamChunk(uint32_t bytes, bool hasTime = false, REFERENCE_TIME sStart = 0, REFERENCE_TIME sEnd = 0) {
        tracker.onUpstreamSamplePreReceive(mockGraph, bytes);
        tracker.onUpstreamSample(mockGraph, bytes, hasTime, sStart, sEnd);
    }

    HRESULT feedDownstreamChunk(const void* data, size_t size, bool hasTime = false,
                                REFERENCE_TIME sStart = 0, REFERENCE_TIME sEnd = 0) {
        auto* sample = new MockMediaSample(data, size, hasTime, sStart, sEnd);
        HRESULT hr = pDownstreamMem->Receive(sample);
        sample->Release();
        return hr;
    }

    // Hook callbacks
    HRESULT onControlPause(IMediaControl* self) {
        bool isEngineCall = (currentCallerContext == CallerContext::EngineAutoChange);

        if (tracker.findGraph(mockGraph) && tracker.findGraph(mockGraph)->isPauseDeferralActive() && !isEngineCall) {
            tracker.cancelFormatChangePause(mockGraph, eosTransition::PauseDeferralStatus::CancelledManualPause);
            manualPauseObserved = true;
            return s_origControlPause(self);
        }

        if (isEngineCall && tracker.isFormatChangePending(mockGraph)) {
            tracker.markFormatChangePauseDeferred(mockGraph);
            deferralStarted.store(true);

            DWORD startTick = GetTickCount();
            DWORD maxWait = deferralTimeoutMs;

            while (true) {
                const auto* st = tracker.findGraph(mockGraph);
                if (!st || !st->isPauseDeferralActive()) {
                    break;
                }

                LONGLONG c = 0, s = 0;
                if (SUCCEEDED(pSeeking->GetCurrentPosition(&c)) &&
                    SUCCEEDED(pSeeking->GetStopPosition(&s))) {
                    if (c >= s && s > 0) {
                        tracker.onFormatChangePauseCompleted(mockGraph, eosTransition::PauseDeferralStatus::CompletedDrain);
                        break;
                    }
                }

                if (GetTickCount() - startTick >= maxWait) {
                    tracker.onFormatChangePauseCompleted(mockGraph, eosTransition::PauseDeferralStatus::TimedOut);
                    break;
                }

                MSG msg;
                bool quitReceived = false;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        quitReceived = true;
                        tracker.cancelFormatChangePause(mockGraph, eosTransition::PauseDeferralStatus::CancelledQuit);
                        PostQuitMessage(static_cast<int>(msg.wParam));
                        break;
                    }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (quitReceived) break;

                Sleep(5);
            }
            return S_OK;
        }

        return s_origControlPause(self);
    }

    HRESULT onControlStop(IMediaControl* self) {
        if (tracker.findGraph(mockGraph) && tracker.findGraph(mockGraph)->isPauseDeferralActive()) {
            tracker.cancelFormatChangePause(mockGraph, eosTransition::PauseDeferralStatus::CancelledStop);
            stopCancelledDeferral = true;
        }
        tracker.onStop(mockGraph);
        return s_origControlStop(self);
    }

    HRESULT onSetPositions(IMediaSeeking* self, LONGLONG* pCurrent, DWORD dwCurrentFlags,
                           LONGLONG* pStop, DWORD dwStopFlags) {
        DWORD currBits = dwCurrentFlags & AM_SEEKING_PositioningBitsMask;
        DWORD stopBits = dwStopFlags & AM_SEEKING_PositioningBitsMask;
        bool modifiesCurr = (currBits != AM_SEEKING_NoPositioning) && (pCurrent != nullptr);
        bool modifiesStop = (stopBits != AM_SEEKING_NoPositioning) && (pStop != nullptr);

        if (modifiesCurr || modifiesStop) {
            if (tracker.findGraph(mockGraph) && tracker.findGraph(mockGraph)->isPauseDeferralActive()) {
                tracker.cancelFormatChangePause(mockGraph, eosTransition::PauseDeferralStatus::CancelledSeek);
                seekCancelledDeferral = true;
            }
            LONGLONG newPos = modifiesCurr ? *pCurrent : -1;
            LONGLONG newStop = modifiesStop ? *pStop : -1;
            tracker.onSeekPositions(mockGraph, modifiesCurr, newPos, modifiesStop, newStop);
        }
        return s_origSetPositions(self, pCurrent, dwCurrentFlags, pStop, dwStopFlags);
    }

    HRESULT onGetCurrentPosition(IMediaSeeking* self, LONGLONG* pCurrent) {
        auto hr = s_origGetCurrentPosition(self, pCurrent);
        if (SUCCEEDED(hr) && pCurrent) {
            tracker.onCurrentPositionReported(mockGraph, *pCurrent);
        }
        return hr;
    }

    HRESULT onGetStopPosition(IMediaSeeking* self, LONGLONG* pStop) {
        auto hr = s_origGetStopPosition(self, pStop);
        if (SUCCEEDED(hr) && pStop) {
            tracker.onStopPositionReported(mockGraph, *pStop);
        }
        return hr;
    }

    HRESULT onMemReceive(IMemInputPin* self, IMediaSample* pSample) {
        BYTE* ptr = nullptr;
        long len = 0;
        if (pSample && SUCCEEDED(pSample->GetPointer(&ptr)) && ptr) {
            len = pSample->GetActualDataLength();
        }
        REFERENCE_TIME sStart = 0, sEnd = 0;
        bool hasTime = (pSample && pSample->GetTime(&sStart, &sEnd) == S_OK);

        HRESULT hr = s_origMemReceive(self, pSample);

        if (len > 0) {
            bool deliverEos = tracker.onDownstreamSample(mockGraph, static_cast<uint32_t>(len), hasTime, sStart, sEnd);
            if (deliverEos) {
                inInternalEosDelivery = true;
                pDownstreamPin->EndOfStream();
                inInternalEosDelivery = false;
            }
        }
        return hr;
    }

    HRESULT onUpstreamPinEos(IPin* self) {
        bool deliverNow = tracker.onUpstreamEndOfStream(mockGraph);
        if (deliverNow) {
            inInternalEosDelivery = true;
            pDownstreamPin->EndOfStream();
            inInternalEosDelivery = false;
        }
        return s_origUpstreamEos(self);
    }

    HRESULT onDownstreamPinEos(IPin* self) {
        if (inInternalEosDelivery) {
            return s_origDownstreamEos(self);
        }
        if (!tracker.onDownstreamEndOfStream(mockGraph)) {
            return S_FALSE; // Reject duplicate or cancelled EOS
        }
        return s_origDownstreamEos(self);
    }

    HRESULT onDsStop(IDirectSoundBuffer* self) {
        ++dsStopCallCount;
        return s_origDsStop(self);
    }

private:
    void installHooks() {
        MH_Initialize();

        auto controlVtable = *reinterpret_cast<void***>(pControl);
        auto seekingVtable = *reinterpret_cast<void***>(pSeeking);
        auto memVtable = *reinterpret_cast<void***>(pDownstreamMem);
        auto upstreamPinVtable = *reinterpret_cast<void***>(pUpstreamPin);
        auto downstreamPinVtable = *reinterpret_cast<void***>(pDownstreamPin);
        auto dsVtable = *reinterpret_cast<void***>(pDsBuffer);

        MH_CreateHook(controlVtable[8], reinterpret_cast<void*>(hookControlPause), reinterpret_cast<void**>(&s_origControlPause));
        MH_CreateHook(controlVtable[9], reinterpret_cast<void*>(hookControlStop), reinterpret_cast<void**>(&s_origControlStop));
        MH_CreateHook(seekingVtable[11], reinterpret_cast<void*>(hookGetStopPosition), reinterpret_cast<void**>(&s_origGetStopPosition));
        MH_CreateHook(seekingVtable[12], reinterpret_cast<void*>(hookGetCurrentPosition), reinterpret_cast<void**>(&s_origGetCurrentPosition));
        MH_CreateHook(seekingVtable[14], reinterpret_cast<void*>(hookSetPositions), reinterpret_cast<void**>(&s_origSetPositions));
        MH_CreateHook(memVtable[6], reinterpret_cast<void*>(hookMemReceive), reinterpret_cast<void**>(&s_origMemReceive));
        MH_CreateHook(upstreamPinVtable[14], reinterpret_cast<void*>(hookUpstreamEos), reinterpret_cast<void**>(&s_origUpstreamEos));
        MH_CreateHook(downstreamPinVtable[14], reinterpret_cast<void*>(hookDownstreamEos), reinterpret_cast<void**>(&s_origDownstreamEos));
        MH_CreateHook(dsVtable[18], reinterpret_cast<void*>(hookDsStop), reinterpret_cast<void**>(&s_origDsStop));

        MH_EnableHook(MH_ALL_HOOKS);
    }

    void uninstallHooks() {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
};

static HRESULT STDMETHODCALLTYPE hookControlPause(IMediaControl* self) {
    return s_activeHarness ? s_activeHarness->onControlPause(self) : S_OK;
}
static HRESULT STDMETHODCALLTYPE hookControlStop(IMediaControl* self) {
    return s_activeHarness ? s_activeHarness->onControlStop(self) : S_OK;
}
static HRESULT STDMETHODCALLTYPE hookSetPositions(IMediaSeeking* self, LONGLONG* pCurrent, DWORD dwCurrentFlags,
                                                  LONGLONG* pStop, DWORD dwStopFlags) {
    return s_activeHarness ? s_activeHarness->onSetPositions(self, pCurrent, dwCurrentFlags, pStop, dwStopFlags) : S_OK;
}
static HRESULT STDMETHODCALLTYPE hookGetCurrentPosition(IMediaSeeking* self, LONGLONG* pCurrent) {
    return s_activeHarness ? s_activeHarness->onGetCurrentPosition(self, pCurrent) : S_OK;
}
static HRESULT STDMETHODCALLTYPE hookGetStopPosition(IMediaSeeking* self, LONGLONG* pStop) {
    return s_activeHarness ? s_activeHarness->onGetStopPosition(self, pStop) : S_OK;
}
static HRESULT STDMETHODCALLTYPE hookMemReceive(IMemInputPin* self, IMediaSample* pSample) {
    return s_activeHarness ? s_activeHarness->onMemReceive(self, pSample) : S_OK;
}
static HRESULT STDMETHODCALLTYPE hookUpstreamEos(IPin* self) {
    return s_activeHarness ? s_activeHarness->onUpstreamPinEos(self) : S_OK;
}
static HRESULT STDMETHODCALLTYPE hookDownstreamEos(IPin* self) {
    return s_activeHarness ? s_activeHarness->onDownstreamPinEos(self) : S_OK;
}
static HRESULT WINAPI hookDsStop(IDirectSoundBuffer* self) {
    return s_activeHarness ? s_activeHarness->onDsStop(self) : S_OK;
}

// ------------------------------------------------------------------------------------------------
// Test 29: [Conflict 1] No Manual Operation: Full Drain -> Single EOS -> Next Track Start
// ------------------------------------------------------------------------------------------------
inline void testAutoTrackNoManualOpCleanDrain() {
    std::printf("[TEST 29] Conflict 1: No manual op -> full drain -> single EOS -> next track start...\n");
    ControlEntryHarness harness;
    harness.initializeGraph(48000, 2, 24);

    const size_t track1Frames = 24000; // 500ms at 48kHz
    std::vector<uint8_t> pcmStream;
    trackTail::appendSynthesizedTrack(pcmStream, 48000, 24, track1Frames, false);

    const REFERENCE_TIME segDuration = static_cast<REFERENCE_TIME>((track1Frames * 10000000ULL) / 48000ULL);
    harness.startSegment(0, segDuration, 1.0);

    // Upstream finishes decoding 5 chunks of 100ms
    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 5; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        harness.feedUpstreamChunk(chunkBytes, true, sStart, sEnd);
    }

    // Upstream finishes decoding -> calls IPin::EndOfStream() via control entry point
    CHECK(SUCCEEDED(harness.pUpstreamPin->EndOfStream()));

    // Next track is 16-bit FLAC -> Pre-read candidate reports FormatChanged
    harness.setPreReadCandidate(false, 48000, 2, 16);

    // Verify format change pending
    CHECK(harness.tracker.isFormatChangePending(harness.mockGraph) == true);

    // Audio streaming thread pushes first 4 chunks through IMemInputPin::Receive
    for (int i = 0; i < 4; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        const uint8_t* ptr = pcmStream.data() + i * chunkBytes;
        CHECK(SUCCEEDED(harness.feedDownstreamChunk(ptr, chunkBytes, true, sStart, sEnd)));
        harness.mockSeeking->currentPos = sEnd;
    }
    CHECK(harness.mockDownstreamPin->eosCallCount == 0); // Not end of segment yet

    // DirectSound background thread runs: will advance clock and feed final chunk
    std::thread streamingThread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        REFERENCE_TIME sStart = 4000000LL;
        REFERENCE_TIME sEnd = 5000000LL;
        const uint8_t* ptr = pcmStream.data() + 4 * chunkBytes;
        harness.feedDownstreamChunk(ptr, chunkBytes, true, sStart, sEnd);

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        harness.mockSeeking->currentPos = segDuration;
    });

    // Engine initiates auto track change -> calls IMediaControl::Pause() via COM entry point
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(SUCCEEDED(harness.pControl->Pause()));

    streamingThread.join();

    // Verify auto-deferral completed cleanly via drain
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CompletedDrain);
    CHECK(harness.mockDownstreamPin->eosCallCount == 1); // Exactly ONE EOS delivered downstream!

    // Verify Track 1 completeness with #19 analyzer
    auto a1 = trackTail::analyzeStream(harness.mockDownstreamPin->receivedPcm.data(),
                                      harness.mockDownstreamPin->receivedPcm.size(),
                                      48000, 24, track1Frames);
    CHECK(a1.tailComplete == true);
    CHECK(a1.missingTailFrames == 0);
    CHECK(a1.missingTailMs == 0.0);
    CHECK(a1.track1LastFrame == track1Frames);

    // Track 2 begins: append synthesized 16-bit track
    const size_t track2Frames = 12000;
    std::vector<uint8_t> track2Pcm;
    trackTail::appendSynthesizedTrack(track2Pcm, 48000, 16, track2Frames, true);
    for (size_t offset = 0; offset < track2Pcm.size(); offset += 2400 * 4) {
        harness.feedDownstreamChunk(track2Pcm.data() + offset, 2400 * 4, false, 0, 0);
    }
    CHECK(harness.mockDownstreamPin->receivedPcm.size() == (track1Frames * 6 + track2Frames * 4));
}

// ------------------------------------------------------------------------------------------------
// Test 30: [Conflict 2A] Manual Pause Priority and Old EOS/Track Change Immunity
// ------------------------------------------------------------------------------------------------
inline void testManualPausePriorityAndOldEosImmunity() {
    std::printf("[TEST 30] Conflict 2A: Manual Pause priority -> immediate cancellation, no stale EOS...\n");
    ControlEntryHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 50000000LL, 1.0);

    harness.feedUpstreamChunk(24000 * 6, true, 0, 50000000LL);
    CHECK(SUCCEEDED(harness.pUpstreamPin->EndOfStream()));
    harness.setPreReadCandidate(false, 48000, 2, 16);
    CHECK(harness.tracker.isFormatChangePending(harness.mockGraph) == true);

    std::atomic<bool> enginePausedStarted{false};
    std::atomic<bool> manualPauseExecuted{false};

    std::thread engineThread([&]() {
        harness.currentCallerContext = CallerContext::EngineAutoChange;
        enginePausedStarted.store(true);
        harness.pControl->Pause();
    });

    std::thread uiThread([&]() {
        int waitCount = 0;
        while ((!enginePausedStarted.load() || !harness.deferralStarted.load()) && waitCount++ < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        harness.currentCallerContext = CallerContext::UserManualOp;
        CHECK(SUCCEEDED(harness.pControl->Pause()));
        manualPauseExecuted.store(true);
    });

    engineThread.join();
    uiThread.join();

    CHECK(manualPauseExecuted.load() == true);
    CHECK(harness.manualPauseObserved == true);

    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledManualPause);
    CHECK(st->isPauseDeferralActive() == false);
    CHECK(harness.tracker.isFormatChangePending(harness.mockGraph) == false);

    // Stale downstream EOS rejected
    CHECK(harness.pDownstreamPin->EndOfStream() == S_FALSE);
    CHECK(harness.mockDownstreamPin->eosCallCount == 0);
    CHECK(st->downstreamEosDeliveredCount == 0);
}

// ------------------------------------------------------------------------------------------------
// Test 31: [Conflict 2B] Manual Seek Priority and Old EOS Immunity
// ------------------------------------------------------------------------------------------------
inline void testManualSeekPriorityAndOldEosImmunity() {
    std::printf("[TEST 31] Conflict 2B: Manual Seek priority -> immediate cancellation, reposition...\n");
    ControlEntryHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 50000000LL, 1.0);

    harness.feedUpstreamChunk(24000 * 6, true, 0, 50000000LL);
    CHECK(SUCCEEDED(harness.pUpstreamPin->EndOfStream()));
    harness.setPreReadCandidate(false, 48000, 2, 16);

    std::atomic<bool> engineThreadStarted{false};
    std::atomic<bool> seekExecuted{false};

    std::thread engineThread([&]() {
        harness.currentCallerContext = CallerContext::EngineAutoChange;
        engineThreadStarted.store(true);
        harness.pControl->Pause();
    });

    std::thread uiThread([&]() {
        int waitCount = 0;
        while ((!engineThreadStarted.load() || !harness.deferralStarted.load()) && waitCount++ < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        LONGLONG seekPos = 150000000LL;
        CHECK(SUCCEEDED(harness.pSeeking->SetPositions(&seekPos, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning)));
        seekExecuted.store(true);
    });

    engineThread.join();
    uiThread.join();

    CHECK(seekExecuted.load() == true);
    CHECK(harness.seekCancelledDeferral == true);
    CHECK(harness.mockSeeking->currentPos == 150000000LL);

    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledSeek);

    CHECK(harness.pDownstreamPin->EndOfStream() == S_FALSE);
    CHECK(harness.mockDownstreamPin->eosCallCount == 0);
    CHECK(harness.tracker.isFormatChangePending(harness.mockGraph) == false);
}

// ------------------------------------------------------------------------------------------------
// Test 32: [Conflict 2C] Manual Track Change Priority and Old Track Isolation
// ------------------------------------------------------------------------------------------------
inline void testManualTrackChangePriorityAndOldTrackIsolation() {
    std::printf("[TEST 32] Conflict 2C: Manual Track Change priority -> isolate old track, reset...\n");
    ControlEntryHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 50000000LL, 1.0);

    harness.feedUpstreamChunk(24000 * 6, true, 0, 50000000LL);
    CHECK(SUCCEEDED(harness.pUpstreamPin->EndOfStream()));
    harness.setPreReadCandidate(false, 48000, 2, 16);

    std::atomic<bool> engineThreadStarted{false};
    std::atomic<bool> trackChangeExecuted{false};

    std::thread engineThread([&]() {
        harness.currentCallerContext = CallerContext::EngineAutoChange;
        engineThreadStarted.store(true);
        harness.pControl->Pause();
    });

    std::thread uiThread([&]() {
        int waitCount = 0;
        while ((!engineThreadStarted.load() || !harness.deferralStarted.load()) && waitCount++ < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        harness.manualTrackChange();
        trackChangeExecuted.store(true);
    });

    engineThread.join();
    uiThread.join();

    CHECK(trackChangeExecuted.load() == true);
    CHECK(harness.trackChangeCancelledDeferral == true);

    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledTrackChange);

    CHECK(harness.pDownstreamPin->EndOfStream() == S_FALSE);
    CHECK(harness.mockDownstreamPin->eosCallCount == 0);

    harness.startSegment(0, 100000000LL, 1.0);
    harness.feedUpstreamChunk(4800 * 6, true, 0, 1000000LL);
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::None);
}

// ------------------------------------------------------------------------------------------------
// Test 33: [Conflict 2D] Manual Stop Priority and Old EOS Immunity
// ------------------------------------------------------------------------------------------------
inline void testManualStopPriorityAndOldEOSImmunity() {
    std::printf("[TEST 33] Conflict 2D: Manual Stop priority -> stop graph, abort deferral...\n");
    ControlEntryHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 50000000LL, 1.0);

    harness.feedUpstreamChunk(24000 * 6, true, 0, 50000000LL);
    CHECK(SUCCEEDED(harness.pUpstreamPin->EndOfStream()));
    harness.setPreReadCandidate(false, 48000, 2, 16);

    std::atomic<bool> engineThreadStarted{false};
    std::atomic<bool> stopExecuted{false};

    std::thread engineThread([&]() {
        harness.currentCallerContext = CallerContext::EngineAutoChange;
        engineThreadStarted.store(true);
        harness.pControl->Pause();
    });

    std::thread uiThread([&]() {
        int waitCount = 0;
        while ((!engineThreadStarted.load() || !harness.deferralStarted.load()) && waitCount++ < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        harness.currentCallerContext = CallerContext::UserManualOp;
        CHECK(SUCCEEDED(harness.pControl->Stop()));
        stopExecuted.store(true);
    });

    engineThread.join();
    uiThread.join();

    CHECK(stopExecuted.load() == true);
    CHECK(harness.stopCancelledDeferral == true);
    CHECK(harness.mockControl->state == State_Stopped);

    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledStop);

    CHECK(harness.pDownstreamPin->EndOfStream() == S_FALSE);
    CHECK(harness.mockDownstreamPin->eosCallCount == 0);
}

// ------------------------------------------------------------------------------------------------
// Test 34: [Conflict 2E] Application Exit (WM_QUIT) During Deferral
// ------------------------------------------------------------------------------------------------
inline void testApplicationExitDuringDeferral() {
    std::printf("[TEST 34] Conflict 2E: Application Exit (WM_QUIT) during deferral window...\n");
    ControlEntryHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 50000000LL, 1.0);

    harness.feedUpstreamChunk(24000 * 6, true, 0, 50000000LL);
    CHECK(SUCCEEDED(harness.pUpstreamPin->EndOfStream()));
    harness.setPreReadCandidate(false, 48000, 2, 16);

    std::atomic<bool> engineThreadStarted{false};
    std::atomic<bool> quitPosted{false};
    std::atomic<DWORD> engineTid{0};

    std::thread engineThread([&]() {
        MSG dummy;
        PeekMessageW(&dummy, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        engineTid.store(GetCurrentThreadId());
        harness.currentCallerContext = CallerContext::EngineAutoChange;
        engineThreadStarted.store(true);
        harness.pControl->Pause();
    });

    std::thread uiThread([&]() {
        int waitCount = 0;
        while ((!engineThreadStarted.load() || !harness.deferralStarted.load() || engineTid.load() == 0) && waitCount++ < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        PostThreadMessageW(engineTid.load(), WM_QUIT, 0, 0);
        quitPosted.store(true);
    });

    engineThread.join();
    uiThread.join();

    CHECK(quitPosted.load() == true);
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledQuit);
    CHECK(harness.mockDownstreamPin->eosCallCount == 0);
}

// ------------------------------------------------------------------------------------------------
// Test 35: [Conflict 3A] Same-Format Pre-Reading Bypass (Seamless Continuity)
// ------------------------------------------------------------------------------------------------
inline void testSameFormatPreReadBypassSeamlessContinuity() {
    std::printf("[TEST 35] Conflict 3A: Same-format pre-reading bypass -> seamless continuation...\n");
    ControlEntryHarness harness;
    harness.initializeGraph(48000, 2, 24);

    const size_t track1Frames = 24000;
    std::vector<uint8_t> track1Pcm;
    trackTail::appendSynthesizedTrack(track1Pcm, 48000, 24, track1Frames, false);

    const REFERENCE_TIME segDuration = static_cast<REFERENCE_TIME>((track1Frames * 10000000ULL) / 48000ULL);
    harness.startSegment(0, segDuration, 1.0);

    harness.feedUpstreamChunk(static_cast<uint32_t>(track1Pcm.size()), true, 0, segDuration);
    CHECK(SUCCEEDED(harness.pUpstreamPin->EndOfStream()));

    // Next track is SAME format (48kHz/24bit stereo)
    harness.setPreReadCandidate(true, 48000, 2, 24);

    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->preReadDecision == eosTransition::PreReadDecision::SameFormat24);
    CHECK(harness.tracker.isFormatChangePending(harness.mockGraph) == false);

    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(SUCCEEDED(harness.pControl->Pause()));
    CHECK(harness.deferralStarted.load() == false); // Not deferred!

    harness.feedDownstreamChunk(track1Pcm.data(), track1Pcm.size(), true, 0, segDuration);

    const size_t track2Frames = 12000;
    std::vector<uint8_t> track2Pcm;
    trackTail::appendSynthesizedTrack(track2Pcm, 48000, 24, track2Frames, true);
    harness.feedDownstreamChunk(track2Pcm.data(), track2Pcm.size(), true, segDuration, segDuration + 2500000LL);

    CHECK(harness.mockDownstreamPin->eosCallCount == 0);

    auto a = trackTail::analyzeStream(harness.mockDownstreamPin->receivedPcm.data(),
                                     track1Frames * 6,
                                     48000, 24, track1Frames);
    CHECK(a.tailComplete == true);
    CHECK(a.missingTailFrames == 0);
    CHECK(a.missingTailMs == 0.0);
}

// ------------------------------------------------------------------------------------------------
// Test 36: [Conflict 3B] Playlist End Drain and Clean Termination
// ------------------------------------------------------------------------------------------------
inline void testPlaylistEndDrainAndCleanTermination() {
    std::printf("[TEST 36] Conflict 3B: Playlist end (no next track) -> drain, single EOS, stop...\n");
    ControlEntryHarness harness;
    harness.initializeGraph(48000, 2, 24);

    const size_t track1Frames = 24000;
    std::vector<uint8_t> track1Pcm;
    trackTail::appendSynthesizedTrack(track1Pcm, 48000, 24, track1Frames, false);

    const REFERENCE_TIME segDuration = static_cast<REFERENCE_TIME>((track1Frames * 10000000ULL) / 48000ULL);
    harness.startSegment(0, segDuration, 1.0);

    harness.feedUpstreamChunk(static_cast<uint32_t>(track1Pcm.size()), true, 0, segDuration);
    CHECK(SUCCEEDED(harness.pUpstreamPin->EndOfStream()));

    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);
    CHECK(harness.tracker.isFormatChangePending(harness.mockGraph) == true);

    std::thread streamingThread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        harness.feedDownstreamChunk(track1Pcm.data(), track1Pcm.size(), true, 0, segDuration);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        harness.mockSeeking->currentPos = segDuration;
    });

    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(SUCCEEDED(harness.pControl->Pause()));

    streamingThread.join();

    CHECK(harness.mockDownstreamPin->eosCallCount == 1);
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CompletedDrain);

    auto a = trackTail::analyzeStream(harness.mockDownstreamPin->receivedPcm.data(),
                                     harness.mockDownstreamPin->receivedPcm.size(),
                                     48000, 24, track1Frames);
    CHECK(a.tailComplete == true);
    CHECK(a.missingTailFrames == 0);

    CHECK(SUCCEEDED(harness.pControl->Stop()));
    CHECK(harness.mockControl->state == State_Stopped);
}

// ------------------------------------------------------------------------------------------------
// Test 37: [Conflict 3C] Old Graph Destruction During Deferral
// ------------------------------------------------------------------------------------------------
inline void testOldGraphDestructionDuringDeferral() {
    std::printf("[TEST 37] Conflict 3C: Old graph destruction / release during deferral...\n");
    auto* harness = new ControlEntryHarness();
    harness->initializeGraph(48000, 2, 24);
    harness->startSegment(0, 50000000LL, 1.0);

    harness->feedUpstreamChunk(24000 * 6, true, 0, 50000000LL);
    CHECK(SUCCEEDED(harness->pUpstreamPin->EndOfStream()));
    harness->setPreReadCandidate(false, 48000, 2, 16);

    void* graphKey = harness->mockGraph;
    CHECK(harness->tracker.findGraph(graphKey) != nullptr);

    harness->tracker.markFormatChangePauseDeferred(graphKey);
    CHECK(harness->tracker.findGraph(graphKey)->isPauseDeferralActive() == true);

    delete harness;

    CHECK(s_activeHarness == nullptr);
}

// ------------------------------------------------------------------------------------------------
// Test 38: [Conflict 3D] Duplicate EOS Notification Rejection
// ------------------------------------------------------------------------------------------------
inline void testDuplicateEosNotificationRejection() {
    std::printf("[TEST 38] Conflict 3D: Duplicate upstream & downstream EOS notifications...\n");
    ControlEntryHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 50000000LL, 1.0);

    harness.feedUpstreamChunk(24000 * 6, true, 0, 50000000LL);

    // 1st Upstream EOS: valid
    CHECK(SUCCEEDED(harness.pUpstreamPin->EndOfStream()));

    // 2nd Upstream EOS: duplicate, rejected by tracker
    CHECK(SUCCEEDED(harness.pUpstreamPin->EndOfStream()));

    // 3rd Upstream EOS: duplicate, rejected
    CHECK(SUCCEEDED(harness.pUpstreamPin->EndOfStream()));

    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->upstreamEosCount == 3);

    // Feed downstream to target
    harness.feedDownstreamChunk(nullptr, 24000 * 6, true, 0, 50000000LL);

    // First downstream EOS fires from feedDownstreamChunk
    CHECK(harness.mockDownstreamPin->eosCallCount == 1);
    CHECK(st->downstreamEosDeliveredCount == 1);

    // Subsequent direct downstream EOS call must be rejected (returns S_FALSE)
    CHECK(harness.pDownstreamPin->EndOfStream() == S_FALSE);
    CHECK(harness.mockDownstreamPin->eosCallCount == 1); // Remains strictly 1!
    CHECK(st->downstreamEosDeliveredCount == 1);
}

// ------------------------------------------------------------------------------------------------
// Test 39: [Conflict 3E] Drain Failure Watchdog Timeout
// ------------------------------------------------------------------------------------------------
inline void testDrainFailureWatchdogTimeout() {
    std::printf("[TEST 39] Conflict 3E: Drain failure / hardware clock stall watchdog safety...\n");
    ControlEntryHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 50000000LL, 1.0);

    harness.feedUpstreamChunk(24000 * 6, true, 0, 50000000LL);
    CHECK(SUCCEEDED(harness.pUpstreamPin->EndOfStream()));
    harness.setPreReadCandidate(false, 48000, 2, 16);

    harness.deferralTimeoutMs = 50;
    harness.mockSeeking->currentPos = 0;

    DWORD t0 = GetTickCount();
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(SUCCEEDED(harness.pControl->Pause()));
    DWORD elapsed = GetTickCount() - t0;

    CHECK(elapsed >= 40);
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::TimedOut);
    CHECK(harness.tracker.isFormatChangePending(harness.mockGraph) == false);
}

inline void runAllConflictEntryPointTests() {
    std::printf("\n=== Issue #30 Control Entry Point Conflict Verification Tests ===\n");
    testAutoTrackNoManualOpCleanDrain();
    testManualPausePriorityAndOldEosImmunity();
    testManualSeekPriorityAndOldEosImmunity();
    testManualTrackChangePriorityAndOldTrackIsolation();
    testManualStopPriorityAndOldEOSImmunity();
    testApplicationExitDuringDeferral();
    testSameFormatPreReadBypassSeamlessContinuity();
    testPlaylistEndDrainAndCleanTermination();
    testOldGraphDestructionDuringDeferral();
    testDuplicateEosNotificationRejection();
    testDrainFailureWatchdogTimeout();
    std::printf("=== Issue #30 Control Entry Point Tests Completed ===\n\n");
}

} // namespace conflictTests
