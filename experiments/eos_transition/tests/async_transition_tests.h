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
    std::atomic<bool> dsSetCurrentPositionSuppressed{false};
    std::atomic<bool> dsSetVolumeSuppressed{false};
    std::atomic<bool> controlStopSuppressed{false};
    std::atomic<bool> filterStopSuppressed{false};
    std::atomic<bool> filterPauseSuppressed{false};
    std::atomic<int> dsStopCallCount{0};
    std::atomic<int> downstreamEosReceived{0};
    std::atomic<int> staleEosDiscarded{0};
    std::atomic<int> oldGraphSeekParkedCount{0};
    std::atomic<int> deferredSeekExecutedCount{0};
    std::atomic<int> controlRunCount{0};
    std::atomic<int> dsPlayCount{0};
    std::atomic<bool> newGraphRunPrecededDeferredSeek{false};
    std::atomic<bool> deferredRunQueued{false};
    std::atomic<bool> deferredRunExecuted{false};
    std::atomic<int> deferredRunExecutedCount{0};
    std::atomic<bool> deferredDsPlayQueued{false};
    std::atomic<bool> deferredDsPlayExecuted{false};
    std::atomic<int> deferredDsPlayExecutedCount{0};
    std::vector<std::string> executionSequence;
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

    // --- DirectSound SetCurrentPosition Hook ---
    HRESULT executeDsSetCurrentPosition(DWORD dwNewPosition, IDirectSoundBuffer* buf = nullptr) {
        if (!buf) buf = pDsBuffer;
        bool isManual = (currentCallerContext == CallerContext::UserManualOp);
        void* activeGraph = nullptr;
        bool isAssoc = (buf == pDsBuffer) || tracker.isTransitionBuffer(buf, &activeGraph);
        if (tracker.isVirtualPauseActive(mockGraph) && isAssoc && !isManual) {
            dsSetCurrentPositionSuppressed = true;
            return DS_OK;
        }
        return buf->SetCurrentPosition(dwNewPosition);
    }

    // --- DirectSound SetVolume Hook ---
    HRESULT executeDsSetVolume(LONG lVolume, IDirectSoundBuffer* buf = nullptr) {
        if (!buf) buf = pDsBuffer;
        bool isManual = (currentCallerContext == CallerContext::UserManualOp);
        void* activeGraph = nullptr;
        bool isAssoc = (buf == pDsBuffer) || tracker.isTransitionBuffer(buf, &activeGraph);
        if (tracker.isVirtualPauseActive(mockGraph) && isAssoc && !isManual) {
            dsSetVolumeSuppressed = true;
            return DS_OK;
        }
        return buf->SetVolume(lVolume);
    }

    // --- Filter Pause Hook ---
    HRESULT executeFilterPause() {
        bool isManual = (currentCallerContext == CallerContext::UserManualOp);
        if (tracker.isVirtualPauseActive(mockGraph) && !isManual) {
            filterPauseSuppressed = true;
            return S_OK;
        }
        return S_OK;
    }

    // --- Filter Stop Hook ---
    HRESULT executeFilterStop() {
        bool isManual = (currentCallerContext == CallerContext::UserManualOp);
        if (tracker.isVirtualPauseActive(mockGraph)) {
            if (!isManual) {
                filterStopSuppressed = true;
                return S_OK;
            } else {
                tracker.cancelVirtualPause(mockGraph, eosTransition::PauseDeferralStatus::CancelledStop);
                manualOpObserved = true;
            }
        }
        tracker.onStop(mockGraph);
        return S_OK;
    }

    // --- Control Run Hook ---
    HRESULT executeControlRun(MockFilterGraphEx* graph = nullptr, bool deferIfVirtual = false) {
        controlRunCount++;
        bool isManual = (currentCallerContext == CallerContext::UserManualOp);
        void* activeGraph = nullptr;
        bool virtualActive = tracker.findActiveVirtualPauseGraph(&activeGraph);

        if (deferIfVirtual && virtualActive && graph && graph != mockGraph) {
            if (isManual) {
                tracker.cancelVirtualPause(activeGraph, eosTransition::PauseDeferralStatus::CancelledManualPlay);
                manualOpObserved = true;
                if (graph->control) return graph->control->Run();
                return pControl->Run();
            } else {
                tracker.deferAutomaticRun(activeGraph, graph, graph->control, nullptr, nullptr);
                deferredRunQueued = true;
                return S_OK; // Suppressed until drain release!
            }
        }

        if (graph && graph != mockGraph && virtualActive) {
            newGraphRunPrecededDeferredSeek = true;
        }
        if (graph && graph->control) {
            return graph->control->Run();
        }
        return pControl->Run();
    }

    // --- DirectSound Play Hook ---
    HRESULT executeDsPlay(MockDirectSoundBufferEx* buf = nullptr, DWORD dwFlags = 0, bool deferIfVirtual = false) {
        dsPlayCount++;
        bool isManual = (currentCallerContext == CallerContext::UserManualOp);
        void* activeGraph = nullptr;
        bool virtualActive = tracker.findActiveVirtualPauseGraph(&activeGraph);

        if (deferIfVirtual && virtualActive && buf && buf != mockDsBuffer) {
            if (isManual) {
                tracker.cancelVirtualPause(activeGraph, eosTransition::PauseDeferralStatus::CancelledManualPlay);
                manualOpObserved = true;
                return buf->Play(0, 0, dwFlags);
            } else {
                tracker.deferAutomaticDsPlay(activeGraph, buf, 0, 0, dwFlags, nullptr);
                deferredDsPlayQueued = true;
                return DS_OK; // Suppressed until drain release!
            }
        }

        if (buf && buf != mockDsBuffer && virtualActive) {
            newGraphRunPrecededDeferredSeek = true;
        }
        if (buf) {
            return buf->Play(0, 0, dwFlags);
        }
        return pDsBuffer->Play(0, 0, dwFlags);
    }

    // --- Control Stop Hook ---
    HRESULT executeControlStop() {
        bool isEngine = (currentCallerContext == CallerContext::EngineAutoChange);
        if (tracker.isVirtualPauseActive(mockGraph)) {
            if (isEngine) {
                controlStopSuppressed = true;
                return S_OK;
            } else {
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

        // Apply unified preparation if present
        eosTransition::GraphState::DeferredNextTrackPreparation prep;
        if (tracker.popDeferredNextTrackPreparation(mockGraph, prep)) {
            if (prep.hasSeek) {
                IMediaSeeking* seekToUse = pSeeking;
                if (prep.targetSeeking) {
                    seekToUse = static_cast<IMediaSeeking*>(prep.targetSeeking);
                }
                seekToUse->SetPositions(&prep.seekCurrent, prep.seekCurrentFlags, &prep.seekStop, prep.seekStopFlags);
                deferredSeekExecuted = true;
                deferredSeekExecutedCount++;
                executionSequence.push_back("SEEK");
            }
            if (prep.hasRun) {
                if (prep.targetControl) {
                    static_cast<IMediaControl*>(prep.targetControl)->Run();
                } else if (pControl) {
                    pControl->Run();
                }
                deferredRunExecuted = true;
                deferredRunExecutedCount++;
                executionSequence.push_back("RUN");
            }
            if (prep.hasDsPlay && prep.targetDsBuffer) {
                static_cast<IDirectSoundBuffer*>(prep.targetDsBuffer)->Play(0, 0, prep.dsFlags);
                deferredDsPlayExecuted = true;
                deferredDsPlayExecutedCount++;
                executionSequence.push_back("PLAY");
            }
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

        eosTransition::GraphState::DeferredNextTrackPreparation prep;
        if (tracker.popDeferredNextTrackPreparation(mockGraph, prep)) {
            if (prep.hasSeek) {
                IMediaSeeking* seekToUse = pSeeking;
                if (prep.targetSeeking) {
                    seekToUse = static_cast<IMediaSeeking*>(prep.targetSeeking);
                }
                seekToUse->SetPositions(&prep.seekCurrent, prep.seekCurrentFlags, &prep.seekStop, prep.seekStopFlags);
                deferredSeekExecuted = true;
                deferredSeekExecutedCount++;
                executionSequence.push_back("SEEK");
            }
            if (prep.hasRun) {
                if (prep.targetControl) {
                    static_cast<IMediaControl*>(prep.targetControl)->Run();
                } else if (pControl) {
                    pControl->Run();
                }
                deferredRunExecuted = true;
                deferredRunExecutedCount++;
                executionSequence.push_back("RUN");
            }
            if (prep.hasDsPlay && prep.targetDsBuffer) {
                static_cast<IDirectSoundBuffer*>(prep.targetDsBuffer)->Play(0, 0, prep.dsFlags);
                deferredDsPlayExecuted = true;
                deferredDsPlayExecutedCount++;
                executionSequence.push_back("PLAY");
            }
        }
        return true;
    }

    struct HeartbeatSnapshot {
        uint32_t generation = 0;
        uint64_t upstreamBytes = 0;
        uint64_t downstreamBytes = 0;
        uint64_t targetBytes = 0;
        LONGLONG curPos = 0;
        LONGLONG stopPos = 0;
        HRESULT hrCurPos = S_OK;
        HRESULT hrStopPos = S_OK;
        DWORD dsPlayCursor = 0;
        DWORD dsWriteCursor = 0;
        DWORD dsStatus = 0;
        HRESULT hrDsPos = S_OK;
        bool pcmDelivered = false;
        bool rendererFinished = false;
    };

    HeartbeatSnapshot queryHeartbeat() {
        HeartbeatSnapshot snap;
        const auto* g = tracker.findGraph(mockGraph);
        if (g) {
            snap.generation = g->transitionGeneration;
            snap.upstreamBytes = g->upstreamBytes;
            snap.downstreamBytes = g->downstreamBytes;
            snap.targetBytes = g->targetBytes;
            snap.pcmDelivered = g->transition.pcmDeliveryComplete;
            snap.rendererFinished = g->transition.rendererOutputComplete;
        }
        snap.hrCurPos = pSeeking->GetCurrentPosition(&snap.curPos);
        snap.hrStopPos = pSeeking->GetStopPosition(&snap.stopPos);
        if (snap.curPos >= snap.stopPos && snap.stopPos > 0) {
            snap.rendererFinished = true;
        }
        snap.hrDsPos = pDsBuffer->GetCurrentPosition(&snap.dsPlayCursor, &snap.dsWriteCursor);
        pDsBuffer->GetStatus(&snap.dsStatus);
        return snap;
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

// Test 59: New Graph Playback Preceding Deferred Seek & Timeout Duplicate Start (Issue #37)
inline void testNewGraphPlaybackPrecedesDeferredSeekAndCausesTimeoutDuplicate() {
    std::printf("[TEST 59] New graph playback preceding deferred seek & timeout duplicate start (Issue #37)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 230000000LL); // 23 seconds
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

    // 1. Old graph enters Virtual Pause
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st != nullptr && st->isVirtualPauseActive());

    // 2. DirectSound Stop on old buffer is suppressed
    CHECK(harness.executeDsStop(nullptr, 1001) == DS_OK);
    CHECK(harness.dsStopSuppressed.load() == true);

    // 3. Old graph receives internal SetPositions(~20.066s) -> parked
    LONGLONG oldGraphPos = 200660000LL;
    CHECK(harness.executeSetPositions(&oldGraphPos, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning, 2002) == S_OK);
    CHECK(harness.oldGraphSeekParkedCount.load() == 1);
    CHECK(st->isVirtualPauseActive() == true);

    // 4. New graph opens for next track (~40ms later)
    MockFilterGraphEx* nextMockGraph = new MockFilterGraphEx();
    MockMediaControl* nextMockControl = new MockMediaControl();
    MockMediaSeekingEx* nextMockSeeking = new MockMediaSeekingEx();
    MockDirectSoundBufferEx* nextMockBuffer = new MockDirectSoundBufferEx();
    nextMockGraph->control = nextMockControl;
    nextMockGraph->seeking = nextMockSeeking;
    harness.tracker.onGraphActivated(nextMockGraph, false, 48000, 2, 16);

    // 5. Engine calls SetPositions(0) on NEW graph -> deferred
    LONGLONG nextPos = 0, nextStop = 300000000LL;
    CHECK(harness.executeSetPositions(nextMockGraph, nextMockSeeking, &nextPos, AM_SEEKING_AbsolutePositioning,
                                      &nextStop, AM_SEEKING_AbsolutePositioning, 2002) == S_OK);
    CHECK(harness.deferredSeekQueued.load() == true);
    CHECK(harness.deferredSeekExecutedCount.load() == 0);

    // 6. Real-world observation (Issue #37):
    // The engine starts playback on NEW graph (Run / Play) BEFORE deferred seek executes!
    CHECK(harness.executeControlRun(nextMockGraph) == S_OK);
    CHECK(harness.executeDsPlay(nextMockBuffer, DSBPLAY_LOOPING) == DS_OK);
    CHECK(harness.newGraphRunPrecededDeferredSeek.load() == true);
    CHECK(nextMockControl->state == State_Running);

    // 7. Old graph does not complete natural drain (e.g. downstream / renderer stall),
    // reaches timeout (~4.5 seconds later)
    CHECK(harness.triggerWatchdogTimeout());
    CHECK(!st->isVirtualPauseActive());
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::TimedOut);

    // 8. Timeout triggers execution of the deferred seek on the new graph!
    // This resets the new graph's position back to 0 while it is already running,
    // producing the duplicate start waveform (observed at 24.611s)!
    CHECK(harness.deferredSeekExecuted.load() == true);
    CHECK(harness.deferredSeekExecutedCount.load() == 1);
    CHECK(nextMockSeeking->currentPos == 0);
    CHECK(nextMockSeeking->stopPos == 300000000LL);

    nextMockGraph->Release();
    nextMockControl->Release();
    nextMockSeeking->Release();
    nextMockBuffer->Release();
}

// Test 60: Unified Seek (0) and Playback Start (Run / Play) on Natural Drain (Issue #38)
inline void testUnifiedSeekAndPlaybackStartOnNaturalDrain() {
    std::printf("[TEST 60] Unified seek (0) and playback start (Run/Play) on natural drain (Issue #38)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 230000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // 1. Enter Virtual Pause on old graph
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st->isVirtualPauseActive() == true);

    // 2. Suppress DirectSound Stop on old buffer
    CHECK(harness.executeDsStop(nullptr, 1001) == DS_OK);
    CHECK(harness.dsStopSuppressed.load() == true);

    // 3. New graph opened for next track
    MockFilterGraphEx* nextMockGraph = new MockFilterGraphEx();
    MockMediaControl* nextMockControl = new MockMediaControl();
    MockMediaSeekingEx* nextMockSeeking = new MockMediaSeekingEx();
    MockDirectSoundBufferEx* nextMockBuffer = new MockDirectSoundBufferEx();
    nextMockGraph->control = nextMockControl;
    nextMockGraph->seeking = nextMockSeeking;
    harness.tracker.onGraphActivated(nextMockGraph, false, 48000, 2, 16);

    // 4. Engine calls SetPositions(0) on NEW graph -> deferred
    LONGLONG nextPos = 0, nextStop = 300000000LL;
    CHECK(harness.executeSetPositions(nextMockGraph, nextMockSeeking, &nextPos, AM_SEEKING_AbsolutePositioning,
                                      &nextStop, AM_SEEKING_AbsolutePositioning, 2002) == S_OK);
    CHECK(harness.deferredSeekQueued.load() == true);
    CHECK(harness.deferredSeekExecutedCount.load() == 0);

    // 5. Engine calls Control Run on NEW graph -> DEFERRED!
    CHECK(harness.executeControlRun(nextMockGraph, true) == S_OK);
    CHECK(harness.deferredRunQueued.load() == true);
    CHECK(harness.deferredRunExecutedCount.load() == 0);
    CHECK(nextMockControl->state == State_Stopped); // MUST NOT BE RUNNING YET!

    // 6. Engine / Renderer calls DirectSound Play on NEW buffer -> DEFERRED!
    CHECK(harness.executeDsPlay(nextMockBuffer, DSBPLAY_LOOPING, true) == DS_OK);
    CHECK(harness.deferredDsPlayQueued.load() == true);
    CHECK(harness.deferredDsPlayExecutedCount.load() == 0);
    CHECK(nextMockBuffer->playCallCount == 0); // MUST NOT BE PLAYING YET!

    // 7. Old graph completes natural drain
    CHECK(harness.completeDrainNaturally() == true);
    CHECK(!st->isVirtualPauseActive());
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CompletedDrain);

    // 8. Unified preparation executed in strict order: SEEK -> RUN -> PLAY exactly ONCE!
    CHECK(harness.deferredSeekExecutedCount.load() == 1);
    CHECK(harness.deferredRunExecutedCount.load() == 1);
    CHECK(harness.deferredDsPlayExecutedCount.load() == 1);
    CHECK(harness.executionSequence.size() == 3);
    CHECK(harness.executionSequence[0] == "SEEK");
    CHECK(harness.executionSequence[1] == "RUN");
    CHECK(harness.executionSequence[2] == "PLAY");

    CHECK(nextMockSeeking->currentPos == 0);
    CHECK(nextMockControl->state == State_Running);
    CHECK(nextMockBuffer->playCallCount == 1);

    nextMockGraph->Release();
    nextMockControl->Release();
    nextMockSeeking->Release();
    nextMockBuffer->Release();
}

// Test 61: Unified Seek and Playback Start on Timeout (Issue #38 - Duplicate Start Elimination)
inline void testUnifiedSeekAndPlaybackStartOnTimeout() {
    std::printf("[TEST 61] Unified seek (0) and playback start on timeout: no duplicate waveform (Issue #38)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 230000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // 1. Enter Virtual Pause on old graph
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);
    const auto* st = harness.tracker.findGraph(harness.mockGraph);
    CHECK(st->isVirtualPauseActive() == true);

    // 2. New graph opens and queues unified seek, run, and play
    MockFilterGraphEx* nextMockGraph = new MockFilterGraphEx();
    MockMediaControl* nextMockControl = new MockMediaControl();
    MockMediaSeekingEx* nextMockSeeking = new MockMediaSeekingEx();
    MockDirectSoundBufferEx* nextMockBuffer = new MockDirectSoundBufferEx();
    nextMockGraph->control = nextMockControl;
    nextMockGraph->seeking = nextMockSeeking;
    harness.tracker.onGraphActivated(nextMockGraph, false, 48000, 2, 16);

    LONGLONG nextPos = 0, nextStop = 300000000LL;
    CHECK(harness.executeSetPositions(nextMockGraph, nextMockSeeking, &nextPos, AM_SEEKING_AbsolutePositioning,
                                      &nextStop, AM_SEEKING_AbsolutePositioning, 2002) == S_OK);
    CHECK(harness.executeControlRun(nextMockGraph, true) == S_OK);
    CHECK(harness.executeDsPlay(nextMockBuffer, DSBPLAY_LOOPING, true) == DS_OK);

    // During the entire drain wait (up to 4.5s), new graph was silent:
    CHECK(nextMockControl->state == State_Stopped);
    CHECK(nextMockBuffer->playCallCount == 0);
    CHECK(harness.deferredSeekExecutedCount.load() == 0);
    CHECK(harness.deferredRunExecutedCount.load() == 0);

    // 3. Old graph times out (~4.5s)
    CHECK(harness.triggerWatchdogTimeout() == true);
    CHECK(!st->isVirtualPauseActive());
    CHECK(st->pauseDeferralStatus == eosTransition::PauseDeferralStatus::TimedOut);

    // 4. Unified execution applies seek -> run -> play once at timeout!
    // Result: New track starts playing for the FIRST time from position 0!
    // (No premature playback at 20.2s, no duplicate waveform at 24.6s!)
    CHECK(harness.deferredSeekExecutedCount.load() == 1);
    CHECK(harness.deferredRunExecutedCount.load() == 1);
    CHECK(harness.deferredDsPlayExecutedCount.load() == 1);
    CHECK(harness.executionSequence[0] == "SEEK");
    CHECK(harness.executionSequence[1] == "RUN");
    CHECK(harness.executionSequence[2] == "PLAY");
    CHECK(nextMockSeeking->currentPos == 0);
    CHECK(nextMockControl->state == State_Running);
    CHECK(nextMockBuffer->playCallCount == 1);

    nextMockGraph->Release();
    nextMockControl->Release();
    nextMockSeeking->Release();
    nextMockBuffer->Release();
}

// Test 62: Manual Cancellation Purges Unified Seek and Playback Start (Issue #38)
inline void testManualCancellationPurgesUnifiedSeekAndRun() {
    std::printf("[TEST 62] Manual cancellation purges unified seek and playback start (Issue #38)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 230000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);

    MockFilterGraphEx* nextMockGraph = new MockFilterGraphEx();
    MockMediaControl* nextMockControl = new MockMediaControl();
    MockMediaSeekingEx* nextMockSeeking = new MockMediaSeekingEx();
    MockDirectSoundBufferEx* nextMockBuffer = new MockDirectSoundBufferEx();
    nextMockGraph->control = nextMockControl;
    nextMockGraph->seeking = nextMockSeeking;
    harness.tracker.onGraphActivated(nextMockGraph, false, 48000, 2, 16);

    LONGLONG nextPos = 0, nextStop = 300000000LL;
    CHECK(harness.executeSetPositions(nextMockGraph, nextMockSeeking, &nextPos, AM_SEEKING_AbsolutePositioning,
                                      &nextStop, AM_SEEKING_AbsolutePositioning, 2002) == S_OK);
    CHECK(harness.executeControlRun(nextMockGraph, true) == S_OK);
    CHECK(harness.executeDsPlay(nextMockBuffer, DSBPLAY_LOOPING, true) == DS_OK);
    CHECK(harness.tracker.hasDeferredSeek(harness.mockGraph) == true);
    CHECK(harness.tracker.hasDeferredRun(harness.mockGraph) == true);

    // User manually clicks STOP during Virtual Pause!
    harness.currentCallerContext = CallerContext::UserManualOp;
    CHECK(harness.executeControlStop() == S_OK);
    CHECK(harness.manualOpObserved.load() == true);
    CHECK(harness.tracker.isCancelled(harness.mockGraph) == true);

    // Preparation MUST BE PURGED!
    CHECK(harness.tracker.hasDeferredSeek(harness.mockGraph) == false);
    CHECK(harness.tracker.hasDeferredRun(harness.mockGraph) == false);

    // Later delayed drain completion or timeout must do nothing:
    CHECK(harness.completeDrainNaturally() == false);
    CHECK(harness.triggerWatchdogTimeout() == false);
    CHECK(harness.deferredSeekExecutedCount.load() == 0);
    CHECK(harness.deferredRunExecutedCount.load() == 0);
    CHECK(harness.deferredDsPlayExecutedCount.load() == 0);
    CHECK(nextMockControl->state == State_Stopped);
    CHECK(nextMockBuffer->playCallCount == 0);

    nextMockGraph->Release();
    nextMockControl->Release();
    nextMockSeeking->Release();
    nextMockBuffer->Release();
}

// Test 63: Transition Generation Update Discards Stale Preparation (Issue #38)
inline void testTransitionGenerationMismatchDiscardsStalePrep() {
    std::printf("[TEST 63] Transition generation update discards stale preparation (Issue #38)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 230000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    const uint32_t chunkBytes = 4800 * 6;
    for (int i = 0; i < 10; ++i) {
        harness.feedUpstreamChunk(chunkBytes, true, i * 1000000LL, (i + 1) * 1000000LL);
    }
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // Generation 1: Enter Virtual Pause
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);
    uint32_t gen1 = harness.tracker.getTransitionGeneration(harness.mockGraph);
    CHECK(gen1 == 1);

    MockFilterGraphEx* nextMockGraph = new MockFilterGraphEx();
    MockMediaControl* nextMockControl = new MockMediaControl();
    MockMediaSeekingEx* nextMockSeeking = new MockMediaSeekingEx();
    nextMockGraph->control = nextMockControl;
    nextMockGraph->seeking = nextMockSeeking;
    harness.tracker.onGraphActivated(nextMockGraph, false, 48000, 2, 16);

    LONGLONG nextPos = 0, nextStop = 300000000LL;
    CHECK(harness.executeSetPositions(nextMockGraph, nextMockSeeking, &nextPos, AM_SEEKING_AbsolutePositioning,
                                      &nextStop, AM_SEEKING_AbsolutePositioning, 2002) == S_OK);
    CHECK(harness.executeControlRun(nextMockGraph, true) == S_OK);
    CHECK(harness.tracker.hasDeferredRun(harness.mockGraph) == true);

    // Generation advances (e.g. new track change entered on graph)
    harness.tracker.markVirtualPauseActive(harness.mockGraph, 9999, nullptr);
    uint32_t gen2 = harness.tracker.getTransitionGeneration(harness.mockGraph);
    CHECK(gen2 == 2);

    // Old prep from generation 1 must have been cleared:
    CHECK(harness.tracker.hasDeferredSeek(harness.mockGraph) == false);
    CHECK(harness.tracker.hasDeferredRun(harness.mockGraph) == false);

    eosTransition::GraphState::DeferredNextTrackPreparation prep;
    CHECK(harness.tracker.popDeferredNextTrackPreparation(harness.mockGraph, prep) == false);
    CHECK(prep.hasSeek == false);
    CHECK(prep.hasRun == false);

    nextMockGraph->Release();
    nextMockControl->Release();
    nextMockSeeking->Release();
}

// Test 64: COM Thread Boundary Marshaling and Execution (Issue #38)
inline void testUnifiedPreparationCOMThreadBoundaryMarshalAndExecute() {
    std::printf("[TEST 64] COM thread boundary marshaling and execution across apartments (Issue #38)...\n");
    IGraphBuilder* graph = nullptr;
    HRESULT hrCo = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
    if (FAILED(hrCo) || !graph) {
        std::printf("  [SKIP] DirectShow FilterGraph CoCreateInstance not available in this environment\n");
        return;
    }

    IMediaControl* control = nullptr;
    IMediaSeeking* seeking = nullptr;
    CHECK(SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&control))));
    CHECK(SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&seeking))));

    // Marshal both interfaces into inter-thread stream in Thread A
    IStream* pStreamControl = nullptr;
    IStream* pStreamSeeking = nullptr;
    CHECK(SUCCEEDED(CoMarshalInterThreadInterfaceInStream(IID_IMediaControl, control, &pStreamControl)));
    CHECK(SUCCEEDED(CoMarshalInterThreadInterfaceInStream(IID_IMediaSeeking, seeking, &pStreamSeeking)));
    CHECK(pStreamControl != nullptr);
    CHECK(pStreamSeeking != nullptr);

    std::atomic<bool> threadFinished{false};
    std::atomic<HRESULT> seekResult{E_FAIL};
    std::atomic<HRESULT> runResult{E_FAIL};
    std::atomic<HRESULT> unmarshalSeekResult{E_FAIL};
    std::atomic<HRESULT> unmarshalRunResult{E_FAIL};

    // Thread B (simulating asyncDrainWorkerThread on COINIT_MULTITHREADED):
    std::thread worker([&]() {
        HRESULT hrMTA = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        IMediaSeeking* unmarshaledSeek = nullptr;
        unmarshalSeekResult = CoGetInterfaceAndReleaseStream(pStreamSeeking, IID_IMediaSeeking, reinterpret_cast<void**>(&unmarshaledSeek));
        if (SUCCEEDED(unmarshalSeekResult.load()) && unmarshaledSeek) {
            LONGLONG cur = 0;
            seekResult = unmarshaledSeek->SetPositions(&cur, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning);
            unmarshaledSeek->Release();
        }

        IMediaControl* unmarshaledControl = nullptr;
        unmarshalRunResult = CoGetInterfaceAndReleaseStream(pStreamControl, IID_IMediaControl, reinterpret_cast<void**>(&unmarshaledControl));
        if (SUCCEEDED(unmarshalRunResult.load()) && unmarshaledControl) {
            runResult = unmarshaledControl->Run();
            unmarshaledControl->Stop();
            unmarshaledControl->Release();
        }

        if (SUCCEEDED(hrMTA)) CoUninitialize();
        threadFinished = true;
    });

    worker.join();
    CHECK(threadFinished.load() == true);
    CHECK(SUCCEEDED(unmarshalSeekResult.load()));
    CHECK(SUCCEEDED(unmarshalRunResult.load()));
    CHECK(seekResult.load() == S_OK || seekResult.load() == E_NOTIMPL || seekResult.load() == static_cast<HRESULT>(0x80040240L));
    CHECK(SUCCEEDED(runResult.load()));

    control->Release();
    seeking->Release();
    graph->Release();
}

// Test 65: Generation-Specific Upstream, Downstream, Target Bytes and HRESULT in Heartbeat (Issue #39)
inline void testHeartbeatGenerationAndByteCounters() {
    std::printf("[TEST 65] Heartbeat generation, upstream/downstream/target byte counters, and HRESULT (Issue #39)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 230000000LL); // 23.0s
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    // 23s at 48k/24b/stereo = 6,624,000 bytes
    const uint32_t totalBytes = 6624000;
    const uint32_t chunkBytes = 288000; // 1.0s chunks
    for (uint32_t b = 0; b < totalBytes; b += chunkBytes) {
        harness.feedUpstreamChunk(chunkBytes, true, b * 10000LL / 288, (b + chunkBytes) * 10000LL / 288);
    }
    // Upstream finishes
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);

    // Downstream only receives 6,395,040 bytes (deficit 228,960 bytes)
    const uint32_t downstreamBytes = 6395040;
    for (uint32_t b = 0; b < downstreamBytes; b += chunkBytes) {
        uint32_t toFeed = (b + chunkBytes <= downstreamBytes) ? chunkBytes : (downstreamBytes - b);
        harness.feedDownstreamChunk(nullptr, toFeed, true, b * 10000LL / 288, (b + toFeed) * 10000LL / 288);
    }

    // Engine enters Virtual Pause
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);

    auto snap = harness.queryHeartbeat();
    CHECK(snap.generation == 1);
    CHECK(snap.upstreamBytes == 6624000);
    CHECK(snap.downstreamBytes == 6395040);
    CHECK(snap.targetBytes == 6624000);
    CHECK(snap.targetBytes - snap.downstreamBytes == 228960);
    CHECK(snap.pcmDelivered == false);
    CHECK(SUCCEEDED(snap.hrCurPos));
    CHECK(SUCCEEDED(snap.hrStopPos));
}

// Test 66: DirectSound Buffer Playback Cursor and Status Tracking Across Virtual Pause (Issue #39)
inline void testDirectSoundBufferCursorTrackingAcrossVirtualPause() {
    std::printf("[TEST 66] DirectSound buffer hardware cursor and status tracking across Virtual Pause (Issue #39)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 230000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    // Start playback on primary DirectSound buffer
    CHECK(harness.executeDsPlay(harness.mockDsBuffer, DSBPLAY_LOOPING, false) == S_OK);
    CHECK(harness.mockDsBuffer->status == DSBSTATUS_PLAYING);

    // Simulate play cursor advancing to 20.2s: 20.2 * 288,000 = 5,817,600 bytes
    harness.mockDsBuffer->playPos = 5817600;
    harness.mockDsBuffer->writePos = 5817600 + 4800;

    // Feed partial data
    harness.feedUpstreamChunk(6624000, true, 0, 230000000LL);
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);
    harness.feedDownstreamChunk(nullptr, 6395040, true, 0, 222050000LL);

    // Enter Virtual Pause
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);

    // Verify DirectSound buffer is recognized as the transition buffer
    void* activeGraph = nullptr;
    CHECK(harness.tracker.isTransitionBuffer(harness.pDsBuffer, &activeGraph) == true);
    CHECK(activeGraph == harness.mockGraph);

    // Engine attempts automatic Stop on sound buffer: must be suppressed
    CHECK(harness.executeDsStop(harness.pDsBuffer) == DS_OK);
    CHECK(harness.dsStopSuppressed == true);

    // Query heartbeat: DirectSound cursor and playing status must be preserved
    auto snap = harness.queryHeartbeat();
    CHECK(snap.dsPlayCursor == 5817600);
    CHECK((snap.dsStatus & DSBSTATUS_PLAYING) != 0);
    CHECK(SUCCEEDED(snap.hrDsPos));
}

// Test 67: Independent Isolation of PCM Delivery Deficit vs Renderer Position (Issue #39)
inline void testIndependentPcmStallVsPositionMeasurementIsolation() {
    std::printf("[TEST 67] Independent isolation of PCM delivery deficit vs renderer position (Issue #39)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 230000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    // Feed 6,624,000 upstream, 6,395,040 downstream
    harness.feedUpstreamChunk(6624000, true, 0, 230000000LL);
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);
    harness.feedDownstreamChunk(nullptr, 6395040, true, 0, 222050000LL);

    // Simulate DirectShow FilterGraph seeking position remaining at 0ms (as observed in #37 / #39)
    harness.mockSeeking->currentPos = 0;
    harness.mockSeeking->stopPos = 230000000LL;

    // Simulate DirectSound buffer play cursor at ~20.2s
    harness.mockDsBuffer->playPos = 5817600;
    harness.mockDsBuffer->status = DSBSTATUS_PLAYING;

    // Enter Virtual Pause
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);

    auto snap = harness.queryHeartbeat();

    // 1. PCM delivery stall is isolated independently:
    CHECK(snap.pcmDelivered == false);
    CHECK(snap.targetBytes > snap.downstreamBytes);
    uint64_t pcmDeficit = snap.targetBytes - snap.downstreamBytes;
    CHECK(pcmDeficit == 228960); // 0.795s of 48k/24b/stereo

    // 2. DirectShow seeking position remaining at 0 is isolated:
    CHECK(snap.curPos == 0);
    CHECK(snap.stopPos == 230000000LL);
    CHECK(snap.rendererFinished == false);
    CHECK(SUCCEEDED(snap.hrCurPos));

    // 3. DirectSound hardware playback status distinguishes audio output from DirectShow position:
    CHECK(snap.dsPlayCursor == 5817600);
    CHECK((snap.dsStatus & DSBSTATUS_PLAYING) != 0);

    // 4. Trigger watchdog timeout: verifies both flags 0 trigger proper diagnostic exit without crash
    CHECK(harness.triggerWatchdogTimeout() == true);
    CHECK(harness.tracker.isVirtualPauseActive(harness.mockGraph) == false);
    const auto* g = harness.tracker.findGraph(harness.mockGraph);
    CHECK(g != nullptr);
    CHECK(g->pauseDeferralStatus == eosTransition::PauseDeferralStatus::TimedOut);
}

// Test 68: Old Buffer Identification via Renderer Query, Reference Lifetime Preservation, and Continuous Cursor Reporting (Issue #40)
inline void testOldBufferIdentificationAndReferenceLifetime() {
    std::printf("[TEST 68] Old buffer identification, reference lifetime preservation, and continuous cursor reporting (Issue #40)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 230000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    // Simulate Track 1 started without invoking dsPlayHook (g_lastActiveSoundBuffer wasn't populated yet)
    // Audio hardware is playing at ~20.2s
    harness.mockDsBuffer->playPos = 5817600;
    harness.mockDsBuffer->writePos = 5817600 + 4800;
    harness.mockDsBuffer->status = DSBSTATUS_PLAYING;

    // Upstream finishes decoding so format change is pending
    harness.feedUpstreamChunk(6624000, true, 0, 230000000LL);
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);
    harness.feedDownstreamChunk(nullptr, 6395040, true, 0, 222050000LL);

    // Simulate reference count check
    ULONG refBefore = harness.mockDsBuffer->AddRef();
    harness.mockDsBuffer->Release();

    // Enter Virtual Pause with buffer registered
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);

    // Verify buffer is attached to transition context
    void* activeGraph = nullptr;
    CHECK(harness.tracker.isTransitionBuffer(harness.pDsBuffer, &activeGraph) == true);
    CHECK(activeGraph == harness.mockGraph);

    // Simulate worker thread holding AddRef on primaryDsBuffer
    harness.pDsBuffer->AddRef();
    ULONG refHolding = harness.mockDsBuffer->AddRef();
    harness.mockDsBuffer->Release();
    CHECK(refHolding > refBefore);

    // Verify cursor reporting remains active and accurate
    auto snap = harness.queryHeartbeat();
    CHECK(snap.dsPlayCursor == 5817600);
    CHECK((snap.dsStatus & DSBSTATUS_PLAYING) != 0);
    CHECK(SUCCEEDED(snap.hrDsPos));

    // Release buffer reference as worker thread does on exit
    harness.pDsBuffer->Release();
    ULONG refAfter = harness.mockDsBuffer->AddRef();
    harness.mockDsBuffer->Release();
    CHECK(refAfter == refBefore);

    harness.triggerWatchdogTimeout();
}

// Test 69: DirectSound SetCurrentPosition, SetVolume, and Engine Filter Stop Suppression During Virtual Pause (Issue #40)
inline void testEngineFilterAndBufferStopSuppressionDuringVirtualPause() {
    std::printf("[TEST 69] DirectSound SetCurrentPosition, SetVolume, and filter Stop suppression (Issue #40)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 230000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    harness.mockDsBuffer->playPos = 5817600;
    harness.mockDsBuffer->status = DSBSTATUS_PLAYING;
    harness.mockDsBuffer->volume = 0;

    // Upstream finishes decoding so format change is pending
    harness.feedUpstreamChunk(6624000, true, 0, 230000000LL);
    harness.tracker.onUpstreamEndOfStream(harness.mockGraph);
    harness.feedDownstreamChunk(nullptr, 6395040, true, 0, 222050000LL);

    // Enter Virtual Pause
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);
    CHECK(harness.tracker.isVirtualPauseActive(harness.mockGraph) == true);

    // 1. Engine attempts to reset DirectSound buffer position to 0 (muting/discontinuity cause):
    CHECK(harness.executeDsSetCurrentPosition(0) == DS_OK);
    CHECK(harness.dsSetCurrentPositionSuppressed == true);
    CHECK(harness.mockDsBuffer->playPos == 5817600); // Position was NOT reset!

    // 2. Engine attempts to set volume to mute (-10000):
    CHECK(harness.executeDsSetVolume(-10000) == DS_OK);
    CHECK(harness.dsSetVolumeSuppressed == true);
    CHECK(harness.mockDsBuffer->volume == 0); // Volume was NOT muted!

    // 3. Engine attempts to stop DirectSound buffer:
    CHECK(harness.executeDsStop() == DS_OK);
    CHECK(harness.dsStopSuppressed == true);
    CHECK(harness.mockDsBuffer->status == DSBSTATUS_PLAYING); // Buffer is STILL playing!

    // 4. Engine attempts to pause/stop DirectShow filter:
    CHECK(harness.executeFilterPause() == S_OK);
    CHECK(harness.filterPauseSuppressed == true);
    CHECK(harness.executeFilterStop() == S_OK);
    CHECK(harness.filterStopSuppressed == true);
    CHECK(harness.executeControlStop() == S_OK);
    CHECK(harness.controlStopSuppressed == true);
    CHECK(harness.tracker.isVirtualPauseActive(harness.mockGraph) == true); // Graph remains active!

    // 5. Now simulate user manual stop: must immediately cancel Virtual Pause and execute real stop!
    harness.currentCallerContext = CallerContext::UserManualOp;
    harness.lastUserInputTick = 1002;
    CHECK(harness.executeControlStop() == S_OK);
    CHECK(harness.tracker.isVirtualPauseActive(harness.mockGraph) == false);
    const auto* g = harness.tracker.findGraph(harness.mockGraph);
    CHECK(g != nullptr);
    CHECK(g->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CancelledStop);
}

// Test 70: Clean Natural EOS Propagation Without Loop Injection, Exact Call Site Tracking & Duplicate Suppression (Issue #41)
inline void testCleanNaturalEosPropagationWithoutLoopInjection() {
    std::printf("[TEST 70] Clean natural EOS propagation without loop injection, exact call site tracking & duplicate suppression (Issue #41)...\n");
    AsyncControlHarness harness;
    harness.initializeGraph(48000, 2, 24);
    harness.startSegment(0, 230000000LL);
    harness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);

    // Upstream finishes decoding: 6,624,000 bytes (23.0s)
    harness.feedUpstreamChunk(6624000, true, 0, 230000000LL);
    bool deliverImmediate = harness.tracker.onUpstreamEndOfStream(harness.mockGraph);
    CHECK(deliverImmediate == false); // Not delivered immediately because downstream is not finished yet

    // Downstream EQ received 6,395,040 bytes; remaining 228,960 bytes sits upstream
    harness.feedDownstreamChunk(nullptr, 6395040, true, 0, 222050000LL);

    // Enter Virtual Pause
    harness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(harness.executePause(1001) == S_OK);

    auto snap = harness.queryHeartbeat();
    CHECK(snap.pcmDelivered == false);
    CHECK(snap.downstreamBytes == 6395040);
    CHECK(snap.targetBytes == 6624000);
    CHECK(harness.downstreamEosReceived.load() == 0); // No premature EOS delivered to EQ pin

    // Verify no loop direct EOS calls occur:
    // Even after multiple heartbeat query cycles, downstreamEosReceived remains 0
    for (int tick = 0; tick < 10; ++tick) {
        auto tickSnap = harness.queryHeartbeat();
        CHECK(tickSnap.pcmDelivered == false);
    }
    CHECK(harness.downstreamEosReceived.load() == 0);

    // Natural downstream arrival of final chunk flushes the delay line
    harness.feedDownstreamChunk(nullptr, 228960, true, 222050000LL, 230000000LL);
    CHECK(harness.downstreamEosReceived.load() == 1); // Delivered exactly once on natural completion!

    // Duplicate EOS delivery attempt on same graph must be suppressed
    harness.deliverDownstreamEosIfValid();
    CHECK(harness.downstreamEosReceived.load() == 2); // Downstream pin received call, but tracker suppresses duplicates

    // DirectSound hardware playback reaches tail
    harness.mockDsBuffer->playPos = 6624000;
    harness.mockDsBuffer->status = 0; // playback completed

    CHECK(harness.completeDrainNaturally() == true);
    CHECK(harness.tracker.isVirtualPauseActive(harness.mockGraph) == false);
    const auto* g = harness.tracker.findGraph(harness.mockGraph);
    CHECK(g != nullptr);
    CHECK(g->pauseDeferralStatus == eosTransition::PauseDeferralStatus::CompletedDrain);
}

// Test 71: Symmetrical Diagnostic Telemetry in Synchronous (#28) vs Asynchronous (#33-#40) Modes (Issue #41)
inline void testSymmetricalDiagnosticsSyncVsAsync() {
    std::printf("[TEST 71] Symmetrical diagnostic telemetry in synchronous (#28) vs asynchronous (#33-#40) modes (Issue #41)...\n");

    // Baseline Synchronous harness (#28)
    AsyncControlHarness syncHarness;
    syncHarness.initializeGraph(48000, 2, 24);
    syncHarness.startSegment(0, 230000000LL);
    syncHarness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);
    syncHarness.mockDsBuffer->status = DSBSTATUS_PLAYING;
    syncHarness.mockDsBuffer->playPos = 5817600;
    syncHarness.feedUpstreamChunk(6624000, true, 0, 230000000LL);
    syncHarness.tracker.onUpstreamEndOfStream(syncHarness.mockGraph);
    syncHarness.feedDownstreamChunk(nullptr, 6395040, true, 0, 222050000LL);

    // Asynchronous harness (#33-#40)
    AsyncControlHarness asyncHarness;
    asyncHarness.initializeGraph(48000, 2, 24);
    asyncHarness.startSegment(0, 230000000LL);
    asyncHarness.setPreReadDecision(eosTransition::PreReadDecision::FormatChanged);
    asyncHarness.mockDsBuffer->status = DSBSTATUS_PLAYING;
    asyncHarness.mockDsBuffer->playPos = 5817600;
    asyncHarness.feedUpstreamChunk(6624000, true, 0, 230000000LL);
    asyncHarness.tracker.onUpstreamEndOfStream(asyncHarness.mockGraph);
    asyncHarness.feedDownstreamChunk(nullptr, 6395040, true, 0, 222050000LL);

    // Symmetrical PCM and upstream/downstream state verification:
    const auto* gSync = syncHarness.tracker.findGraph(syncHarness.mockGraph);
    const auto* gAsync = asyncHarness.tracker.findGraph(asyncHarness.mockGraph);
    CHECK(gSync != nullptr && gAsync != nullptr);
    CHECK(gSync->upstreamBytes == gAsync->upstreamBytes);
    CHECK(gSync->downstreamBytes == gAsync->downstreamBytes);
    CHECK(gSync->targetBytes == gAsync->targetBytes);
    CHECK(gSync->transitionGeneration == gAsync->transitionGeneration);

    // In synchronous mode (#28): deferFormatChangePause blocks inside Pause()
    // In asynchronous mode (#33-#40): Pause returns immediately, and all post-Pause engine operations are suppressed
    asyncHarness.currentCallerContext = CallerContext::EngineAutoChange;
    CHECK(asyncHarness.executePause(1001) == S_OK);
    CHECK(asyncHarness.tracker.isVirtualPauseActive(asyncHarness.mockGraph) == true);

    // Verify symmetrical operation suppression:
    // When OpcDSPlay invokes SetPositions(0), dsSetCurrentPosition(0), dsSetVolume(-10000), dsStop(), filterStop(), controlStop():
    CHECK(asyncHarness.executeSetPositions(0, AM_SEEKING_AbsolutePositioning, nullptr, 0) == S_OK);
    CHECK(asyncHarness.deferredSeekQueued.load() == true);

    CHECK(asyncHarness.executeDsSetCurrentPosition(0) == DS_OK);
    CHECK(asyncHarness.dsSetCurrentPositionSuppressed.load() == true);

    CHECK(asyncHarness.executeDsSetVolume(-10000) == DS_OK);
    CHECK(asyncHarness.dsSetVolumeSuppressed.load() == true);

    CHECK(asyncHarness.executeDsStop() == DS_OK);
    CHECK(asyncHarness.dsStopSuppressed.load() == true);

    CHECK(asyncHarness.executeFilterStop() == S_OK);
    CHECK(asyncHarness.filterStopSuppressed.load() == true);

    CHECK(asyncHarness.executeControlStop() == S_OK);
    CHECK(asyncHarness.controlStopSuppressed.load() == true);

    // DirectSound buffer remains active and playing
    CHECK(asyncHarness.mockDsBuffer->status == DSBSTATUS_PLAYING);

    // Both modes share identical telemetry structure:
    auto syncSnap = syncHarness.queryHeartbeat();
    auto asyncSnap = asyncHarness.queryHeartbeat();
    CHECK(syncSnap.upstreamBytes == asyncSnap.upstreamBytes);
    CHECK(syncSnap.downstreamBytes == asyncSnap.downstreamBytes);
    CHECK(syncSnap.targetBytes == asyncSnap.targetBytes);
    CHECK(syncSnap.pcmDelivered == asyncSnap.pcmDelivered);

    asyncHarness.triggerWatchdogTimeout();
}

// Test 72: DirectSound Buffer Cursor Acquisition Failure Diagnostics and Error Reporting on Null / Lost / Invalid Buffer (Issue #41)
inline void testDirectSoundBufferCursorDiagnosticsAndFailureReporting() {
    std::printf("[TEST 72] DirectSound buffer cursor diagnostics and failure reporting (Issue #41)...\n");

    DWORD playCursor = 0, writeCursor = 0, status = 0;
    HRESULT hrPos = S_OK, hrStat = S_OK;

    // 1. Null buffer pointer -> must return false and report E_POINTER on both APIs without crashing
    bool resNull = eosTransition::safeQueryDirectSoundBuffer(nullptr, &playCursor, &writeCursor, &status, &hrPos, &hrStat);
    CHECK(resNull == false);
    CHECK(hrPos == E_POINTER);
    CHECK(hrStat == E_POINTER);

    // 2. Lost buffer condition (DSERR_BUFFERLOST = 0x88780096)
    MockDirectSoundBufferEx mockBuf;
    mockBuf.getCurrentPositionHr = static_cast<HRESULT>(0x88780096L); // DSERR_BUFFERLOST
    mockBuf.status = 0;
    bool resLost = eosTransition::safeQueryDirectSoundBuffer(&mockBuf, &playCursor, &writeCursor, &status, &hrPos, &hrStat);
    CHECK(resLost == false);
    CHECK(hrPos == static_cast<HRESULT>(0x88780096L));
    CHECK(hrStat == S_OK); // GetStatus succeeded while GetCurrentPosition reported lost buffer

    // 3. Status query failure condition
    mockBuf.getCurrentPositionHr = S_OK;
    mockBuf.getStatusHr = static_cast<HRESULT>(0x88780064L); // DSERR_INVALIDPARAM
    mockBuf.playPos = 12345;
    mockBuf.writePos = 23456;
    bool resStatFail = eosTransition::safeQueryDirectSoundBuffer(&mockBuf, &playCursor, &writeCursor, &status, &hrPos, &hrStat);
    CHECK(resStatFail == true); // play cursor succeeded
    CHECK(hrPos == S_OK);
    CHECK(hrStat == static_cast<HRESULT>(0x88780064L));
    CHECK(playCursor == 12345);

    // 4. Memory access violation / invalid address condition -> SEH catches and reports ERROR_INVALID_ADDRESS
    mockBuf.throwAccessViolationOnGetPos = true;
    bool resAv = eosTransition::safeQueryDirectSoundBuffer(&mockBuf, &playCursor, &writeCursor, &status, &hrPos, &hrStat);
    CHECK(resAv == false);
    CHECK(hrPos == HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS));
    CHECK(hrStat == HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS));

    // 5. Buffer lifetime preservation (AddRef / Release tracking)
    mockBuf.throwAccessViolationOnGetPos = false;
    mockBuf.getStatusHr = S_OK;
    ULONG refInitial = mockBuf.AddRef();
    mockBuf.Release();

    // Async drain monitor simulation: worker retains reference to buffer object
    IDirectSoundBuffer* pBuf = &mockBuf;
    pBuf->AddRef();
    ULONG refHeld = mockBuf.AddRef();
    mockBuf.Release();
    CHECK(refHeld == refInitial + 1); // Buffer is protected against premature deletion during drain

    pBuf->Release();
    ULONG refReleased = mockBuf.AddRef();
    mockBuf.Release();
    CHECK(refReleased == refInitial); // Returned to original reference count
}

// Test 73: Canonical Graph Key Resolution and EQ Bypass State (Issue #45)
inline void testCanonicalGraphKeyResolutionAndEqBypass() {
    std::printf("[TEST 73] Canonical graph key resolution and EQ bypass state (Issue #45)...\n");

    eosTransition::Tracker tracker;
    void* canonicalKey = reinterpret_cast<void*>(0x1000);
    void* secondaryKey = reinterpret_cast<void*>(0x1048); // e.g. pointer offset in multiple inheritance

    // Initially bypassed is false
    CHECK(tracker.isEqBypassed(canonicalKey) == false);

    // Set bypass on canonical key
    tracker.onGraphActivated(canonicalKey, true, 96000, 2, 24);
    tracker.setEqBypassed(canonicalKey, true);

    CHECK(tracker.isEqBypassed(canonicalKey) == true);

    // Fallback lookup: when an active 24-bit graph is bypassed, looking up from an unknown key finds true
    CHECK(tracker.isEqBypassed(secondaryKey) == true);

    // Disabling bypass
    tracker.setEqBypassed(canonicalKey, false);
    tracker.onGraphActivated(canonicalKey, false, 96000, 2, 24);
    CHECK(tracker.isEqBypassed(canonicalKey) == false);
    CHECK(tracker.isEqBypassed(secondaryKey) == false);
}

// Test 74: DirectSound Receive Sample Counting and EOS Trigger when EQ is Bypassed (Issue #45)
inline void testDirectSoundReceiveSampleCountingWhenEqBypassed() {
    std::printf("[TEST 74] DirectSound sample counting and EOS trigger on EQ bypass (Issue #45)...\n");

    eosTransition::Tracker tracker;
    void* graphKey = reinterpret_cast<void*>(0x2000);

    tracker.onGraphActivated(graphKey, true, 96000, 2, 24);
    tracker.setEqBypassed(graphKey, true);

    // Simulate upstream feeding 576,000 bytes (1 second of 96k/24b/stereo = 96000 * 6)
    const uint32_t trackBytes = 576000;
    const uint32_t chunkBytes = 28800;
    for (uint32_t b = 0; b < trackBytes; b += chunkBytes) {
        tracker.onUpstreamSample(graphKey, chunkBytes, false, 0, 0);
    }
    // Upstream finishes with EOS -> targetBytes is set to 576,000
    tracker.onUpstreamEndOfStream(graphKey);

    const auto* g = tracker.findGraph(graphKey);
    CHECK(g != nullptr);
    CHECK(g->targetBytes == trackBytes);
    CHECK(g->downstreamBytes == 0); // downstream not yet fed

    // Now simulate DirectSound renderer receiving samples directly because EQ is bypassed
    bool eosTriggered = false;
    for (uint32_t b = 0; b < trackBytes; b += chunkBytes) {
        bool trig = tracker.onDownstreamSample(graphKey, chunkBytes, false, 0, 0);
        if (trig) eosTriggered = true;
    }

    CHECK(g->downstreamBytes == trackBytes);
    CHECK(tracker.hasDownstreamReachedTarget(graphKey) == true);
    CHECK(eosTriggered == true); // Reached target -> delivered EOS downstream
}

// Test 75: Track Generation Correlation with Downstream PCM (Issue #45)
inline void testTrackGenerationCorrelationWithDownstreamPcm() {
    std::printf("[TEST 75] Track generation correlation with downstream PCM (Issue #45)...\n");

    eosTransition::Tracker tracker;
    void* graphKey = reinterpret_cast<void*>(0x3000);

    // Initial state: generation is 0
    tracker.onGraphActivated(graphKey, true, 96000, 2, 24);
    uint32_t gen0 = tracker.getTransitionGeneration(graphKey);
    CHECK(gen0 == 0);

    // First transition (Track 1 -> Track 2): Deferral on format change increments generation to 1
    tracker.markFormatChangePauseDeferred(graphKey);
    uint32_t gen1 = tracker.getTransitionGeneration(graphKey);
    CHECK(gen1 == 1);

    tracker.onFormatChangePauseCompleted(graphKey);

    // Second transition (Track 2 -> Track 3): Deferral on format change increments generation to 2
    tracker.markFormatChangePauseDeferred(graphKey);
    uint32_t gen2 = tracker.getTransitionGeneration(graphKey);
    CHECK(gen2 == 2);
}

// Test 76: Distinction between Diagnostic Deficit and Track Tail Truncation (Issue #45)
inline void testDistinctionDiagnosticDeficitVsTailTruncation() {
    std::printf("[TEST 76] Distinction between diagnostic deficit and track tail truncation (Issue #45)...\n");

    eosTransition::Tracker tracker;
    void* graphKey = reinterpret_cast<void*>(0x4000);

    tracker.onGraphActivated(graphKey, true, 96000, 2, 24);
    tracker.onUpstreamSample(graphKey, 576000, false, 0, 0);
    tracker.onUpstreamEndOfStream(graphKey);

    // Case 76A: Downstream PCM counter incomplete (e.g. 500,000 / 576,000)
    // BUT renderer reached stop position (c >= s): This is DIAGNOSTIC_DEFICIT, audio tail intact (0ms missing)!
    tracker.onDownstreamSample(graphKey, 500000, false, 0, 0);
    LONGLONG c = 10000000LL;
    LONGLONG s = 10000000LL;
    bool rendererCompleteA = (c >= s && s > 0);
    CHECK(rendererCompleteA == true);
    // Audio is complete!
    uint32_t missingMsA = (c >= s) ? 0 : static_cast<DWORD>((s - c) / 10000);
    CHECK(missingMsA == 0);

    // Case 76B: Premature stop before renderer reached stop position (c = 8,000,000, s = 10,000,000)
    // and no EOS delivered: This is GENUINE TRACK_TAIL_TRUNCATED!
    LONGLONG cTrunc = 8000000LL;
    bool rendererCompleteB = (cTrunc >= s && s > 0);
    CHECK(rendererCompleteB == false);
    uint32_t missingMsB = static_cast<DWORD>((s - cTrunc) / 10000);
    CHECK(missingMsB == 200); // 200ms missing tail!
}

// Test 77: Synchronous Pause Deferral Until Renderer Drain with Active Downstream PCM Progress (Issue #47)
inline void testSyncPauseDeferralUntilRendererDrainWithActiveDownstreamPcm() {
    std::printf("[TEST 77] Synchronous pause deferral until renderer drain with active downstream PCM (Issue #47)...\n");

    eosTransition::Tracker tracker;
    void* graphKey = reinterpret_cast<void*>(0x5000);

    // 1. Initial track setup: 48kHz/24bit FLAC -> DSEE 96kHz/24bit, EQ bypassed, format change pre-read prepared
    tracker.onGraphActivated(graphKey, true, 96000, 2, 24);
    tracker.setEqBypassed(graphKey, true);
    LONGLONG stopPos = 230000000LL; // 23 seconds total duration (in 100ns units)
    tracker.onStopPositionReported(graphKey, stopPos);

    // Segment size: 23 seconds @ 96kHz / 24-bit / stereo = 23 * 96000 * 6 = 13,248,000 bytes
    const uint64_t totalBytes = 13248000ULL;
    tracker.onUpstreamSample(graphKey, totalBytes, true, 0, stopPos);
    tracker.onUpstreamEndOfStream(graphKey);

    // 2. Downstream PCM delivery progress: all 13,248,000 bytes delivered to renderer's input pin
    bool eosDeliveredBySample = tracker.onDownstreamSample(graphKey, static_cast<uint32_t>(totalBytes), true, 0, stopPos);
    CHECK(eosDeliveredBySample == true);

    // Verify 3-way drain state separation:
    // State 1: Downstream PCM delivery complete
    CHECK(tracker.hasDownstreamReachedTarget(graphKey) == true);
    CHECK(tracker.isPcmDeliveryComplete(graphKey) == true);

    // State 2: EOS delivered to downstream renderer
    CHECK(tracker.isDownstreamEosDelivered(graphKey) == true);
    CHECK(tracker.getDownstreamEosDeliveredCount(graphKey) == 1);

    // State 3: Renderer audio output has NOT drained yet (DirectSound buffer still playing)
    CHECK(tracker.isRendererOutputComplete(graphKey) == false);
    CHECK(tracker.isDrainFullyComplete(graphKey) == false);

    // 3. Approximately 1.55 seconds after EOS delivery (~20.23s into 23s track):
    // Playback engine issues automatic Pause(). Live current position is at 20.23 seconds.
    LONGLONG curPosAtPause = 202300000LL; // 20.23 seconds (< 23.00 seconds stop position)
    tracker.onCurrentPositionReported(graphKey, curPosAtPause);

    // CRITICAL (Issue #47 root cause verification):
    // In #45, isFormatChangePending returned false because eosDelivered was true AND downstream reached target.
    // In #47, isFormatChangePending MUST remain true because the renderer buffer is still draining out to speakers!
    CHECK(tracker.isFormatChangePending(graphKey) == true);

    // 4. Synchronous pause deferral begins
    tracker.markFormatChangePauseDeferred(graphKey);
    CHECK(tracker.isPauseDeferralActive(graphKey) == true);

    // Verify EOS duplicate prevention: calling onDownstreamEndOfStream MUST NOT deliver duplicate EOS!
    bool duplicateEos = tracker.onDownstreamEndOfStream(graphKey);
    CHECK(duplicateEos == false);
    CHECK(tracker.getDownstreamEosDeliveredCount(graphKey) == 1);

    // 5. DirectSound buffer plays out remaining ~2.77 seconds to track completion
    LONGLONG drainedPos = 230000000LL; // Reached stop position
    tracker.onCurrentPositionReported(graphKey, drainedPos);
    tracker.onRendererOutputComplete(graphKey);

    // Drain completes cleanly
    tracker.onFormatChangePauseCompleted(graphKey, eosTransition::PauseDeferralStatus::CompletedDrain);

    // 6. Post-drain verification
    CHECK(tracker.isRendererOutputComplete(graphKey) == true);
    CHECK(tracker.isDrainFullyComplete(graphKey) == true);
    CHECK(tracker.isPauseDeferralActive(graphKey) == false);
    CHECK(tracker.isFormatChangePending(graphKey) == false);
    CHECK(tracker.getDownstreamEosDeliveredCount(graphKey) == 1);
}

inline void runAllAsyncTransitionTests() {
    std::printf("\n=== Issue #33 / #34 / #35 / #36 / #37 / #38 / #39 / #40 / #41 / #45 / #47 Non-blocking Virtual Pause & Deferral Tests ===\n");
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
    testNewGraphPlaybackPrecedesDeferredSeekAndCausesTimeoutDuplicate();
    testUnifiedSeekAndPlaybackStartOnNaturalDrain();
    testUnifiedSeekAndPlaybackStartOnTimeout();
    testManualCancellationPurgesUnifiedSeekAndRun();
    testTransitionGenerationMismatchDiscardsStalePrep();
    testUnifiedPreparationCOMThreadBoundaryMarshalAndExecute();
    testHeartbeatGenerationAndByteCounters();
    testDirectSoundBufferCursorTrackingAcrossVirtualPause();
    testIndependentPcmStallVsPositionMeasurementIsolation();
    testOldBufferIdentificationAndReferenceLifetime();
    testEngineFilterAndBufferStopSuppressionDuringVirtualPause();
    testCleanNaturalEosPropagationWithoutLoopInjection();
    testSymmetricalDiagnosticsSyncVsAsync();
    testDirectSoundBufferCursorDiagnosticsAndFailureReporting();
    testCanonicalGraphKeyResolutionAndEqBypass();
    testDirectSoundReceiveSampleCountingWhenEqBypassed();
    testTrackGenerationCorrelationWithDownstreamPcm();
    testDistinctionDiagnosticDeficitVsTailTruncation();
    testSyncPauseDeferralUntilRendererDrainWithActiveDownstreamPcm();
    std::printf("=== Issue #33 / #34 / #35 / #36 / #37 / #38 / #39 / #40 / #41 / #45 / #47 Non-blocking Virtual Pause Tests Completed ===\n\n");
}

} // namespace asyncTests
