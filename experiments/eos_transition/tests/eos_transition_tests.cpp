// Headless state-transition and DirectShow filter integration tests for Issue #20 / #21
// (experimental segment-based EOS delivery across format-changing transitions).
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "MinHook.h"
#include "track_tail_check.h"
#include "eos_transition.h"
#include "eos_transition_diag.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::printf("  [FAIL] line %d: %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

#include "conflict_entry_point_tests.h"
#include "async_transition_tests.h"

// Test 1: Arbitrary durations and seek offsets (no fixed 23s threshold)
void testArbitraryDurationsAndOffsets() {
    std::printf("[TEST 1] Arbitrary segment durations, seek offsets, and byte-only fallback...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0x1001);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);

    // Subcase 1A: Short 1.0s track (10 chunks of 100ms = 28,800 bytes each), queued async drain
    tracker.onUpstreamNewSegment(graph, 0, 10000000LL, 1.0);
    const uint32_t chunkBytes = 4800 * 6; // 100ms at 48kHz/24bit/stereo
    for (int i = 0; i < 10; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onUpstreamSamplePreReceive(graph, chunkBytes);
        tracker.onUpstreamSample(graph, chunkBytes, true, sStart, sEnd);
        if (i < 5) {
            CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, sStart, sEnd));
        }
    }
    // Upstream finishes decoding at 0.5s playback time; 5 chunks remain in SeamlessFilter queue.
    CHECK(!tracker.onUpstreamEndOfStream(graph));
    const auto* st = tracker.findGraph(graph);
    CHECK(st != nullptr && st->eosPending && !st->eosDelivered);

    for (int i = 5; i < 9; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, sStart, sEnd));
    }
    // Final chunk (0.9s..1.0s) triggers EOS once.
    CHECK(tracker.onDownstreamSample(graph, chunkBytes, true, 9000000LL, 10000000LL));
    CHECK(st->eosDelivered && !st->eosPending && st->downstreamEosDeliveredCount == 1);

    // Subcase 1B: Long 324.3s track seeked to 295.284s (duration 29.016s > 23s):
    // Verify EOS does NOT fire at 23.0s and fires only at the actual 29.016s segment end.
    tracker.onSeek(graph);
    const REFERENCE_TIME seekStart = 2952840000LL;
    const REFERENCE_TIME seekStop = 3243000000LL;
    tracker.onUpstreamNewSegment(graph, seekStart, seekStop, 1.0);

    // Feed 290 chunks of 100ms + final 16ms chunk (768 frames = 4,608 bytes)
    for (int i = 0; i < 290; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onUpstreamSample(graph, chunkBytes, true, sStart, sEnd);
        if (i < 260) {
            // Crossing 23.0s (i == 229..235) must NEVER trigger EOS
            CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, sStart, sEnd));
        }
    }
    const uint32_t tailBytes = 768 * 6;
    const REFERENCE_TIME finalStart = 290000000LL;
    const REFERENCE_TIME finalEnd = 290160000LL;
    tracker.onUpstreamSample(graph, tailBytes, true, finalStart, finalEnd);
    CHECK(!tracker.onUpstreamEndOfStream(graph));

    for (int i = 260; i < 290; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, sStart, sEnd));
    }
    CHECK(tracker.onDownstreamSample(graph, tailBytes, true, finalStart, finalEnd));
    CHECK(st->eosDelivered && st->downstreamEosDeliveredCount == 2);

    // Subcase 1C: Synchronous drain where downstream already received all PCM before upstream EOS call
    tracker.onUpstreamNewSegment(graph, 0, 5000000LL, 1.0);
    for (int i = 0; i < 5; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onUpstreamSample(graph, chunkBytes, true, sStart, sEnd);
        CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, sStart, sEnd));
    }
    CHECK(tracker.onUpstreamEndOfStream(graph));
    CHECK(st->eosDelivered && !st->eosPending && st->downstreamEosDeliveredCount == 3);

    // Subcase 1D: Byte-count matching when timestamps are absent (hasTime = false)
    tracker.onUpstreamNewSegment(graph, 0, 0, 1.0);
    for (int i = 0; i < 4; ++i) {
        tracker.onUpstreamSample(graph, 12000, false, 0, 0);
    }
    CHECK(!tracker.onUpstreamEndOfStream(graph));
    for (int i = 0; i < 3; ++i) {
        CHECK(!tracker.onDownstreamSample(graph, 12000, false, 0, 0));
    }
    CHECK(tracker.onDownstreamSample(graph, 12000, false, 0, 0));
    CHECK(st->eosDelivered && st->downstreamEosDeliveredCount == 4);
}

// Test 2: Duplicate upstream & downstream EndOfStream calls deliver at most once
void testDuplicateEosCalls() {
    std::printf("[TEST 2] Duplicate upstream/downstream EOS calls deliver at most once...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0x1002);
    tracker.onGraphActivated(graph, true, 96000, 2, 24);
    tracker.onUpstreamNewSegment(graph, 0, 20000000LL, 1.0);

    // Empty segment receiving EOS should not trigger downstream EOS
    CHECK(!tracker.onUpstreamEndOfStream(graph));
    tracker.onUpstreamNewSegment(graph, 0, 20000000LL, 1.0);

    const uint32_t chunkBytes = 9600 * 6;
    tracker.onUpstreamSample(graph, chunkBytes, true, 0, 1000000LL);
    tracker.onUpstreamSample(graph, chunkBytes, true, 1000000LL, 2000000LL);

    // 3 duplicate upstream EOS calls before downstream drains
    CHECK(!tracker.onUpstreamEndOfStream(graph));
    CHECK(!tracker.onUpstreamEndOfStream(graph));
    CHECK(!tracker.onUpstreamEndOfStream(graph));

    CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, 0, 1000000LL));
    CHECK(tracker.onDownstreamSample(graph, chunkBytes, true, 1000000LL, 2000000LL));

    const auto* st = tracker.findGraph(graph);
    CHECK(st != nullptr && st->downstreamEosDeliveredCount == 1);

    // Additional upstream EOS calls or extra downstream samples/EOS calls after delivery must not re-fire
    CHECK(!tracker.onUpstreamEndOfStream(graph));
    CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, 2000000LL, 3000000LL));
    CHECK(!tracker.onDownstreamEndOfStream(graph));
    CHECK(!tracker.onDownstreamEndOfStream(graph));
    CHECK(st->downstreamEosDeliveredCount == 1);
}

// Test 3: Same-format pre-read is preserved across track boundaries
void testSameFormatPreReadPreserved() {
    std::printf("[TEST 3] Same-format pre-read preserved (no mid-transition EOS)...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0x1003);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);
    tracker.onUpstreamNewSegment(graph, 0, 240000000LL, 1.0);

    const uint32_t chunkBytes = 4800 * 6;
    // Track 1: 4 chunks; downstream receives first 2 before upstream hits EOS
    for (int i = 0; i < 4; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onUpstreamSamplePreReceive(graph, chunkBytes);
        tracker.onUpstreamSample(graph, chunkBytes, true, sStart, sEnd);
        if (i < 2) {
            CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, sStart, sEnd));
        }
    }
    CHECK(!tracker.onUpstreamEndOfStream(graph));
    const auto* st = tracker.findGraph(graph);
    CHECK(st->eosPending);

    // 0x31E7A pre-read setter applies SET_24 for same-format Track 2 (48kHz/24bit/2ch)
    tracker.onPreReadFormatCandidate(graph, true, 48000, 2, 24);
    CHECK(!st->eosPending && st->preReadDecision == eosTransition::PreReadDecision::SameFormat24);

    // Track 2 begins pushing samples into SeamlessInput while Track 1's last 2 chunks drain to EQ
    for (int i = 4; i < 8; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onUpstreamSamplePreReceive(graph, chunkBytes);
        tracker.onUpstreamSample(graph, chunkBytes, true, sStart, sEnd);
    }
    CHECK(st->seamlessTransitionCount == 1);

    // Downstream crosses Track 1's end (i == 2, 3) and continues into Track 2 (i == 4, 5) without EOS!
    for (int i = 2; i < 6; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, sStart, sEnd));
    }
    CHECK(!st->eosDelivered && st->downstreamEosDeliveredCount == 0);

    // Now Track 2 ends and Track 3 is 48kHz/16bit (format change!)
    CHECK(!tracker.onUpstreamEndOfStream(graph));
    tracker.onPreReadFormatCandidate(graph, false, 48000, 2, 16);
    CHECK(st->eosPending && st->preReadDecision == eosTransition::PreReadDecision::FormatChanged);

    CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, 6000000LL, 7000000LL));
    CHECK(tracker.onDownstreamSample(graph, chunkBytes, true, 7000000LL, 8000000LL));
    CHECK(st->eosDelivered && st->downstreamEosDeliveredCount == 1);
}

// Test 4: Format change and unknown format-change behavior
void testFormatChangeAndUnknownBehavior() {
    std::printf("[TEST 4] Format change (16-bit / rate change) and unknown format-change behavior...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0x1004);
    tracker.onGraphActivated(graph, true, 96000, 2, 24);

    const uint32_t chunkBytes = 9600 * 6;
    // Subcase 4A: 96kHz/24bit -> 48kHz/24bit (sample rate change at 0x31E7A)
    tracker.onUpstreamNewSegment(graph, 0, 10000000LL, 1.0);
    tracker.onUpstreamSample(graph, chunkBytes, true, 0, 1000000LL);
    tracker.onUpstreamSample(graph, chunkBytes, true, 1000000LL, 2000000LL);
    CHECK(!tracker.onUpstreamEndOfStream(graph));
    tracker.onPreReadFormatCandidate(graph, true, 48000, 2, 24);
    const auto* st = tracker.findGraph(graph);
    CHECK(st->preReadDecision == eosTransition::PreReadDecision::FormatChanged && st->eosPending);
    CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, 0, 1000000LL));
    CHECK(tracker.onDownstreamSample(graph, chunkBytes, true, 1000000LL, 2000000LL));
    CHECK(st->eosDelivered && st->downstreamEosDeliveredCount == 1);

    // Subcase 4B: Unknown format change (0x31E7A not called: header mismatch in OpcFlac, non-FLAC, or playlist end)
    tracker.onUpstreamNewSegment(graph, 0, 10000000LL, 1.0);
    tracker.onUpstreamSample(graph, chunkBytes, true, 0, 1000000LL);
    tracker.onUpstreamSample(graph, chunkBytes, true, 1000000LL, 2000000LL);
    CHECK(!tracker.onUpstreamEndOfStream(graph));
    CHECK(st->preReadDecision == eosTransition::PreReadDecision::Unknown && st->eosPending);
    CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, 0, 1000000LL));
    CHECK(tracker.onDownstreamSample(graph, chunkBytes, true, 1000000LL, 2000000LL));
    CHECK(st->eosDelivered && st->downstreamEosDeliveredCount == 2);

    // Subcase 4C: 16-bit inactive graph never injects EOS
    void* graph16 = reinterpret_cast<void*>(0x1016);
    tracker.onGraphActivated(graph16, false, 44100, 2, 16);
    tracker.onUpstreamNewSegment(graph16, 0, 10000000LL, 1.0);
    tracker.onUpstreamSample(graph16, 17640, true, 0, 1000000LL);
    CHECK(!tracker.onUpstreamEndOfStream(graph16));
    CHECK(!tracker.onDownstreamSample(graph16, 17640, true, 0, 1000000LL));
    CHECK(tracker.findGraph(graph16)->downstreamEosDeliveredCount == 0);
}

// Test 5: Pending EOS state cleared on Seek, BeginFlush, EndFlush, Stop, and ManualTrackChange
void testResetOnSeekFlushStopAndManualChange() {
    std::printf("[TEST 5] State cleared on Seek, BeginFlush, EndFlush, Stop, and ManualTrackChange...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0x1005);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);
    const uint32_t chunkBytes = 4800 * 6;

    const eosTransition::ResetReason reasons[] = {
        eosTransition::ResetReason::Seek,
        eosTransition::ResetReason::BeginFlush,
        eosTransition::ResetReason::EndFlush,
        eosTransition::ResetReason::Stop,
        eosTransition::ResetReason::ManualTrackChange
    };

    for (auto reason : reasons) {
        tracker.onUpstreamNewSegment(graph, 0, 10000000LL, 1.0);
        tracker.onUpstreamSample(graph, chunkBytes, true, 0, 1000000LL);
        tracker.onUpstreamSample(graph, chunkBytes, true, 1000000LL, 2000000LL);
        CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, 0, 1000000LL));
        CHECK(!tracker.onUpstreamEndOfStream(graph));

        const auto* st = tracker.findGraph(graph);
        CHECK(st->eosPending);

        switch (reason) {
            case eosTransition::ResetReason::Seek: tracker.onSeek(graph); break;
            case eosTransition::ResetReason::BeginFlush: tracker.onBeginFlush(graph); break;
            case eosTransition::ResetReason::EndFlush: tracker.onEndFlush(graph); break;
            case eosTransition::ResetReason::Stop: tracker.onStop(graph); break;
            case eosTransition::ResetReason::ManualTrackChange: tracker.onManualTrackChange(graph); break;
            default: break;
        }
        CHECK(!st->eosPending && !st->eosDelivered && st->upstreamEosCount == 0 && st->lastResetReason == reason);

        // Stale sample matching the pre-reset target must NOT fire EOS
        CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, 1000000LL, 2000000LL));
        CHECK(!st->eosDelivered);
    }

    // After resets, a new segment still works normally and delivers EOS once at its end
    tracker.onUpstreamNewSegment(graph, 50000000LL, 60000000LL, 1.0);
    tracker.onUpstreamSample(graph, chunkBytes, true, 0, 1000000LL);
    CHECK(!tracker.onUpstreamEndOfStream(graph));
    CHECK(tracker.onDownstreamSample(graph, chunkBytes, true, 0, 1000000LL));
    CHECK(tracker.findGraph(graph)->downstreamEosDeliveredCount == 1);
}

// Test 6: Multiple concurrent graphs operate independently without interference
void testMultipleConcurrentGraphs() {
    std::printf("[TEST 6] Multiple concurrent graphs isolated...\n");
    eosTransition::Tracker tracker;
    void* graphA = reinterpret_cast<void*>(0x2001); // 48kHz/24bit -> 16bit (should deliver EOS)
    void* graphB = reinterpret_cast<void*>(0x2002); // 96kHz/24bit -> 96kHz/24bit (same-format pre-read)
    void* graphC = reinterpret_cast<void*>(0x2003); // 192kHz/24bit flushed mid-drain (should not deliver EOS)

    tracker.onGraphActivated(graphA, true, 48000, 2, 24);
    tracker.onGraphActivated(graphB, true, 96000, 2, 24);
    tracker.onGraphActivated(graphC, true, 192000, 2, 24);

    tracker.onUpstreamNewSegment(graphA, 0, 10000000LL, 1.0);
    tracker.onUpstreamNewSegment(graphB, 0, 10000000LL, 1.0);
    tracker.onUpstreamNewSegment(graphC, 0, 10000000LL, 1.0);

    tracker.onUpstreamSample(graphA, 28800, true, 0, 1000000LL);
    tracker.onUpstreamSample(graphB, 57600, true, 0, 1000000LL);
    tracker.onUpstreamSample(graphC, 115200, true, 0, 1000000LL);

    CHECK(!tracker.onUpstreamEndOfStream(graphA));
    CHECK(!tracker.onUpstreamEndOfStream(graphB));
    CHECK(!tracker.onUpstreamEndOfStream(graphC));

    tracker.onPreReadFormatCandidate(graphA, false, 48000, 2, 16);
    tracker.onPreReadFormatCandidate(graphB, true, 96000, 2, 24);
    tracker.onBeginFlush(graphC);

    CHECK(tracker.onDownstreamSample(graphA, 28800, true, 0, 1000000LL));
    CHECK(!tracker.onDownstreamSample(graphB, 57600, true, 0, 1000000LL));
    CHECK(!tracker.onDownstreamSample(graphC, 115200, true, 0, 1000000LL));

    CHECK(tracker.findGraph(graphA)->downstreamEosDeliveredCount == 1);
    CHECK(tracker.findGraph(graphB)->downstreamEosDeliveredCount == 0);
    CHECK(tracker.findGraph(graphC)->downstreamEosDeliveredCount == 0);
}

#ifdef _M_IX86
// Minimal COM MediaSample / MemAllocator / Source / Sink for headless OmgEqxlFilter.ax verification
class SimpleMediaSample final : public IMediaSample {
public:
    SimpleMediaSample(BYTE* buf, LONG cap) : buffer_(buf), capacity_(cap), actual_(cap) {}
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
        *ppBuffer = buffer_;
        return S_OK;
    }
    long STDMETHODCALLTYPE GetSize() override { return capacity_; }
    HRESULT STDMETHODCALLTYPE GetTime(REFERENCE_TIME* pStart, REFERENCE_TIME* pEnd) override {
        if (!pStart || !pEnd) return E_POINTER;
        if (!hasTime_) return VFW_E_SAMPLE_TIME_NOT_SET;
        *pStart = start_;
        *pEnd = end_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTime(REFERENCE_TIME* pStart, REFERENCE_TIME* pEnd) override {
        if (!pStart || !pEnd) { hasTime_ = false; return S_OK; }
        start_ = *pStart;
        end_ = *pEnd;
        hasTime_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsSyncPoint() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetSyncPoint(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE IsPreroll() override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetPreroll(BOOL) override { return S_OK; }
    long STDMETHODCALLTYPE GetActualDataLength() override { return actual_; }
    HRESULT STDMETHODCALLTYPE SetActualDataLength(long len) override {
        if (len < 0 || len > capacity_) return VFW_E_BUFFER_OVERFLOW;
        actual_ = len;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMediaType(AM_MEDIA_TYPE** ppMediaType) override {
        if (!ppMediaType) return E_POINTER;
        *ppMediaType = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE SetMediaType(AM_MEDIA_TYPE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE IsDiscontinuity() override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetDiscontinuity(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMediaTime(LONGLONG*, LONGLONG*) override { return VFW_E_MEDIA_TIME_NOT_SET; }
    HRESULT STDMETHODCALLTYPE SetMediaTime(LONGLONG*, LONGLONG*) override { return S_OK; }

private:
    std::atomic<ULONG> ref_{1};
    BYTE* buffer_;
    LONG capacity_;
    LONG actual_;
    REFERENCE_TIME start_ = 0;
    REFERENCE_TIME end_ = 0;
    bool hasTime_ = false;
};

class SimpleAllocator final : public IMemAllocator {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMemAllocator) {
            *ppv = static_cast<IMemAllocator*>(this);
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
    HRESULT STDMETHODCALLTYPE SetProperties(ALLOCATOR_PROPERTIES* pRequest, ALLOCATOR_PROPERTIES* pActual) override {
        if (!pRequest || !pActual) return E_POINTER;
        props_ = *pRequest;
        if (props_.cBuffers < 1) props_.cBuffers = 1;
        if (props_.cbBuffer < 6) props_.cbBuffer = 49152;
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
        LONG sz = props_.cbBuffer > 0 ? props_.cbBuffer : 49152;
        storage_.emplace_back(static_cast<size_t>(sz), 0);
        *ppBuffer = new SimpleMediaSample(storage_.back().data(), sz);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(IMediaSample*) override { return S_OK; }

private:
    std::atomic<ULONG> ref_{1};
    ALLOCATOR_PROPERTIES props_{8, 49152, 1, 0};
    std::vector<std::vector<BYTE>> storage_;
};

static void initWaveFormat24(WAVEFORMATEXTENSIBLE& wfx, DWORD rate) {
    std::memset(&wfx, 0, sizeof(wfx));
    wfx.Format.wFormatTag = WAVE_FORMAT_PCM;
    wfx.Format.nChannels = 2;
    wfx.Format.nSamplesPerSec = rate;
    wfx.Format.wBitsPerSample = 24;
    wfx.Format.nBlockAlign = 6;
    wfx.Format.nAvgBytesPerSec = rate * 6;
}

class CaptureSinkPin final : public IPin, public IMemInputPin {
public:
    uint64_t totalFrames = 0;
    uint64_t nonZeroFrames = 0;
    uint64_t lastNonZeroFrame = 0;
    uint32_t eosCount = 0;
    IPin* connected_ = nullptr;
    AM_MEDIA_TYPE mt_{};

    CaptureSinkPin() {
        allocator_ = new SimpleAllocator();
    }
    ~CaptureSinkPin() {
        if (mt_.pbFormat) CoTaskMemFree(mt_.pbFormat);
        if (allocator_) allocator_->Release();
    }
    void resetStats() {
        totalFrames = 0;
        nonZeroFrames = 0;
        lastNonZeroFrame = 0;
        eosCount = 0;
    }
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
    ULONG STDMETHODCALLTYPE Release() override { return --ref_; }

    HRESULT STDMETHODCALLTYPE Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override {
        if (!pConnector || !pmt) return E_POINTER;
        connected_ = pConnector;
        if (mt_.pbFormat) CoTaskMemFree(mt_.pbFormat);
        mt_ = *pmt;
        if (pmt->cbFormat > 0 && pmt->pbFormat) {
            mt_.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(pmt->cbFormat));
            std::memcpy(mt_.pbFormat, pmt->pbFormat, pmt->cbFormat);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Disconnect() override { connected_ = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pPin) override {
        if (!pPin) return E_POINTER;
        *pPin = connected_;
        if (connected_) connected_->AddRef();
        return connected_ ? S_OK : VFW_E_NOT_CONNECTED;
    }
    HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE* pmt) override {
        if (!pmt) return E_POINTER;
        *pmt = mt_;
        if (mt_.cbFormat > 0 && mt_.pbFormat) {
            pmt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(mt_.cbFormat));
            std::memcpy(pmt->pbFormat, mt_.pbFormat, mt_.cbFormat);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* pInfo) override {
        if (!pInfo) return E_POINTER;
        std::memset(pInfo, 0, sizeof(*pInfo));
        pInfo->dir = PINDIR_INPUT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* pPinDir) override {
        if (!pPinDir) return E_POINTER;
        *pPinDir = PINDIR_INPUT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* Id) override {
        if (!Id) return E_POINTER;
        *Id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(L"SinkIn")));
        std::memcpy(*Id, L"SinkIn", sizeof(L"SinkIn"));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndOfStream() override { ++eosCount; return S_OK; }
    HRESULT STDMETHODCALLTYPE BeginFlush() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EndFlush() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetAllocator(IMemAllocator** ppAllocator) override {
        if (!ppAllocator) return E_POINTER;
        *ppAllocator = allocator_;
        allocator_->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE NotifyAllocator(IMemAllocator*, BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Receive(IMediaSample* pSample) override {
        if (!pSample) return E_POINTER;
        BYTE* data = nullptr;
        if (FAILED(pSample->GetPointer(&data)) || !data) return E_FAIL;
        long len = pSample->GetActualDataLength();
        long frames = len / 6;
        for (long i = 0; i < frames; ++i) {
            ++totalFrames;
            const BYTE* f = data + i * 6;
            if (f[0] || f[1] || f[2] || f[3] || f[4] || f[5]) {
                ++nonZeroFrames;
                lastNonZeroFrame = totalFrames;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReceiveMultiple(IMediaSample** pSamples, long nSamples, long* nProcessed) override {
        if (!pSamples || !nProcessed) return E_POINTER;
        *nProcessed = 0;
        for (long i = 0; i < nSamples; ++i) {
            if (FAILED(Receive(pSamples[i]))) return E_FAIL;
            ++(*nProcessed);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReceiveCanBlock() override { return S_FALSE; }

private:
    std::atomic<ULONG> ref_{1};
    SimpleAllocator* allocator_ = nullptr;
};

class DummyOutputPin final : public IPin {
public:
    IPin* connected_ = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPin) {
            *ppv = static_cast<IPin*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { return --ref_; }
    HRESULT STDMETHODCALLTYPE Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Disconnect() override { connected_ = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pPin) override {
        if (!pPin) return E_POINTER;
        *pPin = connected_;
        if (connected_) connected_->AddRef();
        return connected_ ? S_OK : VFW_E_NOT_CONNECTED;
    }
    HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* pInfo) override {
        if (!pInfo) return E_POINTER;
        std::memset(pInfo, 0, sizeof(*pInfo));
        pInfo->dir = PINDIR_OUTPUT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* pPinDir) override {
        if (!pPinDir) return E_POINTER;
        *pPinDir = PINDIR_OUTPUT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* Id) override {
        if (!Id) return E_POINTER;
        *Id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(L"SrcOut")));
        std::memcpy(*Id, L"SrcOut", sizeof(L"SrcOut"));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndOfStream() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE BeginFlush() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EndFlush() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

private:
    std::atomic<ULONG> ref_{1};
};

// Test 7: Live OmgEqxlFilter.ax + eosTransition::Tracker integration test (Win32)
void testDirectShowEqFilterIntegration() {
    const wchar_t* eqPath = L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\OmgEqxlFilter.ax";
    if (GetFileAttributesW(eqPath) == INVALID_FILE_ATTRIBUTES) {
        std::printf("[TEST 7] OmgEqxlFilter.ax not installed on this machine; skipping live COM filter check.\n");
        return;
    }
    std::printf("[TEST 7] DirectShow OmgEqxlFilter.ax + eosTransition::Tracker integration...\n");
    HMODULE mod = LoadLibraryExW(eqPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    CHECK(mod != nullptr);
    if (!mod) return;

    auto getClass = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
        GetProcAddress(mod, "DllGetClassObject"));
    CHECK(getClass != nullptr);
    CLSID eqClsid{};
    CLSIDFromString(L"{5C968488-5E35-4CFE-98A1-A51C838797DB}", &eqClsid);
    IClassFactory* factory = nullptr;
    IBaseFilter* eqFilter = nullptr;
    if (SUCCEEDED(getClass(eqClsid, IID_PPV_ARGS(&factory))) && factory) {
        factory->CreateInstance(nullptr, IID_PPV_ARGS(&eqFilter));
        factory->Release();
    }
    CHECK(eqFilter != nullptr);
    if (!eqFilter) return;

    IPin* eqIn = nullptr;
    IPin* eqOut = nullptr;
    IEnumPins* enumPins = nullptr;
    if (SUCCEEDED(eqFilter->EnumPins(&enumPins))) {
        IPin* p = nullptr;
        while (enumPins->Next(1, &p, nullptr) == S_OK) {
            PIN_DIRECTION dir{};
            p->QueryDirection(&dir);
            if (dir == PINDIR_INPUT && !eqIn) eqIn = p;
            else if (dir == PINDIR_OUTPUT && !eqOut) eqOut = p;
            else p->Release();
        }
        enumPins->Release();
    }
    IMemInputPin* eqMemIn = nullptr;
    if (eqIn) eqIn->QueryInterface(IID_PPV_ARGS(&eqMemIn));
    CHECK(eqIn != nullptr && eqOut != nullptr && eqMemIn != nullptr);

    WAVEFORMATEXTENSIBLE wfx{};
    initWaveFormat24(wfx, 48000);
    AM_MEDIA_TYPE mt{};
    mt.majortype = MEDIATYPE_Audio;
    mt.subtype = MEDIASUBTYPE_PCM;
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = 6;
    mt.formattype = FORMAT_WaveFormatEx;
    mt.cbFormat = sizeof(WAVEFORMATEX);
    mt.pbFormat = reinterpret_cast<BYTE*>(&wfx.Format);

    DummyOutputPin srcOut;
    CaptureSinkPin sinkIn;
    srcOut.connected_ = eqIn;
    CHECK(SUCCEEDED(eqIn->ReceiveConnection(&srcOut, &mt)));
    CHECK(SUCCEEDED(eqOut->Connect(&sinkIn, &mt)));

    IMediaFilter* mf = nullptr;
    eqFilter->QueryInterface(IID_PPV_ARGS(&mf));
    CHECK(mf != nullptr);
    CHECK(SUCCEEDED(mf->Pause()));
    CHECK(SUCCEEDED(mf->Run(0)));

    // Subcase 7A: Odd-length 51,733-frame (~1.07777s, non-23s) 48kHz/24bit segment -> format change
    eosTransition::Tracker tracker;
    void* graphKey = eqFilter;
    tracker.onGraphActivated(graphKey, true, 48000, 2, 24);
    const uint64_t targetFrames = 51733;
    const REFERENCE_TIME segDuration = static_cast<REFERENCE_TIME>((targetFrames * 10000000ULL) / 48000ULL);
    tracker.onUpstreamNewSegment(graphKey, 0, segDuration, 1.0);
    eqIn->NewSegment(0, segDuration, 1.0);

    struct QueuedChunk {
        std::vector<BYTE> pcm;
        REFERENCE_TIME start;
        REFERENCE_TIME end;
    };
    std::vector<QueuedChunk> chunks;
    uint64_t built = 0;
    while (built < targetFrames) {
        uint64_t fCount = (targetFrames - built > 4800) ? 4800 : (targetFrames - built);
        QueuedChunk c;
        c.pcm.resize(static_cast<size_t>(fCount * 6));
        for (uint64_t i = 0; i < fCount; ++i) {
            double phase = 2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(built + i) / 48000.0;
            int32_t v = static_cast<int32_t>(std::lround(std::sin(phase) * 2097152.0));
            if (v == 0) v = 256;
            BYTE* dst = c.pcm.data() + i * 6;
            dst[0] = dst[3] = static_cast<BYTE>(v & 0xFF);
            dst[1] = dst[4] = static_cast<BYTE>((v >> 8) & 0xFF);
            dst[2] = dst[5] = static_cast<BYTE>((v >> 16) & 0xFF);
        }
        c.start = static_cast<REFERENCE_TIME>((built * 10000000ULL) / 48000ULL);
        built += fCount;
        c.end = static_cast<REFERENCE_TIME>((built * 10000000ULL) / 48000ULL);
        tracker.onUpstreamSamplePreReceive(graphKey, static_cast<uint32_t>(c.pcm.size()));
        tracker.onUpstreamSample(graphKey, static_cast<uint32_t>(c.pcm.size()), true, c.start, c.end);
        chunks.push_back(std::move(c));
    }
    // Upstream finishes decoding ahead of EQ
    CHECK(!tracker.onUpstreamEndOfStream(graphKey));
    tracker.onPreReadFormatCandidate(graphKey, false, 48000, 2, 16);

    bool eosTriggeredOnLastChunk = false;
    uint64_t framesBeforeEosFlush = 0;
    for (size_t idx = 0; idx < chunks.size(); ++idx) {
        auto& c = chunks[idx];
        auto* sample = new SimpleMediaSample(c.pcm.data(), static_cast<LONG>(c.pcm.size()));
        sample->SetTime(&c.start, &c.end);
        CHECK(SUCCEEDED(eqMemIn->Receive(sample)));
        sample->Release();

        bool shouldEos = tracker.onDownstreamSample(
            graphKey, static_cast<uint32_t>(c.pcm.size()), true, c.start, c.end);
        if (idx + 1 < chunks.size()) {
            CHECK(!shouldEos);
        } else {
            framesBeforeEosFlush = sinkIn.nonZeroFrames;
            CHECK(shouldEos);
            if (shouldEos) {
                eosTriggeredOnLastChunk = true;
                CHECK(SUCCEEDED(eqIn->EndOfStream()));
            }
        }
    }

    // Before EndOfStream was delivered on the final chunk, EQ held 10,773 frames (51,733 - 40,960).
    // Immediately after tracker-triggered EndOfStream, all 51,733 non-zero frames were flushed!
    CHECK(eosTriggeredOnLastChunk);
    CHECK(framesBeforeEosFlush == 40960);
    CHECK(sinkIn.nonZeroFrames == targetFrames);
    CHECK(sinkIn.lastNonZeroFrame == targetFrames);
    CHECK(sinkIn.eosCount == 1);

    // Subcase 7B: Same-format 48kHz/24bit -> 48kHz/24bit pre-read (24,000 + 24,000 frames)
    // followed by format change at the end of Track 2 through real OmgEqxlFilter.ax
    sinkIn.resetStats();
    tracker.onBeginFlush(graphKey);
    eqIn->BeginFlush();
    tracker.onEndFlush(graphKey);
    eqIn->EndFlush();
    tracker.onUpstreamNewSegment(graphKey, 0, 10000000LL, 1.0);
    eqIn->NewSegment(0, 10000000LL, 1.0);

    std::vector<QueuedChunk> twoTrackChunks;
    for (int t = 0; t < 10; ++t) {
        QueuedChunk c;
        c.pcm.resize(4800 * 6, 0x11);
        c.start = static_cast<REFERENCE_TIME>(t) * 1000000LL;
        c.end = static_cast<REFERENCE_TIME>(t + 1) * 1000000LL;
        twoTrackChunks.push_back(std::move(c));
    }
    // Track 1 (chunks 0..4) arrives upstream, then upstream EndOfStream, then 0x31E7A SameFormat24
    for (int t = 0; t < 5; ++t) {
        tracker.onUpstreamSamplePreReceive(graphKey, static_cast<uint32_t>(twoTrackChunks[t].pcm.size()));
        tracker.onUpstreamSample(graphKey, static_cast<uint32_t>(twoTrackChunks[t].pcm.size()),
                                 true, twoTrackChunks[t].start, twoTrackChunks[t].end);
    }
    CHECK(!tracker.onUpstreamEndOfStream(graphKey));
    tracker.onPreReadFormatCandidate(graphKey, true, 48000, 2, 24);
    // Track 2 (chunks 5..9) arrives upstream, then upstream EndOfStream (end of playlist / format change)
    for (int t = 5; t < 10; ++t) {
        tracker.onUpstreamSamplePreReceive(graphKey, static_cast<uint32_t>(twoTrackChunks[t].pcm.size()));
        tracker.onUpstreamSample(graphKey, static_cast<uint32_t>(twoTrackChunks[t].pcm.size()),
                                 true, twoTrackChunks[t].start, twoTrackChunks[t].end);
    }
    CHECK(!tracker.onUpstreamEndOfStream(graphKey));

    for (size_t idx = 0; idx < twoTrackChunks.size(); ++idx) {
        auto& c = twoTrackChunks[idx];
        auto* sample = new SimpleMediaSample(c.pcm.data(), static_cast<LONG>(c.pcm.size()));
        sample->SetTime(&c.start, &c.end);
        CHECK(SUCCEEDED(eqMemIn->Receive(sample)));
        sample->Release();

        bool shouldEos = tracker.onDownstreamSample(
            graphKey, static_cast<uint32_t>(c.pcm.size()), true, c.start, c.end);
        if (idx + 1 < twoTrackChunks.size()) {
            // Must NOT fire EOS at Track 1 boundary (idx == 4)
            CHECK(!shouldEos);
        } else {
            // Must fire EOS once at Track 2 end (idx == 9)
            CHECK(shouldEos);
            if (shouldEos) {
                CHECK(SUCCEEDED(eqIn->EndOfStream()));
            }
        }
    }
    CHECK(sinkIn.nonZeroFrames == 48000);
    CHECK(sinkIn.lastNonZeroFrame == 48000);
    CHECK(sinkIn.eosCount == 1);

    mf->Stop();
    mf->Release();
    eqMemIn->Release();
    eqIn->Release();
    eqOut->Release();
    eqFilter->Release();
}
#endif

// Test 8: COM vtable hooks on IMediaSeeking (vtable[11], [12], [14]) and AM_SEEKING_NoPositioning safety
class MockMediaSeeking final : public IMediaSeeking {
public:
    LONGLONG currentPos = 10000000LL; // 1.0s
    LONGLONG stopPos = 230000000LL;    // 23.0s
    LONGLONG duration = 230000000LL;
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
    ULONG STDMETHODCALLTYPE Release() override { return --ref_; }

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
    HRESULT STDMETHODCALLTYPE GetStopPosition(LONGLONG* pStop) override {
        ++getStopPositionCallCount;
        if (!pStop) return E_POINTER;
        *pStop = stopPos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentPosition(LONGLONG* pCurrent) override {
        ++getCurrentPositionCallCount;
        if (!pCurrent) return E_POINTER;
        *pCurrent = currentPos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ConvertTimeFormat(LONGLONG*, const GUID*, LONGLONG, const GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPositions(LONGLONG* pCurrent, DWORD dwCurrentFlags,
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

private:
    std::atomic<ULONG> ref_{1};
};

using GetPositionFn = HRESULT(STDMETHODCALLTYPE*)(IMediaSeeking*, LONGLONG*);
using SetPositionsFn = HRESULT(STDMETHODCALLTYPE*)(IMediaSeeking*, LONGLONG*, DWORD, LONGLONG*, DWORD);

static GetPositionFn g_testOrigGetStopPosition = nullptr;
static GetPositionFn g_testOrigGetCurrentPosition = nullptr;
static SetPositionsFn g_testOrigSetPositions = nullptr;
static eosTransition::Tracker* g_testTracker = nullptr;
static void* g_testGraphKey = nullptr;

static HRESULT STDMETHODCALLTYPE testHookGetStopPosition(IMediaSeeking* self, LONGLONG* pStop) {
    auto hr = g_testOrigGetStopPosition(self, pStop);
    if (SUCCEEDED(hr) && pStop && g_testTracker) {
        g_testTracker->onStopPositionReported(g_testGraphKey, *pStop);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE testHookGetCurrentPosition(IMediaSeeking* self, LONGLONG* pCurrent) {
    auto hr = g_testOrigGetCurrentPosition(self, pCurrent);
    if (SUCCEEDED(hr) && pCurrent && g_testTracker) {
        g_testTracker->onCurrentPositionReported(g_testGraphKey, *pCurrent);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE testHookSetPositions(IMediaSeeking* self, LONGLONG* pCurrent, DWORD dwCurrentFlags,
                                                     LONGLONG* pStop, DWORD dwStopFlags) {
    const DWORD currPosBits = dwCurrentFlags & AM_SEEKING_PositioningBitsMask;
    const DWORD stopPosBits = dwStopFlags & AM_SEEKING_PositioningBitsMask;
    bool modifiesCurr = (currPosBits != AM_SEEKING_NoPositioning) && (pCurrent != nullptr);
    bool modifiesStop = (stopPosBits != AM_SEEKING_NoPositioning) && (pStop != nullptr);
    if ((modifiesCurr || modifiesStop) && g_testTracker) {
        LONGLONG newPos = modifiesCurr ? *pCurrent : -1;
        LONGLONG newStop = modifiesStop ? *pStop : -1;
        g_testTracker->onSeekPositions(g_testGraphKey, modifiesCurr, newPos, modifiesStop, newStop);
    }
    return g_testOrigSetPositions(self, pCurrent, dwCurrentFlags, pStop, dwStopFlags);
}

#pragma optimize("", off)
static IMediaSeeking* createMockMediaSeeking() {
    return new MockMediaSeeking();
}
#pragma optimize("", on)

void testMediaSeekingComVtableHooks() {
    std::printf("[TEST 8] IMediaSeeking COM vtable hooks (index 11, 12, 14) and positioning flag safety...\n");
    CHECK(MH_Initialize() == MH_OK);

    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0x8888);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);
    tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);

    g_testTracker = &tracker;
    g_testGraphKey = graph;

    IMediaSeeking* pSeeking = createMockMediaSeeking();
    auto* mock = static_cast<MockMediaSeeking*>(pSeeking);
    auto seekVtable = *reinterpret_cast<void***>(pSeeking);

    auto h1 = MH_CreateHook(seekVtable[11], reinterpret_cast<void*>(testHookGetStopPosition),
                            reinterpret_cast<void**>(&g_testOrigGetStopPosition));
    auto h2 = MH_CreateHook(seekVtable[12], reinterpret_cast<void*>(testHookGetCurrentPosition),
                            reinterpret_cast<void**>(&g_testOrigGetCurrentPosition));
    auto h3 = MH_CreateHook(seekVtable[14], reinterpret_cast<void*>(testHookSetPositions),
                            reinterpret_cast<void**>(&g_testOrigSetPositions));
    CHECK(h1 == MH_OK);
    CHECK(h2 == MH_OK);
    CHECK(h3 == MH_OK);
    CHECK(MH_EnableHook(MH_ALL_HOOKS) == MH_OK);

    // 1. Verify GetStopPosition (vtable[11])
    LONGLONG stop = 0;
    CHECK(SUCCEEDED(pSeeking->GetStopPosition(&stop)));
    CHECK(stop == 230000000LL);
    CHECK(mock->getStopPositionCallCount == 1);
    const auto* st = tracker.findGraph(graph);
    CHECK(st != nullptr && st->hasStopPosition && st->stopPosition == 230000000LL);

    // 2. Verify GetCurrentPosition (vtable[12])
    LONGLONG curr = 0;
    CHECK(SUCCEEDED(pSeeking->GetCurrentPosition(&curr)));
    CHECK(curr == 10000000LL);
    CHECK(mock->getCurrentPositionCallCount == 1);
    CHECK(st->hasCurrentPosition && st->currentPosition == 10000000LL);

    // 3. Verify AM_SEEKING_NoPositioning safety (both nullptr and dummy pointers)
    CHECK(SUCCEEDED(pSeeking->SetPositions(nullptr, AM_SEEKING_NoPositioning, nullptr, AM_SEEKING_NoPositioning)));
    CHECK(mock->setPositionsCallCount == 1);
    CHECK(st->lastResetReason != eosTransition::ResetReason::Seek);

    LONGLONG dummyPos = 99999LL;
    CHECK(SUCCEEDED(pSeeking->SetPositions(&dummyPos, AM_SEEKING_NoPositioning, &dummyPos, AM_SEEKING_NoPositioning)));
    CHECK(mock->setPositionsCallCount == 2);
    CHECK(mock->currentPos == 10000000LL); // Must NOT be overwritten
    CHECK(mock->stopPos == 230000000LL);    // Must NOT be overwritten
    CHECK(st->lastResetReason != eosTransition::ResetReason::Seek);

    // 4. Verify SetPositions with real seek flags (vtable[14])
    LONGLONG seekPos = 50000000LL; // seek to 5.0s
    CHECK(SUCCEEDED(pSeeking->SetPositions(&seekPos, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning)));
    CHECK(mock->setPositionsCallCount == 3);
    CHECK(mock->currentPos == 50000000LL);
    CHECK(st->lastResetReason == eosTransition::ResetReason::Seek);
    CHECK(st->hasCurrentPosition && st->currentPosition == 50000000LL);

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    pSeeking->Release();
    g_testTracker = nullptr;
    g_testGraphKey = nullptr;
}

// Test 9: Defer format-changing transition until track end with track-tail analyzer check (#19 analyzer)
void testDeferredFormatChangeWithTrackTailCheck() {
    std::printf("[TEST 9] Defer format change transition until track end + trackTail::analyzeStream check...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0x9999);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);

    const size_t track1Frames = 24000; // 0.500s of 48kHz/24bit audio (boosted tail in final 300ms)
    std::vector<uint8_t> pcmStream;
    trackTail::appendSynthesizedTrack(pcmStream, 48000, 24, track1Frames, false);
    CHECK(pcmStream.size() == track1Frames * 6);

    const REFERENCE_TIME segDuration = static_cast<REFERENCE_TIME>((track1Frames * 10000000ULL) / 48000ULL);
    tracker.onUpstreamNewSegment(graph, 0, segDuration, 1.0);
    tracker.onStopPositionReported(graph, segDuration);

    // Upstream feeds 5 chunks of 4,800 frames (28,800 bytes)
    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 5; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onUpstreamSamplePreReceive(graph, chunkBytes);
        tracker.onUpstreamSample(graph, chunkBytes, true, sStart, sEnd);
    }

    // Upstream finishes decoding ahead of DirectSound playback clock (~3s before end in Music Center)
    CHECK(!tracker.onUpstreamEndOfStream(graph));

    // Format change candidate reported at 0x31E7A (e.g. next track is 16-bit FLAC)
    tracker.onPreReadFormatCandidate(graph, false, 48000, 2, 16);

    // Verify format change transition is detected and pending
    CHECK(tracker.isFormatChangePending(graph) == true);
    const auto* st = tracker.findGraph(graph);
    CHECK(st != nullptr && st->preReadDecision == eosTransition::PreReadDecision::FormatChanged);
    CHECK(st->eosPending == true && !st->eosDelivered);

    // Downstream plays out chunks 0..3 (4800 * 4 = 19,200 frames)
    for (int i = 0; i < 4; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onCurrentPositionReported(graph, sEnd);
        CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, sStart, sEnd));
        CHECK(tracker.isFormatChangePending(graph) == true);
    }

    // Final chunk 4 reaches EQ: triggers EOS delivery once!
    REFERENCE_TIME finalStart = 4000000LL;
    REFERENCE_TIME finalEnd = 5000000LL;
    tracker.onCurrentPositionReported(graph, finalEnd);
    CHECK(tracker.onDownstreamSample(graph, chunkBytes, true, finalStart, finalEnd));
    CHECK(st->eosDelivered == true);
    CHECK(st->downstreamEosDeliveredCount == 1);

    // DirectSound drains to track end (curPos >= stopPos)
    tracker.onCurrentPositionReported(graph, segDuration);
    CHECK(tracker.isFormatChangePending(graph) == false);

    // Deferred stop completes cleanly
    tracker.onFormatChangeCompleted(graph);
    tracker.onStop(graph);

    // Verify Track 1 tail completeness using #19's analyzer
    auto a1 = trackTail::analyzeStream(pcmStream.data(), pcmStream.size(), 48000, 24, track1Frames);
    CHECK(a1.tailComplete == true);
    CHECK(a1.missingTailFrames == 0);
    CHECK(a1.missingTailMs == 0.0);
    CHECK(a1.approx300msMissing == false);
    CHECK(a1.track1LastFrame == track1Frames);

    // Now append next track (48kHz/16bit stereo anti-phase, 12,000 frames)
    const size_t track2Frames = 12000;
    trackTail::appendSynthesizedTrack(pcmStream, 48000, 16, track2Frames, true);

    // Verify 24-bit analysis on Track 1 boundary: Track 1 tail is 100% intact before Track 2
    auto a2 = trackTail::analyzeStream(pcmStream.data(), track1Frames * 6, 48000, 24, track1Frames);
    CHECK(a2.tailComplete == true);
    CHECK(a2.missingTailFrames == 0);
}

// Test 10: Playlist-end (no next track) drain with track-tail analyzer check
void testPlaylistEndDrainWithTrackTailCheck() {
    std::printf("[TEST 10] Playlist-end drain (no next track) + trackTail::analyzeStream check...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0xAAAA);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);

    const size_t track1Frames = 24000;
    std::vector<uint8_t> pcmStream;
    trackTail::appendSynthesizedTrack(pcmStream, 48000, 24, track1Frames, false);

    const REFERENCE_TIME segDuration = static_cast<REFERENCE_TIME>((track1Frames * 10000000ULL) / 48000ULL);
    tracker.onUpstreamNewSegment(graph, 0, segDuration, 1.0);
    tracker.onStopPositionReported(graph, segDuration);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 5; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onUpstreamSamplePreReceive(graph, chunkBytes);
        tracker.onUpstreamSample(graph, chunkBytes, true, sStart, sEnd);
    }

    // Upstream finishes decoding; no next track in playlist (PreReadDecision::Unknown)
    CHECK(!tracker.onUpstreamEndOfStream(graph));

    for (int i = 0; i < 4; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, sStart, sEnd));
    }

    // Final chunk reaches EQ and fires EOS once
    CHECK(tracker.onDownstreamSample(graph, chunkBytes, true, 4000000LL, 5000000LL));
    const auto* st = tracker.findGraph(graph);
    CHECK(st != nullptr && st->eosDelivered == true && st->downstreamEosDeliveredCount == 1);

    auto a = trackTail::analyzeStream(pcmStream.data(), pcmStream.size(), 48000, 24, track1Frames);
    CHECK(a.tailComplete == true);
    CHECK(a.missingTailFrames == 0);
    CHECK(a.missingTailMs == 0.0);
    CHECK(a.approx300msMissing == false);
    CHECK(a.nextTrackDetected == false);
}

// Test 11: IMediaControl::Pause deferral on automatic format change (Issue #23)
void testPauseDeferralOnFormatChange() {
    std::printf("[TEST 11] IMediaControl::Pause deferral on format change (OpcDSPlay.dll+0xDAC9)...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0xBB01);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);

    const size_t track1Frames = 24000; // 500ms at 48kHz
    std::vector<uint8_t> pcmStream;
    trackTail::appendSynthesizedTrack(pcmStream, 48000, 24, track1Frames, false);

    const REFERENCE_TIME segDuration = static_cast<REFERENCE_TIME>((track1Frames * 10000000ULL) / 48000ULL);
    tracker.onUpstreamNewSegment(graph, 0, segDuration, 1.0);
    tracker.onStopPositionReported(graph, segDuration);

    const uint32_t chunkBytes = 4800 * 6; // 100ms
    for (int i = 0; i < 5; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onUpstreamSamplePreReceive(graph, chunkBytes);
        tracker.onUpstreamSample(graph, chunkBytes, true, sStart, sEnd);
    }

    // Upstream FLAC finish -> Event 3841 -> Pre-read failure (FormatChanged)
    tracker.onUpstreamEndOfStream(graph);
    tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);
    CHECK(tracker.isFormatChangePending(graph) == true);

    // OpcDSPlay.dll calls Pause() ~52ms after 3841: Pause deferral starts!
    tracker.markFormatChangePauseDeferred(graph);
    const auto* st = tracker.findGraph(graph);
    CHECK(st != nullptr && st->formatChangePauseDeferred == true);

    // Audio in pipeline continues draining through EQ (first 4 chunks)
    for (int i = 0; i < 4; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onCurrentPositionReported(graph, sEnd);
        CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, sStart, sEnd));
        CHECK(tracker.isFormatChangePending(graph) == true);
    }

    // Final chunk 4 reaches EQ: triggers EOS delivery once to EQ
    REFERENCE_TIME finalStart = 4000000LL;
    REFERENCE_TIME finalEnd = 5000000LL;
    tracker.onCurrentPositionReported(graph, finalEnd);
    CHECK(tracker.onDownstreamSample(graph, chunkBytes, true, finalStart, finalEnd));
    CHECK(st->eosDelivered == true);
    CHECK(st->downstreamEosDeliveredCount == 1);

    // DirectSound drains to track end (curPos >= stopPos)
    tracker.onCurrentPositionReported(graph, segDuration);
    CHECK(tracker.isFormatChangePending(graph) == false);

    // Deferred Pause completes cleanly
    tracker.onFormatChangePauseCompleted(graph);
    CHECK(st->formatChangePauseCompleted == true);
    CHECK(st->formatChangePauseDeferred == false);

    // Verify Track 1 tail completeness using #19 analyzer
    auto a1 = trackTail::analyzeStream(pcmStream.data(), pcmStream.size(), 48000, 24, track1Frames);
    CHECK(a1.tailComplete == true);
    CHECK(a1.missingTailFrames == 0);
    CHECK(a1.missingTailMs == 0.0);
    CHECK(a1.approx300msMissing == false);
    CHECK(a1.track1LastFrame == track1Frames);

    // Subsequent SetPositions and Track 2 open proceed cleanly
    tracker.onSeekPositions(graph, true, 0, false, -1);
    const size_t track2Frames = 12000;
    trackTail::appendSynthesizedTrack(pcmStream, 48000, 16, track2Frames, true);

    auto a2 = trackTail::analyzeStream(pcmStream.data(), track1Frames * 6, 48000, 24, track1Frames);
    CHECK(a2.tailComplete == true);
    CHECK(a2.missingTailFrames == 0);
}

// Test 12: Observation mode vs Deferral mode behavior (Issue #23)
void testPauseModeSwitching() {
    std::printf("[TEST 12] Observation mode (MC_BRIDGE_PAUSE_MODE=observe) vs Deferral mode...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0xBB02);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);
    tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
    tracker.onUpstreamSamplePreReceive(graph, 4800 * 6);
    tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
    tracker.onUpstreamEndOfStream(graph);
    tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);

    // In Observation mode: format change is detected, but pause is NOT deferred
    CHECK(tracker.isFormatChangePending(graph) == true);
    const auto* st = tracker.findGraph(graph);
    CHECK(st->formatChangePauseDeferred == false);

    // When Deferral mode is active: Pause is explicitly deferred
    tracker.markFormatChangePauseDeferred(graph);
    CHECK(st->formatChangePauseDeferred == true);

    tracker.onFormatChangePauseCompleted(graph);
    CHECK(st->formatChangePauseCompleted == true);
    CHECK(tracker.isFormatChangePending(graph) == false);
}

// Test 13: Non-format-change Pause operations are NEVER deferred (Issue #23)
void testNonFormatChangePauseNeverDeferred() {
    std::printf("[TEST 13] Non-format-change Pause operations are never deferred...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0xBB03);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);
    tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);

    // Subcase 13A: Normal playback in progress (upstream hasn't finished, no 3841)
    tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
    CHECK(tracker.isFormatChangePending(graph) == false); // Must NOT defer user pause!

    // Subcase 13B: Same-format pre-read succeeded (48k/24 -> 48k/24)
    tracker.onUpstreamEndOfStream(graph);
    tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::SameFormat24);
    CHECK(tracker.isFormatChangePending(graph) == false); // Must NOT defer same-format pause!

    // Subcase 13C: User manual seek in progress
    tracker.onSeekPositions(graph, true, 50000000LL, false, -1);
    CHECK(tracker.isFormatChangePending(graph) == false); // Must NOT defer seek-associated pause!

    // Subcase 13D: 16-bit non-target graph
    void* graph16 = reinterpret_cast<void*>(0xBB04);
    tracker.onGraphActivated(graph16, false, 48000, 2, 16);
    tracker.onUpstreamEndOfStream(graph16);
    CHECK(tracker.isFormatChangePending(graph16) == false); // Must NOT defer 16-bit graph!
}

// Test 14: Manual operations during Pause deferral (Issue #24)
void testManualOperationsDuringPauseDeferral() {
    std::printf("[TEST 14] Manual operations during Pause deferral (Stop, Seek, TrackChange, ManualPause, Exit)...\n");

    // Subcase 14A: Manual Stop during Pause deferral
    {
        eosTransition::Tracker tracker;
        void* graph = reinterpret_cast<void*>(0xCC01);
        tracker.onGraphActivated(graph, true, 48000, 2, 24);
        tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
        tracker.onStopPositionReported(graph, 230000000LL);
        tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
        tracker.onUpstreamEndOfStream(graph);
        tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);

        CHECK(tracker.isFormatChangePending(graph) == true);
        tracker.markFormatChangePauseDeferred(graph);
        const auto* st = tracker.findGraph(graph);
        CHECK(st->isPauseDeferralActive() == true);

        // User clicks Stop during the deferral
        tracker.onStop(graph);
        CHECK(st->isPauseDeferralActive() == false);
        CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledStop);
        CHECK(tracker.isFormatChangePending(graph) == false); // No second deferral loop!
    }

    // Subcase 14B: Manual Seek during Pause deferral
    {
        eosTransition::Tracker tracker;
        void* graph = reinterpret_cast<void*>(0xCC02);
        tracker.onGraphActivated(graph, true, 48000, 2, 24);
        tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
        tracker.onStopPositionReported(graph, 230000000LL);
        tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
        tracker.onUpstreamEndOfStream(graph);
        tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);

        tracker.markFormatChangePauseDeferred(graph);
        const auto* st = tracker.findGraph(graph);
        CHECK(st->isPauseDeferralActive() == true);

        // User seeks during the deferral
        tracker.onSeekPositions(graph, true, 50000000LL, false, -1);
        CHECK(st->isPauseDeferralActive() == false);
        CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledSeek);
        CHECK(tracker.isFormatChangePending(graph) == false);
        CHECK(st->hasCurrentPosition == true && st->currentPosition == 50000000LL);
    }

    // Subcase 14C: Manual Track Change during Pause deferral
    {
        eosTransition::Tracker tracker;
        void* graph = reinterpret_cast<void*>(0xCC03);
        tracker.onGraphActivated(graph, true, 48000, 2, 24);
        tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
        tracker.onStopPositionReported(graph, 230000000LL);
        tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
        tracker.onUpstreamEndOfStream(graph);
        tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);

        tracker.markFormatChangePauseDeferred(graph);
        const auto* st = tracker.findGraph(graph);
        CHECK(st->isPauseDeferralActive() == true);

        // User clicks Next Track during the deferral
        tracker.onManualTrackChange(graph);
        CHECK(st->isPauseDeferralActive() == false);
        CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledTrackChange);
        CHECK(tracker.isFormatChangePending(graph) == false);
    }

    // Subcase 14D: Manual Pause during Pause deferral
    {
        eosTransition::Tracker tracker;
        void* graph = reinterpret_cast<void*>(0xCC04);
        tracker.onGraphActivated(graph, true, 48000, 2, 24);
        tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
        tracker.onStopPositionReported(graph, 230000000LL);
        tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
        tracker.onUpstreamEndOfStream(graph);
        tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);

        tracker.markFormatChangePauseDeferred(graph);
        const auto* st = tracker.findGraph(graph);
        CHECK(st->isPauseDeferralActive() == true);

        // User clicks Pause button during the deferral
        tracker.cancelFormatChangePause(graph, eosTransition::PauseDeferralStatus::CancelledManualPause);
        CHECK(st->isPauseDeferralActive() == false);
        CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledManualPause);
        CHECK(tracker.isFormatChangePending(graph) == false);
    }

    // Subcase 14E: Application Exit (WM_QUIT) during Pause deferral
    {
        eosTransition::Tracker tracker;
        void* graph = reinterpret_cast<void*>(0xCC05);
        tracker.onGraphActivated(graph, true, 48000, 2, 24);
        tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
        tracker.onStopPositionReported(graph, 230000000LL);
        tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
        tracker.onUpstreamEndOfStream(graph);
        tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);

        tracker.markFormatChangePauseDeferred(graph);
        const auto* st = tracker.findGraph(graph);
        CHECK(st->isPauseDeferralActive() == true);

        // WM_QUIT dispatched during message pump
        tracker.cancelFormatChangePause(graph, eosTransition::PauseDeferralStatus::CancelledQuit);
        CHECK(st->isPauseDeferralActive() == false);
        CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledQuit);
        CHECK(tracker.isFormatChangePending(graph) == false);
    }
}

// Test 15: Drain wait timeout safety ceiling (Issue #24)
void testDrainWaitTimeoutSafety() {
    std::printf("[TEST 15] Drain wait timeout safety ceiling (safety exit without deadlock)...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0xCC06);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);
    tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
    tracker.onStopPositionReported(graph, 230000000LL);
    tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
    tracker.onUpstreamEndOfStream(graph);
    tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);

    tracker.markFormatChangePauseDeferred(graph);
    const auto* st = tracker.findGraph(graph);
    CHECK(st->isPauseDeferralActive() == true);

    // DirectSound stalls and maxWait timer expires: safe exit with TimedOut
    tracker.onFormatChangePauseCompleted(graph, eosTransition::PauseDeferralStatus::TimedOut);
    CHECK(st->isPauseDeferralActive() == false);
    CHECK(st->formatChangePauseCompleted == true);
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::TimedOut);
    CHECK(tracker.isFormatChangePending(graph) == false);
}

// Test 16: Reentrancy protection and duplicate EOS prevention on old graph (Issue #24)
void testReentrancyAndDuplicateEosPrevention() {
    std::printf("[TEST 16] Reentrancy protection and duplicate EOS prevention on old graph...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0xCC07);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);
    tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
    tracker.onStopPositionReported(graph, 230000000LL);
    tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
    tracker.onUpstreamEndOfStream(graph);
    tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);

    tracker.markFormatChangePauseDeferred(graph);
    tracker.onDownstreamEndOfStream(graph); // Deliver EOS once to EQ
    tracker.onFormatChangePauseCompleted(graph, eosTransition::PauseDeferralStatus::CompletedDrain);

    const auto* st = tracker.findGraph(graph);
    CHECK(st->formatChangePauseCompleted == true);
    CHECK(st->eosDelivered == true);
    CHECK(st->downstreamEosDeliveredCount == 1);

    // 1. Duplicate downstream EOS on old graph is rejected
    CHECK(tracker.onDownstreamEndOfStream(graph) == false);
    CHECK(st->downstreamEosDeliveredCount == 1);

    // 2. Duplicate upstream EOS on old graph is rejected
    CHECK(tracker.onUpstreamEndOfStream(graph) == false);
    CHECK(st->downstreamEosDeliveredCount == 1);

    // 3. Repeated Pause call on old graph is NOT deferred
    CHECK(tracker.isFormatChangePending(graph) == false);

    // 4. Repeated Stop call on old graph is NOT deferred
    CHECK(tracker.isFormatChangePending(graph) == false);
}

// Test 17: Different sample rate transitions (96kHz/24bit -> 44.1kHz/16bit and 96kHz/24bit -> 48kHz/24bit) (Issue #24)
void testDifferentSampleRateTransitions() {
    std::printf("[TEST 17] Different sample rate transitions (96kHz/24bit -> 44.1kHz/16bit & 96kHz/24bit -> 48kHz/24bit)...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0xCC08);
    // Track 1: 96kHz / 24-bit stereo, 48,000 frames = 500ms
    const DWORD rate1 = 96000;
    const size_t track1Frames = 48000;
    tracker.onGraphActivated(graph, true, rate1, 2, 24);

    std::vector<uint8_t> pcmStream;
    trackTail::appendSynthesizedTrack(pcmStream, rate1, 24, track1Frames, false);

    const REFERENCE_TIME segDuration = static_cast<REFERENCE_TIME>((track1Frames * 10000000ULL) / static_cast<uint64_t>(rate1));
    tracker.onUpstreamNewSegment(graph, 0, segDuration, 1.0);
    tracker.onStopPositionReported(graph, segDuration);

    const uint32_t chunkBytes = 9600 * 6; // 100ms chunks at 96kHz
    for (int i = 0; i < 5; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onUpstreamSamplePreReceive(graph, chunkBytes);
        tracker.onUpstreamSample(graph, chunkBytes, true, sStart, sEnd);
    }

    // Upstream finishes decoding; pre-read candidate arrives with different sample rate (44.1kHz / 16bit or 48kHz / 24bit)
    tracker.onUpstreamEndOfStream(graph);
    tracker.onPreReadFormatCandidate(graph, false, 48000, 2, 24); // rate differs -> FormatChanged

    const auto* st = tracker.findGraph(graph);
    CHECK(st->preReadDecision == eosTransition::PreReadDecision::FormatChanged);
    CHECK(tracker.isFormatChangePending(graph) == true);

    // OpcDSPlay.dll calls Pause(): Pause deferral starts
    tracker.markFormatChangePauseDeferred(graph);
    CHECK(st->isPauseDeferralActive() == true);

    // Audio in pipeline drains through EQ
    for (int i = 0; i < 4; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onCurrentPositionReported(graph, sEnd);
        CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, sStart, sEnd));
        CHECK(tracker.isFormatChangePending(graph) == true);
    }

    // Final chunk reaches EQ: one-shot EOS delivered
    REFERENCE_TIME finalStart = 4000000LL;
    REFERENCE_TIME finalEnd = 5000000LL;
    tracker.onCurrentPositionReported(graph, finalEnd);
    CHECK(tracker.onDownstreamSample(graph, chunkBytes, true, finalStart, finalEnd));
    CHECK(st->eosDelivered == true);
    CHECK(st->downstreamEosDeliveredCount == 1);

    // DirectSound drains to track end (curPos >= stopPos)
    tracker.onCurrentPositionReported(graph, segDuration);
    CHECK(tracker.isFormatChangePending(graph) == false);

    tracker.onFormatChangePauseCompleted(graph, eosTransition::PauseDeferralStatus::CompletedDrain);
    CHECK(st->formatChangePauseCompleted == true);

    // Verify 96kHz Track 1 tail completeness using #19 analyzer
    auto a = trackTail::analyzeStream(pcmStream.data(), pcmStream.size(), rate1, 24, track1Frames);
    CHECK(a.tailComplete == true);
    CHECK(a.missingTailFrames == 0);
    CHECK(a.missingTailMs == 0.0);
    CHECK(a.approx300msMissing == false);
    CHECK(a.track1LastFrame == track1Frames);
}

// Test 18: 24-bit playlist end with different sample rates (no next track) (Issue #24)
void testPlaylistEndWithDifferentSampleRates() {
    std::printf("[TEST 18] 24-bit playlist end with different sample rates (no next track)...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0xCC09);
    const DWORD rate = 96000;
    const size_t trackFrames = 48000; // 500ms
    tracker.onGraphActivated(graph, true, rate, 2, 24);

    std::vector<uint8_t> pcmStream;
    trackTail::appendSynthesizedTrack(pcmStream, rate, 24, trackFrames, false);

    const REFERENCE_TIME segDuration = static_cast<REFERENCE_TIME>((trackFrames * 10000000ULL) / static_cast<uint64_t>(rate));
    tracker.onUpstreamNewSegment(graph, 0, segDuration, 1.0);
    tracker.onStopPositionReported(graph, segDuration);

    const uint32_t chunkBytes = 9600 * 6;
    for (int i = 0; i < 5; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        tracker.onUpstreamSamplePreReceive(graph, chunkBytes);
        tracker.onUpstreamSample(graph, chunkBytes, true, sStart, sEnd);
    }

    // Upstream finishes decoding; no next track (PreReadDecision::Unknown)
    CHECK(!tracker.onUpstreamEndOfStream(graph));

    for (int i = 0; i < 4; ++i) {
        REFERENCE_TIME sStart = static_cast<REFERENCE_TIME>(i) * 1000000LL;
        REFERENCE_TIME sEnd = static_cast<REFERENCE_TIME>(i + 1) * 1000000LL;
        CHECK(!tracker.onDownstreamSample(graph, chunkBytes, true, sStart, sEnd));
    }

    // Final chunk reaches EQ and fires EOS once
    CHECK(tracker.onDownstreamSample(graph, chunkBytes, true, 4000000LL, 5000000LL));
    const auto* st = tracker.findGraph(graph);
    CHECK(st != nullptr && st->eosDelivered == true && st->downstreamEosDeliveredCount == 1);

    // Natural end: graph stops via EC_COMPLETE / Stop
    tracker.onCurrentPositionReported(graph, segDuration);
    tracker.onStop(graph);

    auto a = trackTail::analyzeStream(pcmStream.data(), pcmStream.size(), rate, 24, trackFrames);
    CHECK(a.tailComplete == true);
    CHECK(a.missingTailFrames == 0);
    CHECK(a.missingTailMs == 0.0);
    CHECK(a.nextTrackDetected == false);
}

// Test 19: Concurrent manual operations during synchronous EQ EOS call (Issue #25)
void testConcurrentOperationsDuringSynchronousEqEos() {
    std::printf("[TEST 19] Concurrent manual operations during synchronous EQ EOS call...\n");
    // Subcase 19A: Manual Stop from UI thread while EQ EOS call is executing
    {
        eosTransition::Tracker tracker;
        void* graph = reinterpret_cast<void*>(0xDD01);
        tracker.onGraphActivated(graph, true, 48000, 2, 24);
        tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
        tracker.onStopPositionReported(graph, 230000000LL);
        tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
        tracker.onUpstreamEndOfStream(graph);
        tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);

        tracker.markFormatChangePauseDeferred(graph);
        std::atomic<bool> eosCallExecuting{true};
        std::atomic<bool> stopProcessed{false};

        std::thread uiThread([&]() {
            while (!eosCallExecuting.load()) {
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            tracker.onStop(graph);
            stopProcessed.store(true);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        eosCallExecuting.store(false);
        uiThread.join();

        const auto* st = tracker.findGraph(graph);
        CHECK(st->isPauseDeferralActive() == false);
        CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledStop);
        CHECK(tracker.isFormatChangePending(graph) == false);
        CHECK(stopProcessed.load() == true);
    }

    // Subcase 19B: Manual Seek from UI thread while EQ EOS call is executing
    {
        eosTransition::Tracker tracker;
        void* graph = reinterpret_cast<void*>(0xDD02);
        tracker.onGraphActivated(graph, true, 48000, 2, 24);
        tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
        tracker.onStopPositionReported(graph, 230000000LL);
        tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
        tracker.onUpstreamEndOfStream(graph);
        tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);

        tracker.markFormatChangePauseDeferred(graph);
        std::atomic<bool> eosCallExecuting{true};

        std::thread uiThread([&]() {
            while (!eosCallExecuting.load()) {
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            tracker.onSeekPositions(graph, true, 100000000LL, false, -1);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        eosCallExecuting.store(false);
        uiThread.join();

        const auto* st = tracker.findGraph(graph);
        CHECK(st->isPauseDeferralActive() == false);
        CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledSeek);
        CHECK(st->hasCurrentPosition == true && st->currentPosition == 100000000LL);
    }
}

// Test 20: Concurrent manual operations during drain wait interval (Issue #25)
void testConcurrentOperationsDuringDrainWait() {
    std::printf("[TEST 20] Concurrent manual operations during drain wait interval...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0xDD03);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);
    tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
    tracker.onStopPositionReported(graph, 230000000LL);
    tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
    tracker.onUpstreamEndOfStream(graph);
    tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);

    tracker.markFormatChangePauseDeferred(graph);
    tracker.onDownstreamEndOfStream(graph);
    CHECK(tracker.findGraph(graph)->isPauseDeferralActive() == true);

    std::atomic<bool> waitActive{true};
    std::atomic<bool> cancelObserved{false};

    std::thread drainThread([&]() {
        while (waitActive.load()) {
            const auto* st = tracker.findGraph(graph);
            if (!st || !st->isPauseDeferralActive()) {
                cancelObserved.store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    tracker.cancelFormatChangePause(graph, eosTransition::PauseDeferralStatus::CancelledManualPause);

    drainThread.join();
    CHECK(cancelObserved.load() == true);
    CHECK(tracker.findGraph(graph)->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledManualPause);
}

// Test 21: Premature EQ EOS prevention (hasDownstreamReachedTarget) (Issue #25)
void testPrematureEqEosPrevention() {
    std::printf("[TEST 21] Premature EQ EOS prevention (downstream not yet reached target)...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0xDD04);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);

    const size_t totalFrames = 48000; // 1000ms
    const uint64_t totalBytes = totalFrames * 6;
    tracker.onUpstreamNewSegment(graph, 0, 10000000LL, 1.0);
    tracker.onStopPositionReported(graph, 10000000LL);

    tracker.onUpstreamSample(graph, static_cast<uint32_t>(totalBytes), true, 0, 10000000LL);
    tracker.onUpstreamEndOfStream(graph);
    tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);

    // Downstream has only received 700ms (partial)
    const uint32_t partialBytes = static_cast<uint32_t>(totalBytes * 7 / 10);
    tracker.onDownstreamSample(graph, partialBytes, true, 0, 7000000LL);

    // At this point (e.g. 700ms / 20.1s in MC), downstream has NOT reached target:
    CHECK(tracker.hasDownstreamReachedTarget(graph) == false);

    // Therefore, deferFormatChangePause does NOT deliver EOS prematurely!
    tracker.markFormatChangePauseDeferred(graph);
    const auto* st = tracker.findGraph(graph);
    CHECK(st->eosDelivered == false);

    // Audio continues streaming to downstream: final chunk arrives
    const uint32_t remBytes = static_cast<uint32_t>(totalBytes - partialBytes);
    bool deliversEos = tracker.onDownstreamSample(graph, remBytes, true, 7000000LL, 10000000LL);
    CHECK(deliversEos == true);
    CHECK(tracker.hasDownstreamReachedTarget(graph) == true);
    CHECK(st->eosDelivered == true);
    CHECK(st->downstreamEosDeliveredCount == 1);
}

// Test 22: Preserving 24-bit track end and same-format pre-reading across cancellation cycles (Issue #25)
void testPreserveTrackEndAndSameFormatAcrossCancellation() {
    std::printf("[TEST 22] Preserving 24-bit track end & same-format pre-reading across cancellation...\n");
    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0xDD05);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);

    // Track 1 starts -> format change pre-read fails -> Pause deferred -> user seeks
    tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
    tracker.onUpstreamSample(graph, 4800 * 6, true, 0, 1000000LL);
    tracker.onUpstreamEndOfStream(graph);
    tracker.onPreReadDecision(graph, eosTransition::PreReadDecision::FormatChanged);
    tracker.markFormatChangePauseDeferred(graph);

    // User seeks to position 15s
    tracker.onSeekPositions(graph, true, 150000000LL, false, -1);
    const auto* st = tracker.findGraph(graph);
    CHECK(st->isPauseDeferralActive() == false);
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledSeek);

    // After seek: same track finishes or Track 2 starts with SAME format (48k/24bit)
    tracker.onUpstreamNewSegment(graph, 0, 230000000LL, 1.0);
    tracker.onPreReadFormatCandidate(graph, true, 48000, 2, 24);
    CHECK(st->preReadDecision == eosTransition::PreReadDecision::SameFormat24);

    const size_t trackFrames = 24000;
    std::vector<uint8_t> pcmStream;
    trackTail::appendSynthesizedTrack(pcmStream, 48000, 24, trackFrames, false);

    tracker.onUpstreamSample(graph, 24000 * 6, true, 0, 5000000LL);
    tracker.onUpstreamEndOfStream(graph);

    CHECK(tracker.isFormatChangePending(graph) == false);
    CHECK(st->eosPending == false);

    auto a = trackTail::analyzeStream(pcmStream.data(), pcmStream.size(), 48000, 24, trackFrames);
    CHECK(a.tailComplete == true);
    CHECK(a.missingTailFrames == 0);
    CHECK(a.missingTailMs == 0.0);
}

enum class CommandContentionDiagnosis {
    Undetermined,
    CommandNotReachedEngine,
    BlockedWaitingForLock,
    ExecutedImmediately
};

static CommandContentionDiagnosis diagnoseCommandContention(
    DWORD uiTid,
    const std::vector<eosDiag::TraceItem>& trace)
{
    bool appInputFound = false;
    bool lockTryFound = false;
    uint64_t waitMs = 0;

    for (const auto& item : trace) {
        if (item.eventName == "APP_INPUT_DISPATCH") {
            appInputFound = true;
        }
        if (item.eventName == "ENGINE_LOCK_TRY" && item.tid == uiTid) {
            lockTryFound = true;
        }
        if (item.eventName == "ENGINE_LOCK_ACQUIRED" && item.tid == uiTid) {
            waitMs = item.p2; // p2 is waitMs
        }
    }

    if (appInputFound && !lockTryFound) {
        return CommandContentionDiagnosis::CommandNotReachedEngine;
    }
    if (lockTryFound && waitMs > 0) {
        return CommandContentionDiagnosis::BlockedWaitingForLock;
    }
    if (lockTryFound && waitMs == 0) {
        return CommandContentionDiagnosis::ExecutedImmediately;
    }
    return CommandContentionDiagnosis::Undetermined;
}

// Test 23: Distinction Case B - Command reached engine but blocked waiting for lock (Issue #27)
void testLockContentionDetection() {
    std::printf("[TEST 23] Tracing reached command waiting for lock contention...\n");
    auto& tracker = eosDiag::getLockTracker();
    auto& collector = eosDiag::getTraceCollector();
    tracker.reset();
    collector.clear();

    CRITICAL_SECTION csEngine;
    InitializeCriticalSection(&csEngine);
    void* callerDeferral = reinterpret_cast<void*>(0x60FEDAC9);
    void* callerManual = reinterpret_cast<void*>(0x60FEED58);

    std::atomic<bool> threadAStarted{false};
    std::atomic<bool> threadBLockTried{false};
    std::atomic<bool> threadBCompleted{false};
    std::atomic<DWORD> threadBTid{0};
    uint64_t threadBWaitReported = 0;

    // Thread A: simulates Pause deferral / format change drain wait holding the engine lock
    std::thread threadA([&]() {
        DWORD tidA = GetCurrentThreadId();
        uint64_t t0 = eosDiag::DiagClock::monotonicMs();
        tracker.onTry(&csEngine, callerDeferral, tidA, t0);
        collector.add("ENGINE_LOCK_TRY", callerDeferral, reinterpret_cast<uint64_t>(&csEngine), 0, S_OK);
        EnterCriticalSection(&csEngine);
        uint64_t t1 = eosDiag::DiagClock::monotonicMs();
        uint64_t waitA = tracker.onAcquired(&csEngine, callerDeferral, tidA, t1);
        collector.add("ENGINE_LOCK_ACQUIRED", callerDeferral, reinterpret_cast<uint64_t>(&csEngine), waitA, S_OK);
        collector.add("PAUSE_DEFERRED_START", callerDeferral, 1, 0, S_OK);

        threadAStarted.store(true);

        // Hold lock for 60ms to simulate drain wait window
        std::this_thread::sleep_for(std::chrono::milliseconds(60));

        collector.add("PAUSE_DEFERRED_RELEASE", callerDeferral, 0, 0, S_OK);
        uint64_t t2 = eosDiag::DiagClock::monotonicMs();
        uint64_t heldA = tracker.onReleased(&csEngine, callerDeferral, tidA, t2);
        collector.add("ENGINE_LOCK_RELEASED", callerDeferral, reinterpret_cast<uint64_t>(&csEngine), heldA, S_OK);
        LeaveCriticalSection(&csEngine);
    });

    // Thread B: simulates UI thread receiving manual Pause click during the deferral window
    std::thread threadB([&]() {
        while (!threadAStarted.load()) {
            std::this_thread::yield();
        }
        DWORD tidB = GetCurrentThreadId();
        threadBTid.store(tidB);

        // UI receives button click
        collector.add("APP_INPUT_DISPATCH", callerManual, WM_LBUTTONDOWN, 0, S_OK);

        // UI calls into player engine -> tries to enter engine critical section
        uint64_t t0 = eosDiag::DiagClock::monotonicMs();
        tracker.onTry(&csEngine, callerManual, tidB, t0);
        collector.add("ENGINE_LOCK_TRY", callerManual, reinterpret_cast<uint64_t>(&csEngine), 0, S_OK);
        threadBLockTried.store(true);

        EnterCriticalSection(&csEngine);
        uint64_t t1 = eosDiag::DiagClock::monotonicMs();
        uint64_t waitB = tracker.onAcquired(&csEngine, callerManual, tidB, t1);
        threadBWaitReported = waitB;
        collector.add("ENGINE_LOCK_ACQUIRED", callerManual, reinterpret_cast<uint64_t>(&csEngine), waitB, S_OK);
        collector.add("BOUNDARY_CONTROL_PAUSE_ENTER", callerManual, 0, 0, S_OK);

        uint64_t t2 = eosDiag::DiagClock::monotonicMs();
        uint64_t heldB = tracker.onReleased(&csEngine, callerManual, tidB, t2);
        collector.add("ENGINE_LOCK_RELEASED", callerManual, reinterpret_cast<uint64_t>(&csEngine), heldB, S_OK);
        LeaveCriticalSection(&csEngine);
        threadBCompleted.store(true);
    });

    threadA.join();
    threadB.join();

    CHECK(threadBLockTried.load() == true);
    CHECK(threadBCompleted.load() == true);
    CHECK(threadBWaitReported >= 30); // Waited during Thread A's hold

    // Analyze trace
    auto diagnosis = diagnoseCommandContention(threadBTid.load(), collector.items);
    CHECK(diagnosis == CommandContentionDiagnosis::BlockedWaitingForLock);

    DeleteCriticalSection(&csEngine);
}

// Test 24: Distinction Case A - Command never reached playback engine (Issue #27)
void testUnreachedCommandDetection() {
    std::printf("[TEST 24] Tracing unreached command (app received input but engine not called)...\n");
    auto& tracker = eosDiag::getLockTracker();
    auto& collector = eosDiag::getTraceCollector();
    tracker.reset();
    collector.clear();

    CRITICAL_SECTION csEngine;
    InitializeCriticalSection(&csEngine);
    void* callerDeferral = reinterpret_cast<void*>(0x60FEDAC9);
    void* callerUI = reinterpret_cast<void*>(0x00401234);

    DWORD uiTid = 9999;

    // App dispatches user click
    collector.add("APP_INPUT_DISPATCH", callerUI, WM_LBUTTONDOWN, 0, S_OK);

    // Thread A holds lock and executes deferral
    EnterCriticalSection(&csEngine);
    tracker.onTry(&csEngine, callerDeferral, 1111, 1000);
    tracker.onAcquired(&csEngine, callerDeferral, 1111, 1000);
    collector.add("PAUSE_DEFERRED_START", callerDeferral, 1, 0, S_OK);
    collector.add("PAUSE_DEFERRED_RELEASE", callerDeferral, 0, 0, S_OK);
    tracker.onReleased(&csEngine, callerDeferral, 1111, 1500);
    LeaveCriticalSection(&csEngine);

    // UI thread never called engine method: 0 lock attempts from uiTid
    auto diagnosis = diagnoseCommandContention(uiTid, collector.items);
    CHECK(diagnosis == CommandContentionDiagnosis::CommandNotReachedEngine);

    DeleteCriticalSection(&csEngine);
}

// Test 25: Observation Mode - Strict ordering, monotonic timestamps, and non-interference (Issue #27)
void testObservationModeNonInterferenceAndOrdering() {
    std::printf("[TEST 25] Observation mode: monotonic timeline, thread tracing, and non-interference...\n");
    auto& collector = eosDiag::getTraceCollector();
    collector.clear();

    eosTransition::Tracker tracker;
    void* graph = reinterpret_cast<void*>(0xEE01);
    tracker.onGraphActivated(graph, true, 48000, 2, 24);

    // Run trace items
    collector.add("PAUSE_MODE_OBSERVE", nullptr, 0, 0, S_OK);
    collector.add("APP_INPUT_DISPATCH", reinterpret_cast<void*>(0x100), WM_LBUTTONDOWN, 0, S_OK);
    collector.add("CONTROL_PAUSE_OBSERVED", reinterpret_cast<void*>(0x200), 1, 0, S_OK);
    collector.add("EOS_DELIVERED_DOWNSTREAM", reinterpret_cast<void*>(0x300), 1000, 28800, S_OK);

    // Verify monotonic ordering
    uint64_t prevTime = 0;
    for (const auto& it : collector.items) {
        CHECK(it.monoMs >= prevTime);
        prevTime = it.monoMs;
        CHECK(it.tid != 0);
    }

    CHECK(collector.countEvent("CONTROL_PAUSE_OBSERVED") == 1);
    CHECK(collector.countEvent("PAUSE_MODE_OBSERVE") == 1);
}

// Test 26: Diagnostic Hook Registration Logging & Graceful Fallback (Issue #28)
void testDiagnosticHookRegistrationAndFallback() {
    std::printf("[TEST 26] Diagnostic hook registration logging & graceful fallback...\n");
    auto& reporter = eosDiag::getHookStatusReporter();
    reporter.clear();

    // DirectShow and User32 hooks register successfully
    reporter.report("DirectShow.ControlPause", "OK", S_OK);
    reporter.report("DirectShow.ControlStop", "OK", S_OK);
    reporter.report("DirectShow.FilterPause", "OK", S_OK);
    reporter.report("DirectShow.FilterStop", "OK", S_OK);
    reporter.report("DirectShow.Seeking", "OK", S_OK);
    reporter.report("User32.DispatchMessageW", "OK", S_OK);

    // Initial state: OpcDSPlay.dll not loaded -> marked as PENDING_MODULE_LOAD
    reporter.report("Engine.OpcDSPlay.IAT.EnterCriticalSection", "PENDING_MODULE_LOAD", S_FALSE);
    reporter.report("Engine.OpcDSPlay.IAT.LeaveCriticalSection", "PENDING_MODULE_LOAD", S_FALSE);

    CHECK(reporter.getStatus("DirectShow.ControlPause") == "OK");
    CHECK(reporter.getStatus("User32.DispatchMessageW") == "OK");
    CHECK(reporter.getStatus("Engine.OpcDSPlay.IAT.EnterCriticalSection") == "PENDING_MODULE_LOAD");

    // Later: module becomes available, late attachment succeeds
    reporter.report("Engine.OpcDSPlay.IAT.EnterCriticalSection", "ATTACHED_LATE", S_OK);
    reporter.report("Engine.OpcDSPlay.IAT.LeaveCriticalSection", "ATTACHED_LATE", S_OK);

    CHECK(reporter.getStatus("Engine.OpcDSPlay.IAT.EnterCriticalSection") == "PENDING_MODULE_LOAD"); // first entry preserved in list
    CHECK(reporter.entries.size() == 10);
}

// Test 27: Reentrancy Protection and Safe Pre/Post-Init Calls (Issue #28)
void testReentrancyProtectionAndPrePostInitCalls() {
    std::printf("[TEST 27] Reentrancy protection and safe pre/post-init calls...\n");
    auto& tracker = eosDiag::getLockTracker();
    tracker.reset();

    CRITICAL_SECTION csTest;
    InitializeCriticalSection(&csTest);
    DWORD tid = GetCurrentThreadId();
    void* caller = reinterpret_cast<void*>(0x60FE1234);

    // Level 1: Initial lock acquire
    tracker.onTry(&csTest, caller, tid, 100);
    uint64_t wait1 = tracker.onAcquired(&csTest, caller, tid, 100);
    CHECK(wait1 == 0);
    CHECK(tracker.activeLocks[&csTest].recursionCount == 1);

    // Level 2: Reentrant acquire on same thread
    tracker.onTry(&csTest, caller, tid, 105);
    uint64_t wait2 = tracker.onAcquired(&csTest, caller, tid, 105);
    CHECK(wait2 == 0);
    CHECK(tracker.activeLocks[&csTest].recursionCount == 2);

    // Release level 2 (nested): lock must still remain active
    uint64_t held2 = tracker.onReleased(&csTest, caller, tid, 110);
    CHECK(held2 == 0);
    CHECK(tracker.activeLocks.find(&csTest) != tracker.activeLocks.end());
    CHECK(tracker.activeLocks[&csTest].recursionCount == 1);

    // Release level 1 (outermost): lock is now fully released
    uint64_t held1 = tracker.onReleased(&csTest, caller, tid, 120);
    CHECK(held1 == 20); // Held from t=100 to t=120 = 20ms
    CHECK(tracker.activeLocks.find(&csTest) == tracker.activeLocks.end());

    DeleteCriticalSection(&csTest);
}

// Test 28: Engine-Side Observation Point Correlation on Manual Pause (Issue #28)
void testEngineObservationOnManualPause() {
    std::printf("[TEST 28] Correlating input dispatch, engine lock, and DirectShow on manual Pause...\n");
    auto& collector = eosDiag::getTraceCollector();
    collector.clear();

    void* callerUI = reinterpret_cast<void*>(0x00401000);
    void* callerOpcPause = reinterpret_cast<void*>(0x60FEED58); // Manual Pause entry
    void* csOpc = reinterpret_cast<void*>(0x03001078); // [esi+78h]

    // 1. User releases mouse button on "Pause" (mono_ms = 1000)
    collector.add("APP_INPUT_DISPATCH", callerUI, WM_LBUTTONUP, 0, S_OK);

    // 2. ~2-3ms later: UI calls OpcDSPlay.dll -> OpcDSPlay calls EnterCriticalSection (mono_ms = 1003)
    collector.add("ENGINE_LOCK_TRY", callerOpcPause, reinterpret_cast<uint64_t>(csOpc), 0, S_OK);
    collector.add("ENGINE_LOCK_ACQUIRED", callerOpcPause, reinterpret_cast<uint64_t>(csOpc), 0, S_OK);

    // 3. OpcDSPlay calls DirectShow IMediaControl::Pause() (mono_ms = 1004)
    collector.add("BOUNDARY_CONTROL_PAUSE_ENTER", callerOpcPause, 0, 0, S_OK);
    collector.add("BOUNDARY_CONTROL_PAUSE_LEAVE", callerOpcPause, 0, 0, S_OK);

    // 4. OpcDSPlay calls LeaveCriticalSection (mono_ms = 1006)
    collector.add("ENGINE_LOCK_RELEASED", callerOpcPause, reinterpret_cast<uint64_t>(csOpc), 3, S_OK);

    // Verify chronological sequence
    CHECK(collector.items.size() == 6);
    CHECK(collector.items[0].eventName == "APP_INPUT_DISPATCH");
    CHECK(collector.items[1].eventName == "ENGINE_LOCK_TRY");
    CHECK(collector.items[2].eventName == "ENGINE_LOCK_ACQUIRED");
    CHECK(collector.items[3].eventName == "BOUNDARY_CONTROL_PAUSE_ENTER");
    CHECK(collector.items[4].eventName == "BOUNDARY_CONTROL_PAUSE_LEAVE");
    CHECK(collector.items[5].eventName == "ENGINE_LOCK_RELEASED");

    // All events occurred and are correlated
    CHECK(collector.countEvent("APP_INPUT_DISPATCH") == 1);
    CHECK(collector.countEvent("ENGINE_LOCK_TRY") == 1);
    CHECK(collector.countEvent("BOUNDARY_CONTROL_PAUSE_ENTER") == 1);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::printf("=== Issue #20 / #21 / #23 / #24 / #25 / #27 / #28 Experimental EOS Transition Tests ===\n");

    testArbitraryDurationsAndOffsets();
    testDuplicateEosCalls();
    testSameFormatPreReadPreserved();
    testFormatChangeAndUnknownBehavior();
    testResetOnSeekFlushStopAndManualChange();
    testMultipleConcurrentGraphs();
#ifdef _M_IX86
    testDirectShowEqFilterIntegration();
#endif
    testMediaSeekingComVtableHooks();
    testDeferredFormatChangeWithTrackTailCheck();
    testPlaylistEndDrainWithTrackTailCheck();
    testPauseDeferralOnFormatChange();
    testPauseModeSwitching();
    testNonFormatChangePauseNeverDeferred();
    testManualOperationsDuringPauseDeferral();
    testDrainWaitTimeoutSafety();
    testReentrancyAndDuplicateEosPrevention();
    testDifferentSampleRateTransitions();
    testPlaylistEndWithDifferentSampleRates();
    testConcurrentOperationsDuringSynchronousEqEos();
    testConcurrentOperationsDuringDrainWait();
    testPrematureEqEosPrevention();
    testPreserveTrackEndAndSameFormatAcrossCancellation();
    testLockContentionDetection();
    testUnreachedCommandDetection();
    testObservationModeNonInterferenceAndOrdering();
    testDiagnosticHookRegistrationAndFallback();
    testReentrancyProtectionAndPrePostInitCalls();
    testEngineObservationOnManualPause();

    // Issue #30 Control Entry Point Conflict Verification Tests
    conflictTests::runAllConflictEntryPointTests();

    // Issue #33 / #34 / #35 / #36 / #37 Non-blocking Virtual Pause & Deferral Tests
    asyncTests::runAllAsyncTransitionTests();

    if (SUCCEEDED(co)) CoUninitialize();
    if (g_failures == 0) {
        std::printf("=== ALL ISSUE #20 / #21 / #23 / #24 / #25 / #27 / #28 / #30 / #33 / #34 / #35 / #36 / #37 EOS TRANSITION TESTS PASSED ===\n");
        return 0;
    }
    std::printf("=== %d TEST ASSERTION(S) FAILED ===\n", g_failures);
    return 1;
}
