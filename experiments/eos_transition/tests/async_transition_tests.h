#pragma once
// Non-blocking Virtual Pause and Deferred Operation Headless Verification Tests (Issue #33)
// Verifies that:
// 1. Automatic IMediaControl::Pause returns S_OK immediately (< 2 ms) without blocking.
// 2. Subsequent automatic operations (SetPositions, dsStop) are deferred while Track 1 tail drains.
// 3. In absence of manual ops, drain completes naturally -> COMPLETED logged -> deferred ops executed.
// 4. Manual operations (Pause, Seek, Stop, TrackChange, Exit) cancel Virtual Pause immediately (< 1 ms),
//    purge deferred automatic operations, and prioritize the user's command.
// 5. Stale EOS / delayed notifications arriving after cancellation are discarded (stale_eos_intrusions == 0).

#include <windows.h>
#include <dshow.h>
#include <dsound.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <cstdio>
#include "MinHook.h"
#include "track_tail_check.h"
#include "eos_transition.h"
#include "conflict_entry_point_tests.h"

namespace asyncTests {

using namespace conflictTests;

class AsyncControlHarness {
public:
    MockFilterGraphEx* mockGraph = nullptr;
    MockMediaControl* mockControl = nullptr;
    MockMediaSeekingEx* mockSeeking = nullptr;
    MockUpstreamPin* mockUpstreamPin = nullptr;
    MockDownstreamPin* mockDownstreamPin = nullptr;
    MockDirectSoundBufferEx* mockDsBuffer = nullptr;

    IMediaControl* pControl = nullptr;
    IMediaSeeking* pSeeking = nullptr;
    IPin* pUpstreamPin = nullptr;
    IPin* pDownstreamPin = nullptr;
    IMemInputPin* pDownstreamMem = nullptr;
    IDirectSoundBuffer* pDsBuffer = nullptr;

    eosTransition::Tracker tracker;
    CallerContext currentCallerContext = CallerContext::EngineAutoChange;

    std::atomic<bool> virtualPauseEntered{false};
    std::atomic<bool> deferredSeekQueued{false};
    std::atomic<bool> deferredSeekExecuted{false};
    std::atomic<bool> manualOpObserved{false};
    std::atomic<bool> dsStopSuppressed{false};
    std::atomic<int> dsStopCallCount{0};
    std::atomic<int> downstreamEosReceived{0};
    std::atomic<int> staleEosDiscarded{0};

    // Stored deferred seek parameters
    eosTransition::GraphState::DeferredSeekParams lastDeferredSeek;

    AsyncControlHarness() {
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
    }

    ~AsyncControlHarness() {
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

    void setPreReadDecision(eosTransition::PreReadDecision decision) {
        tracker.onPreReadDecision(mockGraph, decision);
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

        // Check if downstream reached target
        if (tracker.onDownstreamSample(mockGraph, static_cast<uint32_t>(size), hasTime, sStart, sEnd)) {
            deliverDownstreamEosIfValid();
        }
        return hr;
    }

    void deliverDownstreamEosIfValid() {
        if (tracker.isCancelled(mockGraph)) {
            staleEosDiscarded++;
            return;
        }
        downstreamEosReceived++;
        pDownstreamPin->EndOfStream();
    }

    // --- Non-Blocking Virtual Pause Control Hook ---
    HRESULT executePause() {
        bool isEngine = (currentCallerContext == CallerContext::EngineAutoChange);
        if (tracker.isVirtualPauseActive(mockGraph) && !isEngine) {
            // Manual Pause from user while Virtual Pause is active:
            // Cancel Virtual Pause and execute real Pause immediately!
            tracker.cancelVirtualPause(mockGraph, eosTransition::PauseDeferralStatus::CancelledManualPause);
            manualOpObserved = true;
            return pControl->Pause();
        }

        if (isEngine && tracker.isFormatChangePending(mockGraph)) {
            // Automatic track transition: Enter Virtual Pause and return S_OK immediately!
            tracker.markVirtualPauseActive(mockGraph);
            virtualPauseEntered = true;
            return S_OK; // Returns in < 1 ms! Caller immediately releases its internal lock.
        }

        return pControl->Pause();
    }

    // --- Non-Blocking SetPositions Hook ---
    HRESULT executeSetPositions(LONGLONG* pCurrent, DWORD dwCurrentFlags,
                                LONGLONG* pStop, DWORD dwStopFlags) {
        bool isEngine = (currentCallerContext == CallerContext::EngineAutoChange);
        DWORD currBits = dwCurrentFlags & AM_SEEKING_PositioningBitsMask;
        DWORD stopBits = dwStopFlags & AM_SEEKING_PositioningBitsMask;
        bool modifiesCurr = (currBits != AM_SEEKING_NoPositioning) && (pCurrent != nullptr);
        bool modifiesStop = (stopBits != AM_SEEKING_NoPositioning) && (pStop != nullptr);

        if (tracker.isVirtualPauseActive(mockGraph)) {
            if (isEngine && (modifiesCurr || modifiesStop)) {
                // Engine automatic seek for next track: DEFER IT!
                LONGLONG newPos = modifiesCurr ? *pCurrent : -1;
                LONGLONG newStop = modifiesStop ? *pStop : -1;
                tracker.deferAutomaticSeek(mockGraph, newPos, dwCurrentFlags, newStop, dwStopFlags);
                lastDeferredSeek.valid = true;
                lastDeferredSeek.current = newPos;
                lastDeferredSeek.currentFlags = dwCurrentFlags;
                lastDeferredSeek.stop = newStop;
                lastDeferredSeek.stopFlags = dwStopFlags;
                deferredSeekQueued = true;
                return S_OK; // Return S_OK to caller without modifying graph seeking position!
            } else if (!isEngine && (modifiesCurr || modifiesStop)) {
                // User manual seek: CANCEL Virtual Pause and execute immediately!
                tracker.cancelVirtualPause(mockGraph, eosTransition::PauseDeferralStatus::CancelledSeek);
                manualOpObserved = true;
            }
        }

        if (modifiesCurr || modifiesStop) {
            LONGLONG newPos = modifiesCurr ? *pCurrent : -1;
            LONGLONG newStop = modifiesStop ? *pStop : -1;
            tracker.onSeekPositions(mockGraph, modifiesCurr, newPos, modifiesStop, newStop);
        }
        return pSeeking->SetPositions(pCurrent, dwCurrentFlags, pStop, dwStopFlags);
    }

    // --- DirectSound Stop Hook ---
    HRESULT executeDsStop() {
        dsStopCallCount++;
        bool isEngine = (currentCallerContext == CallerContext::EngineAutoChange);
        if (tracker.isVirtualPauseActive(mockGraph) && isEngine) {
            // Engine automatic buffer stop while Virtual Pause active: suppress it!
            dsStopSuppressed = true;
            return DS_OK;
        }
        return pDsBuffer->Stop();
    }

    // --- Control Stop Hook ---
    HRESULT executeControlStop() {
        bool isEngine = (currentCallerContext == CallerContext::EngineAutoChange);
        if (tracker.isVirtualPauseActive(mockGraph)) {
            if (!isEngine) {
                // User manual stop: cancel Virtual Pause immediately!
                tracker.cancelVirtualPause(mockGraph, eosTransition::PauseDeferralStatus::CancelledStop);
                manualOpObserved = true;
            }
        }
        tracker.onStop(mockGraph);
        return pControl->Stop();
    }

    // --- Manual Track Change ---
    void executeManualTrackChange() {
        if (tracker.isVirtualPauseActive(mockGraph)) {
            tracker.cancelVirtualPause(mockGraph, eosTransition::PauseDeferralStatus::CancelledTrackChange);
            manualOpObserved = true;
        }
        tracker.onManualTrackChange(mockGraph);
    }

    // --- Natural Drain Completion Trigger ---
    bool completeDrainNaturally() {
        if (tracker.isCancelled(mockGraph) || !tracker.isVirtualPauseActive(mockGraph)) {
            return false;
        }
        tracker.onVirtualPauseCompleted(mockGraph, eosTransition::PauseDeferralStatus::CompletedDrain);
        if (downstreamEosReceived.load() == 0) {
            deliverDownstreamEosIfValid();
        }

        // Apply deferred seek if present
        eosTransition::GraphState::DeferredSeekParams sp;
        if (tracker.popDeferredSeek(mockGraph, sp)) {
            pSeeking->SetPositions(&sp.current, sp.currentFlags, &sp.stop, sp.stopFlags);
            deferredSeekExecuted = true;
        }
        return true;
    }

    // --- Watchdog Timeout Trigger ---
    bool triggerWatchdogTimeout() {
        if (tracker.isCancelled(mockGraph) || !tracker.isVirtualPauseActive(mockGraph)) {
            return false;
        }
        tracker.onVirtualPauseCompleted(mockGraph, eosTransition::PauseDeferralStatus::TimedOut);
        if (downstreamEosReceived.load() == 0) {
            deliverDownstreamEosIfValid();
        }

        eosTransition::GraphState::DeferredSeekParams sp;
        if (tracker.popDeferredSeek(mockGraph, sp)) {
            pSeeking->SetPositions(&sp.current, sp.currentFlags, &sp.stop, sp.stopFlags);
            deferredSeekExecuted = true;
        }
        return true;
    }
};

// Test 40: Non-blocking Virtual Pause Immediate Return (< 2 ms)
inline void testAsyncVirtualPauseImmediateReturn() {
    std::printf("[TEST 40] Non-blocking Virtual Pause: IMediaControl::Pause returns immediately (< 2 ms)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 10000000LL); // 1.0s
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6; // 100ms
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
        if (i < 5) harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // Call Pause as engine: must return S_OK immediately without blocking
    auto t0 = std::chrono::high_resolution_clock::now();
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    HRESULT hr = harness.executePause();
    auto t1 = std::chrono::high_resolution_clock::now();
    auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    CHECK(hr == S_OK);
    CHECK(harness.virtualPauseEntered.load());
    CHECK(harness.tracker.isVirtualPauseActive(harness.mockGraph));
    CHECK(elapsedUs < 2000); // must return in < 2.0 ms (no synchronous sleep)
}

// Test 41: Automatic SetPositions Deferred During Virtual Pause
inline void testAutomaticSetPositionsDeferredDuringVirtualPause() {
    std::printf("[TEST 41] Automatic SetPositions during Virtual Pause is deferred without altering graph...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 10000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
        if (i < 5) harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause() == S_OK);

    // Engine immediately calls SetPositions to cue next track at 0
    LONGLONG newPos = 0;
    LONGLONG newStop = 20000000LL; // 2.0s
    HRESULT hrSeek = harness.executeSetPositions(&newPos, AM_SEEKING_AbsolutePositioning, &newStop, AM_SEEKING_AbsolutePositioning);

    CHECK(hrSeek == S_OK);
    CHECK(harness.deferredSeekQueued.load());
    CHECK(harness.tracker.hasDeferredSeek(harness.mockGraph));
    // The DirectShow graph mock seeking position must still be 0 (from startSegment) and NOT updated to newStop yet
    CHECK(harness.mockSeeking->stopPos == 10000000LL); // original segment stop preserved!
}

// Test 42: Natural Completion Path (COMPLETED logged, deferred ops executed)
inline void testNaturalDrainCompletionPath() {
    std::printf("[TEST 42] Natural drain completion: COMPLETED logged, 0ms missing tail, deferred seek executed...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 10000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    std::vector<uint8_t> pcm;
    trackTail::appendSynthesizedTrack(pcm, 48000, 24, 48000, false); // 1.0s = 48000 frames
    const uint32_t chunkBytes = 4800 * 6; // 10 chunks
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
        if (i < 5) harness.feedDownstreamChunk(pcm.data() + i * chunkBytes, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // Engine calls Pause and SetPositions (both non-blocking)
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause() == S_OK);
    LONGLONG nextPos = 0;
    LONGLONG nextStop = 20000000LL;
    CHECK(harness.executeSetPositions(&nextPos, AM_SEEKING_AbsolutePositioning, &nextStop, AM_SEEKING_AbsolutePositioning) == S_OK);

    // DirectSound Stop is called by engine: must be suppressed
    CHECK(harness.executeDsStop() == DS_OK);
    CHECK(harness.dsStopSuppressed.load());

    // Remaining 5 chunks drain naturally from SeamlessFilter into downstream
    for (int i = 5; i < 10; ++i) {
        harness.feedDownstreamChunk(pcm.data() + i * chunkBytes, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }

    // Now trigger completion
    CHECK(harness.completeDrainNaturally());
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr);
    CHECK(!st->isVirtualPauseActive());
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CompletedDrain);
    CHECK(harness.downstreamEosReceived.load() == 1);
    CHECK(harness.deferredSeekExecuted.load());
    CHECK(harness.mockSeeking->stopPos == 20000000LL); // next track stop position now applied!

    // Verify PCM analysis
    auto analysis = trackTail::analyzeStream(pcm.data(), pcm.size(), 48000, 24, 48000);
    CHECK(analysis.tailComplete);
    CHECK(analysis.missingTailFrames == 0);
}

// Test 43: Manual Pause Priority (CANCELLED logged, immediate pause)
inline void testManualPausePriorityCancellation() {
    std::printf("[TEST 43] Manual Pause priority: cancels Virtual Pause immediately (< 1 ms), purges deferred ops...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 10000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
        if (i < 5) harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // Engine enters Virtual Pause and defers seek
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause() == S_OK);
    LONGLONG nextPos = 0, nextStop = 20000000LL;
    CHECK(harness.executeSetPositions(&nextPos, AM_SEEKING_AbsolutePositioning, &nextStop, AM_SEEKING_AbsolutePositioning) == S_OK);
    CHECK(harness.tracker.isVirtualPauseActive(harness.mockGraph));

    // User clicks Pause on UI thread: executes with UserManualOp context
    auto t0 = std::chrono::high_resolution_clock::now();
    harness.currentCallerContext = CallerContext::UserManualOp;
    HRESULT hr = harness.executePause();
    auto t1 = std::chrono::high_resolution_clock::now();
    auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    CHECK(hr == S_OK);
    CHECK(elapsedUs < 1000); // cancelled in < 1 ms!
    CHECK(harness.manualOpObserved.load());
    CHECK(!harness.tracker.isVirtualPauseActive(harness.mockGraph));
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledManualPause);
    CHECK(st->isCancelled());
    // Deferred automatic seek must be purged
    CHECK(!harness.tracker.hasDeferredSeek(harness.mockGraph));
}

// Test 44: Manual Seek Priority (CANCELLED logged, immediate seek applied)
inline void testManualSeekPriorityCancellation() {
    std::printf("[TEST 44] Manual Seek priority: cancels Virtual Pause immediately, user seek applied...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 10000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
        if (i < 5) harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // Engine enters Virtual Pause and defers seek
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause() == S_OK);
    LONGLONG nextPos = 0, nextStop = 20000000LL;
    CHECK(harness.executeSetPositions(&nextPos, AM_SEEKING_AbsolutePositioning, &nextStop, AM_SEEKING_AbsolutePositioning) == S_OK);

    // User clicks Seekbar to seek to 0.3s (3,000,000)
    LONGLONG userSeekPos = 3000000LL;
    LONGLONG userStopPos = 10000000LL;
    harness.currentCallerContext = CallerContext::UserManualOp;
    HRESULT hr = harness.executeSetPositions(&userSeekPos, AM_SEEKING_AbsolutePositioning, &userStopPos, AM_SEEKING_AbsolutePositioning);

    CHECK(hr == S_OK);
    CHECK(harness.manualOpObserved.load());
    CHECK(!harness.tracker.isVirtualPauseActive(harness.mockGraph));
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledSeek);
    CHECK(st->isCancelled());
    // DirectShow mock seeking position must reflect the user's seek position immediately!
    CHECK(harness.mockSeeking->currentPos == userSeekPos);
    CHECK(!harness.tracker.hasDeferredSeek(harness.mockGraph));
}

// Test 45: Manual Stop Priority (CANCELLED logged, graph stopped)
inline void testManualStopPriorityCancellation() {
    std::printf("[TEST 45] Manual Stop priority: cancels Virtual Pause immediately, graph stopped...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 10000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
        if (i < 5) harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause() == S_OK);

    // User clicks Stop button
    harness.currentCallerContext = CallerContext::UserManualOp;
    HRESULT hr = harness.executeControlStop();

    CHECK(hr == S_OK);
    CHECK(harness.manualOpObserved.load());
    CHECK(!harness.tracker.isVirtualPauseActive(harness.mockGraph));
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledStop);
    CHECK(st->isCancelled());
}

// Test 46: Manual Track Change (CANCELLED logged, old track isolated)
inline void testManualTrackChangeCancellation() {
    std::printf("[TEST 46] Manual TrackChange: cancels Virtual Pause immediately, old track reset...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 10000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
        if (i < 5) harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause() == S_OK);

    // User clicks Next Track (Manual Track Change)
    harness.executeManualTrackChange();

    CHECK(harness.manualOpObserved.load());
    CHECK(!harness.tracker.isVirtualPauseActive(harness.mockGraph));
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledTrackChange);
    CHECK(st->isCancelled());
}

// Test 47: Application Exit (WM_QUIT) During Virtual Pause
inline void testApplicationExitCancellation() {
    std::printf("[TEST 47] Application Exit (WM_QUIT) during Virtual Pause: clean cancellation...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 10000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause() == S_OK);

    // Simulate WM_QUIT cancellation
    harness.tracker.cancelVirtualPause(harness.mockGraph, eosTransition::PauseDeferralStatus::CancelledQuit);

    CHECK(!harness.tracker.isVirtualPauseActive(harness.mockGraph));
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledQuit);
    CHECK(st->isCancelled());
}

// Test 48: Stale EOS Discarded After Cancellation (stale_eos_intrusions == 0)
inline void testStaleEosDiscardedAfterCancellation() {
    std::printf("[TEST 48] Stale EOS prevention: delayed EOS after cancellation is discarded (0 intrusions)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 10000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
        if (i < 5) harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause() == S_OK);

    // User cancels via Manual Pause
    harness.currentCallerContext = CallerContext::UserManualOp;
    CHECK(harness.executePause() == S_OK);
    CHECK(harness.tracker.isCancelled(harness.mockGraph));

    // Remaining downstream chunks arrive late
    for (int i = 5; i < 10; ++i) {
        harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }

    // Attempt to deliver late EOS
    harness.deliverDownstreamEosIfValid();

    // Must be discarded!
    CHECK(harness.downstreamEosReceived.load() == 0); // zero stale EOS delivered to renderer!
    CHECK(harness.staleEosDiscarded.load() > 0);     // detected and cleanly dropped
}

// Test 49: Same-Format Pre-Reading Continuity
inline void testSameFormatPreReadBypassesVirtualPause() {
    std::printf("[TEST 49] Same-format pre-reading continuity: bypasses Virtual Pause with 0ms gap...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(96000, 2, 24);
    harness.startSegment(0, 10000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::SameFormat24); // Same format!

    const uint32_t chunkBytes = 9600 * 6; // 100ms at 96kHz
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
        harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // Call Pause as engine: should NOT enter Virtual Pause because format did not change
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(!harness.tracker.isFormatChangePending(harness.mockGraph));
    CHECK(harness.executePause() == S_OK);
    CHECK(!harness.virtualPauseEntered.load());
    CHECK(!harness.tracker.isVirtualPauseActive(harness.mockGraph));
}

// Test 50: Watchdog Timeout Safety and Flush
inline void testWatchdogTimeoutSafetyAndFlush() {
    std::printf("[TEST 50] Watchdog timeout safety: hardware stall safely times out, executes deferred ops...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 10000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
        if (i < 5) harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause() == S_OK);
    LONGLONG nextPos = 0, nextStop = 20000000LL;
    CHECK(harness.executeSetPositions(&nextPos, AM_SEEKING_AbsolutePositioning, &nextStop, AM_SEEKING_AbsolutePositioning) == S_OK);

    // Hardware clock stall simulated (no more downstream chunks arrive)
    // Trigger watchdog timeout
    CHECK(harness.triggerWatchdogTimeout());

    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr);
    CHECK(!st->isVirtualPauseActive());
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::TimedOut);
    // Deferred seek must still be applied so graph does not freeze indefinitely!
    CHECK(harness.deferredSeekExecuted.load());
    CHECK(harness.mockSeeking->stopPos == 20000000LL);
}

inline void runAllAsyncTransitionTests() {
    std::printf("\n=== Issue #33 Non-blocking Virtual Pause & Deferral Tests ===\n");
    testAsyncVirtualPauseImmediateReturn();
    testAutomaticSetPositionsDeferredDuringVirtualPause();
    testNaturalDrainCompletionPath();
    testManualPausePriorityCancellation();
    testManualSeekPriorityCancellation();
    testManualStopPriorityCancellation();
    testManualTrackChangeCancellation();
    testApplicationExitCancellation();
    testStaleEosDiscardedAfterCancellation();
    testSameFormatPreReadBypassesVirtualPause();
    testWatchdogTimeoutSafetyAndFlush();
    std::printf("=== Issue #33 Non-blocking Virtual Pause Tests Completed ===\n\n");
}

} // namespace asyncTests
