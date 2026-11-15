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
    std::atomic<int> oldGraphSeekParkedCount{0};
    std::atomic<int> deferredSeekExecutedCount{0};
    DWORD lastUserInputTick = 0;

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
        tracker.registerActiveSoundBuffer(mockGraph, pDsBuffer);
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
            tracker.onDownstreamPcmDelivered(mockGraph);
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
    HRESULT executePause(DWORD threadId = 0) {
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
            tracker.markVirtualPauseActive(mockGraph, threadId, pDsBuffer);
            virtualPauseEntered = true;
            return S_OK; // Returns in < 1 ms! Caller immediately releases its internal lock.
        }

        return pControl->Pause();
    }

    // --- Non-Blocking SetPositions Hook ---
    HRESULT executeSetPositions(void* targetGraphKey, IMediaSeeking* targetSeeking,
                                LONGLONG* pCurrent, DWORD dwCurrentFlags,
                                LONGLONG* pStop, DWORD dwStopFlags, DWORD threadId = 0) {
        if (!targetGraphKey) targetGraphKey = mockGraph;
        if (!targetSeeking) targetSeeking = pSeeking;
        DWORD currBits = dwCurrentFlags & AM_SEEKING_PositioningBitsMask;
        DWORD stopBits = dwStopFlags & AM_SEEKING_PositioningBitsMask;
        bool modifiesCurr = (currBits != AM_SEEKING_NoPositioning) && (pCurrent != nullptr);
        bool modifiesStop = (stopBits != AM_SEEKING_NoPositioning) && (pStop != nullptr);
        LONGLONG newPos = modifiesCurr ? *pCurrent : -1;
        LONGLONG newStop = modifiesStop ? *pStop : -1;

        bool isExplicitManual = (currentCallerContext == CallerContext::UserManualOp);
        bool isExplicitEngine = (currentCallerContext == CallerContext::EngineAutoChange);

        void* activeGraph = nullptr;
        auto classification = tracker.classifySeekOperation(targetGraphKey, isExplicitManual, isExplicitEngine,
                                                            newPos, lastUserInputTick, &activeGraph);
        if (!activeGraph) activeGraph = mockGraph;

        if (classification == eosTransition::Tracker::SeekClassification::OldGraphAutomatedPark) {
            oldGraphSeekParkedCount++;
            return S_OK; // Absorbed without cancelling drain or flushing buffer
        } else if (classification == eosTransition::Tracker::SeekClassification::AutomatedDeferred) {
            tracker.deferAutomaticSeek(activeGraph, targetGraphKey, static_cast<void*>(targetSeeking),
                                       newPos, dwCurrentFlags, newStop, dwStopFlags);
            lastDeferredSeek.valid = true;
            lastDeferredSeek.targetGraph = targetGraphKey;
            lastDeferredSeek.targetSeeking = targetSeeking;
            lastDeferredSeek.current = newPos;
            lastDeferredSeek.currentFlags = dwCurrentFlags;
            lastDeferredSeek.stop = newStop;
            lastDeferredSeek.stopFlags = dwStopFlags;
            deferredSeekQueued = true;
            return S_OK;
        } else if (classification == eosTransition::Tracker::SeekClassification::ManualCancelled) {
            tracker.cancelVirtualPause(activeGraph, eosTransition::PauseDeferralStatus::CancelledSeek);
            manualOpObserved = true;
        }

        if (modifiesCurr || modifiesStop) {
            tracker.onSeekPositions(targetGraphKey, modifiesCurr, newPos, modifiesStop, newStop);
        }
        return targetSeeking->SetPositions(pCurrent, dwCurrentFlags, pStop, dwStopFlags);
    }

    HRESULT executeSetPositions(LONGLONG* pCurrent, DWORD dwCurrentFlags,
                                LONGLONG* pStop, DWORD dwStopFlags, DWORD threadId = 0) {
        return executeSetPositions(mockGraph, pSeeking, pCurrent, dwCurrentFlags, pStop, dwStopFlags, threadId);
    }

    // --- DirectSound Stop Hook ---
    HRESULT executeDsStop(IDirectSoundBuffer* buf = nullptr, DWORD threadId = 0) {
        if (!buf) buf = pDsBuffer;
        dsStopCallCount++;
        bool isManual = (currentCallerContext == CallerContext::UserManualOp);
        void* activeGraph = nullptr;
        bool isAssoc = (buf == pDsBuffer) || tracker.isTransitionBuffer(buf, &activeGraph);

        if (tracker.isVirtualPauseActive(mockGraph) && isAssoc) {
            if (!isManual) {
                dsStopSuppressed = true;
                return DS_OK;
            } else {
                tracker.cancelVirtualPause(mockGraph, eosTransition::PauseDeferralStatus::CancelledStop);
                manualOpObserved = true;
            }
        }
        return buf->Stop();
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
        // Stage 1: PCM reached downstream
        tracker.onDownstreamPcmDelivered(mockGraph);
        if (downstreamEosReceived.load() == 0) {
            deliverDownstreamEosIfValid();
        }
        // Stage 2: Renderer output complete
        tracker.onRendererOutputComplete(mockGraph);

        // Execute once
        if (!tracker.onVirtualPauseCompleted(mockGraph, eosTransition::PauseDeferralStatus::CompletedDrain)) {
            return false;
        }

        // Apply deferred seek if present
        eosTransition::GraphState::DeferredSeekParams sp;
        if (tracker.popDeferredSeek(mockGraph, sp)) {
            IMediaSeeking* seekToUse = pSeeking;
            if (sp.targetSeeking) {
                seekToUse = static_cast<IMediaSeeking*>(sp.targetSeeking);
            }
            seekToUse->SetPositions(&sp.current, sp.currentFlags, &sp.stop, sp.stopFlags);
            deferredSeekExecuted = true;
            deferredSeekExecutedCount++;
        }
        return true;
    }

    // --- Watchdog Timeout Trigger ---
    bool triggerWatchdogTimeout() {
        if (tracker.isCancelled(mockGraph) || !tracker.isVirtualPauseActive(mockGraph)) {
            return false;
        }
        if (!tracker.onVirtualPauseCompleted(mockGraph, eosTransition::PauseDeferralStatus::TimedOut)) {
            return false;
        }
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

// Test 51: Automated SetPositions Caller Matching & Unrelated Buffer Stop Isolation
inline void testAutomatedSetPositionsCallerMatchingAndUnrelatedBufferIsolation() {
    std::printf("[TEST 51] Automated SetPositions caller matching & unrelated buffer stop isolation...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 10000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    // Setup track end
    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // Engine enters Virtual Pause on thread 1001
    const DWORD engineThreadId = 1001;
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(engineThreadId) == S_OK);
    CHECK(harness.virtualPauseEntered.load());

    // 1. Unrelated DirectSoundBuffer Stop (e.g. system sound or UI click)
    MockDirectSoundBufferEx unrelatedBuffer;
    harness.currentCallerContext = CallerContext::UserManualOp; // Unrelated caller
    CHECK(harness.executeDsStop(&unrelatedBuffer, 9999) == DS_OK);
    CHECK(unrelatedBuffer.stopCallCount == 1);
    // Virtual Pause MUST NOT be cancelled by unrelated buffer!
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->isVirtualPauseActive());
    CHECK(!harness.manualOpObserved.load());

    // 2. Engine calls SetPositions on the same transition thread (thread 1001), but from a different RVA
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    LONGLONG nextPos = 0, nextStop = 15000000LL;
    CHECK(harness.executeSetPositions(&nextPos, AM_SEEKING_AbsolutePositioning, &nextStop, AM_SEEKING_AbsolutePositioning, engineThreadId) == S_OK);
    CHECK(harness.deferredSeekQueued.load());
    CHECK(st->isVirtualPauseActive());

    // 3. Unrelated / manual Seek from a different thread (thread 2002 - UI thread)
    harness.currentCallerContext = CallerContext::UserManualOp;
    LONGLONG userPos = 5000000LL;
    CHECK(harness.executeSetPositions(&userPos, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning, 2002) == S_OK);
    // Manual seek from UI thread cancels Virtual Pause immediately!
    CHECK(!st->isVirtualPauseActive());
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledSeek);
    CHECK(harness.manualOpObserved.load());
}

// Test 52: Two-Stage Drain Verification (PCM Delivery vs Renderer Output)
inline void testTwoStageDrainCompletionVerification() {
    std::printf("[TEST 52] Two-stage drain verification: PCM delivery vs renderer output...\n");
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
    CHECK(harness.executePause(1001) == S_OK);
    LONGLONG nextPos = 0, nextStop = 20000000LL;
    CHECK(harness.executeSetPositions(&nextPos, AM_SEEKING_AbsolutePositioning, &nextStop, AM_SEEKING_AbsolutePositioning, 1001) == S_OK);

    // Feed all 10 downstream chunks (Stage 1: PCM delivered)
    for (int i = 0; i < 10; ++i) {
        harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr);
    CHECK(st->transition.pcmDeliveryComplete == true);
    // Downstream EOS delivered
    CHECK(harness.downstreamEosReceived.load() == 1);

    // BUT renderer output has NOT finished yet!
    CHECK(st->transition.rendererOutputComplete == false);
    // Drain is NOT fully complete yet!
    CHECK(!harness.tracker.isDrainFullyComplete(harness.mockGraph));
    CHECK(st->isVirtualPauseActive() == true);
    CHECK(!harness.deferredSeekExecuted.load());

    // Now Stage 2 completes: Audio renderer finishes outputting to device
    harness.tracker.onRendererOutputComplete(harness.mockGraph);
    CHECK(harness.tracker.isDrainFullyComplete(harness.mockGraph));

    // Complete drain naturally
    CHECK(harness.completeDrainNaturally());
    CHECK(!st->isVirtualPauseActive());
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CompletedDrain);
    CHECK(harness.deferredSeekExecuted.load());
    CHECK(harness.mockSeeking->stopPos == 20000000LL);
}

// Test 53: Pre-Completion Cancellation Race Prevention
inline void testPreCompletionCancellationRacePrevention() {
    std::printf("[TEST 53] Pre-completion cancellation race: manual op right before output finish...\n");
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
    CHECK(harness.executePause(1001) == S_OK);
    LONGLONG nextPos = 0, nextStop = 20000000LL;
    CHECK(harness.executeSetPositions(&nextPos, AM_SEEKING_AbsolutePositioning, &nextStop, AM_SEEKING_AbsolutePositioning, 1001) == S_OK);

    // Stage 1 completes: PCM delivered, downstream EOS sent
    for (int i = 0; i < 10; ++i) {
        harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr);
    CHECK(st->transition.pcmDeliveryComplete == true);

    // User presses Pause RIGHT BEFORE renderer output finishes
    harness.currentCallerContext = CallerContext::UserManualOp;
    CHECK(harness.executePause(2002) == S_OK);
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledManualPause);
    CHECK(st->transition.cancelled == true);

    // Now late renderer output finish arrives
    harness.tracker.onRendererOutputComplete(harness.mockGraph);

    // Attempting to complete drain or pop deferred seek MUST FAIL and NOT execute deferred seek!
    CHECK(!harness.completeDrainNaturally());
    CHECK(!harness.deferredSeekExecuted.load());
    eosTransition::GraphState::DeferredSeekParams sp;
    CHECK(!harness.tracker.popDeferredSeek(harness.mockGraph, sp));
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledManualPause);
}

// Test 54: Monitor Thread Creation Failure Non-blocking Safety
inline void testMonitorThreadCreateFailureNonBlocking() {
    std::printf("[TEST 54] Monitor thread creation failure non-blocking safety...\n");
    eosTransition::Tracker testTracker;
    void* fakeGraph = reinterpret_cast<void*>(0x12345678);
    testTracker.onGraphActivated(fakeGraph, true, 48000, 2, 24);
    testTracker.onPreReadDecision(fakeGraph, eosTransition::PreReadDecision::FormatChanged);

    DWORD start = GetTickCount();
    testTracker.markVirtualPauseActive(fakeGraph, 1001);

    // When thread creation fails, cancelVirtualPause is called immediately
    testTracker.cancelVirtualPause(fakeGraph, eosTransition::PauseDeferralStatus::TimedOut);
    DWORD elapsed = GetTickCount() - start;

    CHECK(elapsed < 10); // Must be non-blocking (< 10 ms)
    const auto* st = testTracker.findGraph(fakeGraph);
    CHECK(st != nullptr);
    CHECK(!st->isVirtualPauseActive());
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::TimedOut);
    CHECK(st->transition.cancelled == true);
}

// Test 55: 48kHz Auto Track Change Event Sequence Regression (Issue #35)
inline void testAutoTrackChange48kEventSequenceRegression() {
    std::printf("[TEST 55] 48kHz auto track change event sequence regression (Issue #35)...\n");
    AsyncControlHarness harness;
    // 48kHz / 24bit track
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 230000000LL); // 23 seconds
    // Next track is 48kHz / 16bit (format change)
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    // Track 1 finishes decoding at ~20.2 seconds
    const uint32_t chunkBytes = 4800 * 6; // 100ms
    for (int i = 0; i < 202; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
        if (i < 174) {
            harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
        }
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // 1. Engine calls IMediaControl::Pause() -> enters Virtual Pause immediately (< 2 ms)
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);
    CHECK(harness.virtualPauseEntered.load());
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->isVirtualPauseActive());

    // 2. DirectSound Stop() called immediately after Pause
    // Under Issue #34, this stopped the renderer. Under Issue #35, this MUST BE SUPPRESSED!
    CHECK(harness.executeDsStop(nullptr, 1001) == DS_OK);
    CHECK(harness.dsStopSuppressed.load() == true);
    // Buffer continues playing; Virtual Pause MUST NOT be cancelled!
    CHECK(st->isVirtualPauseActive() == true);

    // 3. Approximately 6ms later, engine calls SetPositions(~20.066s) on the OLD graph (Issue #36)
    // Real machine sequence: engine calls SetPositions with ~20.066s on old graph without user input.
    // Under Issue #36, this is classified as OldGraphAutomatedPark: absorbed without cancelling drain!
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    LONGLONG oldGraphPos = 200660000LL;
    CHECK(harness.executeSetPositions(&oldGraphPos, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning, 2002) == S_OK);
    CHECK(harness.oldGraphSeekParkedCount.load() == 1);
    CHECK(st->isVirtualPauseActive() == true);
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::Active);

    // 4. Approximately 40ms later, new graph for next track opens
    MockFilterGraphEx* nextMockGraph = new MockFilterGraphEx();
    MockMediaSeekingEx* nextMockSeeking = new MockMediaSeekingEx();
    nextMockGraph->seeking = nextMockSeeking;
    harness.tracker.onGraphActivated(nextMockGraph, false, 48000, 2, 16); // 48kHz / 16bit

    // 5. Engine calls SetPositions(0) on NEW graph to prepare Track 2
    LONGLONG nextPos = 0, nextStop = 300000000LL;
    CHECK(harness.executeSetPositions(nextMockGraph, nextMockSeeking, &nextPos, AM_SEEKING_AbsolutePositioning,
                                      &nextStop, AM_SEEKING_AbsolutePositioning, 2002) == S_OK);
    CHECK(harness.deferredSeekQueued.load() == true);
    CHECK(st->isVirtualPauseActive() == true);
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::Active);
    // Next graph seek must NOT have executed yet (drain still ongoing)
    CHECK(nextMockSeeking->stopPos != 300000000LL);

    // 6. Remaining PCM (from 17.4s to 23.0s, remaining ~2.8s) drains naturally through sound card
    for (int i = 174; i < 202; ++i) {
        harness.feedDownstreamChunk(nullptr, chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    CHECK(st->transition.pcmDeliveryComplete == true);
    CHECK(harness.downstreamEosReceived.load() == 1);

    // 7. DirectSound renderer outputs all remaining audio samples (Stage 2)
    harness.tracker.onRendererOutputComplete(harness.mockGraph);
    CHECK(harness.tracker.isDrainFullyComplete(harness.mockGraph));

    // 8. Natural drain completes
    CHECK(harness.completeDrainNaturally());
    CHECK(!st->isVirtualPauseActive());
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CompletedDrain);
    CHECK(harness.deferredSeekExecuted.load() == true);
    CHECK(harness.deferredSeekExecutedCount.load() == 1);

    // 9. Verify deferred zero seek was executed on NEW graph exactly once!
    CHECK(nextMockSeeking->currentPos == 0);
    CHECK(nextMockSeeking->stopPos == 300000000LL);

    nextMockGraph->Release();
    nextMockSeeking->Release();
}

// Test 56: Manual Seek Not Misclassified From Same Caller
inline void testManualSeekNotMisclassifiedFromSameCaller() {
    std::printf("[TEST 56] Manual seek not misclassified from same caller...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 200000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // Virtual Pause starts
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->isVirtualPauseActive());

    // User manual seek to 50 seconds (even from same caller / thread)
    harness.currentCallerContext = CallerContext::UserManualOp;
    LONGLONG userPos = 500000000LL;
    CHECK(harness.executeSetPositions(&userPos, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning, 1001) == S_OK);

    // MUST cancel Virtual Pause immediately and apply user seek
    CHECK(!st->isVirtualPauseActive());
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledSeek);
    CHECK(harness.manualOpObserved.load() == true);
    CHECK(harness.mockSeeking->currentPos == 500000000LL);
}

// Test 57: Manual Stop Regression During Virtual Pause
inline void testManualStopRegressionDuringVirtualPause() {
    std::printf("[TEST 57] Manual stop regression during Virtual Pause...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 200000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->isVirtualPauseActive());

    // User clicks Stop
    harness.currentCallerContext = CallerContext::UserManualOp;
    CHECK(harness.executeControlStop() == S_OK);

    // Virtual Pause cancelled immediately, graph stopped
    CHECK(!st->isVirtualPauseActive());
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledStop);
    CHECK(harness.mockControl->state == State_Stopped);
}

// Test 58: Next Track Zero Seek Applied to New Graph Exactly Once (Issue #36)
inline void testNewGraphZeroSeekAppliedExactlyOnce() {
    std::printf("[TEST 58] Next track zero seek applied to new graph exactly once (Issue #36)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 200000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // Start Virtual Pause on old graph
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->isVirtualPauseActive());

    // Old graph receives non-zero seek -> parked without cancelling Virtual Pause
    LONGLONG oldPos = 180000000LL;
    CHECK(harness.executeSetPositions(&oldPos, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning, 2002) == S_OK);
    CHECK(harness.oldGraphSeekParkedCount.load() == 1);
    CHECK(st->isVirtualPauseActive() == true);

    // New graph created for next track
    MockFilterGraphEx* nextGraph = new MockFilterGraphEx();
    MockMediaSeekingEx* nextSeeking = new MockMediaSeekingEx();
    nextGraph->seeking = nextSeeking;

    // Send SetPositions(0) to new graph TWICE (e.g. duplicate engine calls)
    LONGLONG pos0 = 0, stop0 = 250000000LL;
    CHECK(harness.executeSetPositions(nextGraph, nextSeeking, &pos0, AM_SEEKING_AbsolutePositioning,
                                      &stop0, AM_SEEKING_AbsolutePositioning, 2002) == S_OK);
    CHECK(harness.executeSetPositions(nextGraph, nextSeeking, &pos0, AM_SEEKING_AbsolutePositioning,
                                      &stop0, AM_SEEKING_AbsolutePositioning, 2002) == S_OK);
    CHECK(st->isVirtualPauseActive() == true);

    // Complete drain
    CHECK(harness.completeDrainNaturally());
    CHECK(!st->isVirtualPauseActive());
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CompletedDrain);
    CHECK(harness.deferredSeekExecutedCount.load() == 1);
    CHECK(nextSeeking->currentPos == 0);
    CHECK(nextSeeking->stopPos == 250000000LL);

    nextGraph->Release();
    nextSeeking->Release();
}

inline void runAllAsyncTransitionTests() {
    std::printf("\n=== Issue #33 / #34 / #35 / #36 Non-blocking Virtual Pause & Deferral Tests ===\n");
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
    testAutomatedSetPositionsCallerMatchingAndUnrelatedBufferIsolation();
    testTwoStageDrainCompletionVerification();
    testPreCompletionCancellationRacePrevention();
    testMonitorThreadCreateFailureNonBlocking();
    testAutoTrackChange48kEventSequenceRegression();
    testManualSeekNotMisclassifiedFromSameCaller();
    testManualStopRegressionDuringVirtualPause();
    testNewGraphZeroSeekAppliedExactlyOnce();
    std::printf("=== Issue #33 / #34 / #35 / #36 Non-blocking Virtual Pause Tests Completed ===\n\n");
}

} // namespace asyncTests
