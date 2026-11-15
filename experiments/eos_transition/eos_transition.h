#pragma once
// Experimental segment-based EndOfStream (EOS) state machine for format-changing
// track transitions. No fixed track duration (e.g. 23s) assumptions.
#include <windows.h>
#include <dshow.h>
#include <dsound.h>
#include <cstdint>
#include <map>

struct IStream;

namespace eosTransition {

inline bool safeQueryDirectSoundBuffer(IDirectSoundBuffer* buf, DWORD* playCursor, DWORD* writeCursor, DWORD* status, HRESULT* hrPos, HRESULT* hrStat) {
    if (!buf) {
        if (hrPos) *hrPos = E_POINTER;
        if (hrStat) *hrStat = E_POINTER;
        return false;
    }
    __try {
        HRESULT pRes = buf->GetCurrentPosition(playCursor, writeCursor);
        if (hrPos) *hrPos = pRes;
        HRESULT sRes = buf->GetStatus(status);
        if (hrStat) *hrStat = sRes;
        return SUCCEEDED(pRes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (hrPos) *hrPos = HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
        if (hrStat) *hrStat = HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
        return false;
    }
}

enum class PreReadDecision {
    Unknown,        // Next track format not yet determined (or 0x31E7A not called)
    SameFormat24,   // Next track is 24-bit stereo at the same sample rate (preserve seamless pre-read)
    FormatChanged   // Next track differs in bit depth, sample rate, or channel count
};

enum class PauseDeferralStatus {
    None,
    Active,
    CompletedDrain,
    CancelledStop,
    CancelledSeek,
    CancelledTrackChange,
    CancelledManualPause,
    CancelledManualPlay,
    CancelledQuit,
    TimedOut
};

enum class ResetReason {
    NewSegment,
    Seek,
    BeginFlush,
    EndFlush,
    Stop,
    ManualTrackChange
};

struct GraphState {
    bool active24 = false;
    DWORD sampleRate = 0;
    WORD channels = 0;
    WORD bitsPerSample = 0;

    REFERENCE_TIME segmentStart = 0;
    REFERENCE_TIME segmentStop = 0;
    double segmentRate = 1.0;

    uint64_t upstreamSamples = 0;
    uint64_t upstreamBytes = 0;
    REFERENCE_TIME upstreamLastEndTime = -1;
    bool upstreamHasTimestamp = false;
    uint32_t upstreamEosCount = 0;

    uint64_t targetBytes = 0;
    REFERENCE_TIME targetEndTime = -1;
    bool targetHasTimestamp = false;

    uint64_t downstreamSamples = 0;
    uint64_t downstreamBytes = 0;
    REFERENCE_TIME downstreamLastEndTime = -1;
    bool downstreamHasTimestamp = false;
    bool eqBypassed = false;

    LONGLONG stopPosition = 0;
    LONGLONG currentPosition = 0;
    bool hasStopPosition = false;
    bool hasCurrentPosition = false;

    PreReadDecision preReadDecision = PreReadDecision::Unknown;
    bool eosPending = false;
    bool eosDelivered = false;
    bool formatChangePauseDeferred = false;
    bool formatChangePauseCompleted = false;
    PauseDeferralStatus pauseDeferralStatus = PauseDeferralStatus::None;
    bool formatChangeStopDeferred = false;
    bool formatChangeCompleted = false;
    uint32_t downstreamEosDeliveredCount = 0;
    uint32_t seamlessTransitionCount = 0;
    ResetReason lastResetReason = ResetReason::NewSegment;

    struct DeferredSeekParams {
        bool valid = false;
        void* targetGraph = nullptr;
        void* targetSeeking = nullptr;
        IStream* targetSeekingMarshalStream = nullptr;
        LONGLONG current = 0;
        DWORD currentFlags = 0;
        LONGLONG stop = 0;
        DWORD stopFlags = 0;
    };

    struct DeferredNextTrackPreparation {
        bool valid = false;
        void* targetGraph = nullptr;
        uint32_t transitionGeneration = 0;

        // Deferred Seek (SetPositions)
        bool hasSeek = false;
        void* targetSeeking = nullptr;
        IStream* targetSeekingMarshalStream = nullptr;
        LONGLONG seekCurrent = 0;
        DWORD seekCurrentFlags = 0;
        LONGLONG seekStop = 0;
        DWORD seekStopFlags = 0;

        // Deferred Playback Start (IMediaControl::Run)
        bool hasRun = false;
        void* targetControl = nullptr;
        IStream* targetControlMarshalStream = nullptr;
        void* runCaller = nullptr;

        // Deferred DirectSound Play
        bool hasDsPlay = false;
        void* targetDsBuffer = nullptr;
        DWORD dsReserved1 = 0;
        DWORD dsPriority = 0;
        DWORD dsFlags = 0;
        void* dsCaller = nullptr;
    };

    struct TransitionContext {
        uint64_t transitionId = 0;
        uint32_t generation = 0;
        DWORD initiatingThreadId = 0;
        DWORD enterTick = 0;
        void* primaryDsBuffer = nullptr;
        bool pcmDeliveryComplete = false;
        bool downstreamEosDelivered = false;
        bool rendererOutputComplete = false;
        bool completed = false;
        bool cancelled = false;
    };

    bool virtualPauseActive = false;
    bool autoTrackChangeInProgress = false;
    uint32_t transitionGeneration = 0;
    DWORD virtualPauseEnterTick = 0;
    TransitionContext transition;
    DeferredNextTrackPreparation deferredPreparation;
    DeferredSeekParams deferredSeek;
    bool deferredDsStop = false;
    bool deferredGraphStop = false;

    void releaseDeferredStreams() {
        if (deferredPreparation.targetSeekingMarshalStream) {
            auto* pStream = deferredPreparation.targetSeekingMarshalStream;
            deferredPreparation.targetSeekingMarshalStream = nullptr;
            pStream->Release();
        }
        if (deferredPreparation.targetControlMarshalStream) {
            auto* pStream = deferredPreparation.targetControlMarshalStream;
            deferredPreparation.targetControlMarshalStream = nullptr;
            pStream->Release();
        }
        if (deferredSeek.targetSeekingMarshalStream) {
            auto* pStream = deferredSeek.targetSeekingMarshalStream;
            deferredSeek.targetSeekingMarshalStream = nullptr;
            pStream->Release();
        }
    }

    void releaseDeferredSeekStream() {
        releaseDeferredStreams();
    }

    bool isVirtualPauseActive() const { return virtualPauseActive; }
    bool isPauseDeferralActive() const { return virtualPauseActive || pauseDeferralStatus == PauseDeferralStatus::Active; }
    bool isCancelled() const {
        return transition.cancelled ||
               pauseDeferralStatus == PauseDeferralStatus::CancelledStop ||
               pauseDeferralStatus == PauseDeferralStatus::CancelledSeek ||
               pauseDeferralStatus == PauseDeferralStatus::CancelledTrackChange ||
               pauseDeferralStatus == PauseDeferralStatus::CancelledManualPause ||
               pauseDeferralStatus == PauseDeferralStatus::CancelledManualPlay ||
               pauseDeferralStatus == PauseDeferralStatus::CancelledQuit;
    }
};

class Tracker {
public:
    void onGraphActivated(void* graphKey, bool active24, DWORD sampleRate, WORD channels = 2, WORD bitsPerSample = 24) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.active24 = active24;
        s.sampleRate = sampleRate;
        s.channels = channels;
        s.bitsPerSample = bitsPerSample;
        if (!active24) {
            s.eosPending = false;
        }
    }

    void setEqBypassed(void* graphKey, bool bypassed) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.eqBypassed = bypassed;
    }

    bool isEqBypassed(void* graphKey) {
        if (!graphKey) return false;
        auto* s = findGraph(graphKey);
        return s ? s->eqBypassed : false;
    }

    // Called on 0x31E7A (next-track pre-read setter) when the graph key is known,
    // or falls back to active 24-bit graphs when graphKey is nullptr.
    void onPreReadDecision(void* graphKey, PreReadDecision decision) {
        if (graphKey) {
            applyPreReadDecision(ensureGraph(graphKey), decision);
            return;
        }
        for (auto& kv : graphs_) {
            if (kv.second.active24) {
                applyPreReadDecision(kv.second, decision);
            }
        }
    }

    void onPreReadFormatCandidate(void* graphKey, bool eligible24Applied, DWORD nextRate, WORD nextChannels, WORD nextBits) {
        if (graphKey) {
            auto& s = ensureGraph(graphKey);
            bool same = s.active24 && eligible24Applied && nextBits == 24 && nextChannels == s.channels && nextRate == s.sampleRate;
            applyPreReadDecision(s, same ? PreReadDecision::SameFormat24 : PreReadDecision::FormatChanged);
            return;
        }
        for (auto& kv : graphs_) {
            auto& s = kv.second;
            if (!s.active24) continue;
            bool same = eligible24Applied && nextBits == 24 && nextChannels == s.channels && nextRate == s.sampleRate;
            applyPreReadDecision(s, same ? PreReadDecision::SameFormat24 : PreReadDecision::FormatChanged);
        }
    }

    void onUpstreamNewSegment(void* graphKey, REFERENCE_TIME tStart, REFERENCE_TIME tStop, double rate) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.segmentStart = tStart;
        s.segmentStop = tStop;
        s.segmentRate = rate;
        // If upstream already saw EndOfStream for the current segment and a new segment
        // arrives without a flush/seek/stop while not marked FormatChanged, treat it as
        // a same-format seamless continuation and keep cumulative byte alignment.
        if (s.upstreamEosCount > 0 && !s.eosDelivered && s.preReadDecision != PreReadDecision::FormatChanged) {
            s.upstreamEosCount = 0;
            s.targetBytes = 0;
            s.targetEndTime = -1;
            s.targetHasTimestamp = false;
            s.eosPending = false;
            s.preReadDecision = PreReadDecision::Unknown;
            ++s.seamlessTransitionCount;
            s.lastResetReason = ResetReason::NewSegment;
            return;
        }
        clearSegmentCounters(s, ResetReason::NewSegment);
    }

    // Called before SeamlessInput::Receive forwards a sample into COutputQueue so
    // that if new upstream samples arrive after upstream EndOfStream (same-format
    // next track connected to SeamlessFilter), pending EOS is disarmed immediately.
    void onUpstreamSamplePreReceive(void* graphKey, uint32_t bytes) {
        if (!graphKey || bytes == 0) return;
        auto& s = ensureGraph(graphKey);
        if (s.upstreamEosCount > 0) {
            s.preReadDecision = PreReadDecision::Unknown;
            s.eosPending = false;
            s.upstreamEosCount = 0;
            s.targetBytes = 0;
            s.targetEndTime = -1;
            s.targetHasTimestamp = false;
            ++s.seamlessTransitionCount;
        }
    }

    void onUpstreamSample(void* graphKey, uint32_t bytes, bool hasTime, REFERENCE_TIME sStart, REFERENCE_TIME sEnd) {
        (void)sStart;
        if (!graphKey || bytes == 0) return;
        auto& s = ensureGraph(graphKey);
        if (s.isCancelled()) {
            s.pauseDeferralStatus = PauseDeferralStatus::None;
        }
        if (s.upstreamEosCount > 0) {
            // New upstream PCM arrived after EndOfStream without Flush/Seek/Stop:
            // same-format pre-read track is now feeding SeamlessFilter.
            s.preReadDecision = PreReadDecision::Unknown;
            s.eosPending = false;
            s.upstreamEosCount = 0;
            s.targetBytes = 0;
            s.targetEndTime = -1;
            s.targetHasTimestamp = false;
            ++s.seamlessTransitionCount;
        }
        ++s.upstreamSamples;
        s.upstreamBytes += bytes;
        if (hasTime) {
            s.upstreamLastEndTime = sEnd;
            s.upstreamHasTimestamp = true;
        }
    }

    // Returns true if downstream has ALREADY received the final PCM of the segment
    // at the moment upstream EndOfStream arrives, requiring immediate one-shot EOS delivery.
    bool onUpstreamEndOfStream(void* graphKey) {
        if (!graphKey) return false;
        auto& s = ensureGraph(graphKey);
        ++s.upstreamEosCount;
        if (s.upstreamEosCount > 1) {
            // Duplicate upstream EndOfStream on the same segment: never reset eosDelivered.
            return false;
        }
        if (s.upstreamSamples == 0 || s.upstreamBytes == 0) {
            return false;
        }
        s.targetBytes = s.upstreamBytes;
        s.targetEndTime = s.upstreamLastEndTime;
        s.targetHasTimestamp = s.upstreamHasTimestamp;
        if (s.active24 && !s.eosDelivered && s.preReadDecision != PreReadDecision::SameFormat24) {
            s.eosPending = true;
            if (shouldDeliverDownstreamEos(s)) {
                s.eosPending = false;
                s.eosDelivered = true;
                ++s.downstreamEosDeliveredCount;
                return true;
            }
        }
        return false;
    }

    // Returns true if downstream has just received the final PCM of the segment and
    // IPin::EndOfStream should now be delivered once to the downstream input pin.
    bool onDownstreamSample(void* graphKey, uint32_t bytes, bool hasTime, REFERENCE_TIME sStart, REFERENCE_TIME sEnd) {
        (void)sStart;
        if (!graphKey || bytes == 0) return false;
        auto& s = ensureGraph(graphKey);
        ++s.downstreamSamples;
        s.downstreamBytes += bytes;
        if (hasTime) {
            s.downstreamLastEndTime = sEnd;
            s.downstreamHasTimestamp = true;
        }
        if (shouldDeliverDownstreamEos(s)) {
            s.eosPending = false;
            s.eosDelivered = true;
            ++s.downstreamEosDeliveredCount;
            return true;
        }
        return false;
    }

    // Returns true if a direct IPin::EndOfStream call on the downstream pin should be
    // forwarded; returns false if EOS was already delivered for this segment.
    bool onDownstreamEndOfStream(void* graphKey) {
        if (!graphKey) return true;
        auto& s = ensureGraph(graphKey);
        if (s.eosDelivered || s.isCancelled()) {
            return false;
        }
        s.eosPending = false;
        s.eosDelivered = true;
        ++s.downstreamEosDeliveredCount;
        return true;
    }

    void onStopPositionReported(void* graphKey, LONGLONG stopPos) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.stopPosition = stopPos;
        s.hasStopPosition = true;
    }

    void onCurrentPositionReported(void* graphKey, LONGLONG currentPos) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.currentPosition = currentPos;
        s.hasCurrentPosition = true;
    }

    void onSeekPositions(void* graphKey, bool modifiesCurrent, LONGLONG currentPos, bool modifiesStop, LONGLONG stopPos) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        if (s.isPauseDeferralActive()) {
            cancelFormatChangePause(graphKey, PauseDeferralStatus::CancelledSeek);
        }
        clearSegmentCounters(s, ResetReason::Seek);
        if (modifiesStop && stopPos >= 0) {
            s.stopPosition = stopPos;
            s.hasStopPosition = true;
        }
        if (modifiesCurrent && currentPos >= 0) {
            s.currentPosition = currentPos;
            s.hasCurrentPosition = true;
        }
    }

    bool isFormatChangePending(void* graphKey) const {
        const auto* s = findGraph(graphKey);
        if (!s) return false;
        if (!s->active24) return false;
        if (s->preReadDecision == PreReadDecision::SameFormat24) return false;
        if (s->upstreamEosCount == 0) return false;
        if (s->formatChangePauseCompleted || s->formatChangeCompleted) return false;
        if (s->pauseDeferralStatus != PauseDeferralStatus::None && s->pauseDeferralStatus != PauseDeferralStatus::Active) {
            return false;
        }
        if (!s->eosDelivered) return true;
        if (s->hasStopPosition && s->hasCurrentPosition && s->currentPosition < s->stopPosition) {
            return true;
        }
        if (s->targetBytes > 0 && s->downstreamBytes < s->targetBytes) {
            return true;
        }
        return false;
    }

    bool hasDownstreamReachedTarget(void* graphKey) const {
        const auto* s = findGraph(graphKey);
        return s ? downstreamReachedTarget(*s) : false;
    }

    void markFormatChangePauseDeferred(void* graphKey) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.formatChangePauseDeferred = true;
        s.pauseDeferralStatus = PauseDeferralStatus::Active;
    }

    void onFormatChangePauseCompleted(void* graphKey, PauseDeferralStatus status = PauseDeferralStatus::CompletedDrain) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        if (s.isCancelled()) return;
        s.formatChangePauseCompleted = true;
        s.formatChangePauseDeferred = false;
        s.pauseDeferralStatus = status;
        s.eosPending = false;
    }

    void cancelFormatChangePause(void* graphKey, PauseDeferralStatus status) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.virtualPauseActive = false;
        s.transition.cancelled = true;
        s.formatChangePauseDeferred = false;
        s.pauseDeferralStatus = status;
        s.releaseDeferredStreams();
        s.deferredPreparation = GraphState::DeferredNextTrackPreparation{};
        s.deferredSeek = GraphState::DeferredSeekParams{};
        s.deferredDsStop = false;
        s.deferredGraphStop = false;
        s.eosPending = false;
    }

    uint64_t nextTransitionId_ = 0;

    void registerActiveSoundBuffer(void* graphKey, void* buffer) {
        if (graphKey) {
            auto& s = ensureGraph(graphKey);
            s.transition.primaryDsBuffer = buffer;
        } else {
            for (auto& kv : graphs_) {
                if (kv.second.virtualPauseActive || kv.second.active24) {
                    kv.second.transition.primaryDsBuffer = buffer;
                }
            }
        }
    }

    bool isTransitionBuffer(void* buffer, void** outGraphKey = nullptr) const {
        if (!buffer) return false;
        for (const auto& kv : graphs_) {
            if (kv.second.virtualPauseActive && kv.second.transition.primaryDsBuffer == buffer) {
                if (outGraphKey) *outGraphKey = kv.first;
                return true;
            }
        }
        return false;
    }

    bool isTransitionThread(void* graphKey, DWORD threadId) const {
        const auto* s = findGraph(graphKey);
        if (!s || !s->virtualPauseActive) return false;
        return (s->transition.initiatingThreadId == 0 || s->transition.initiatingThreadId == threadId);
    }

    void markVirtualPauseActive(void* graphKey, DWORD initiatingThreadId = 0, void* primaryDsBuffer = nullptr) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.virtualPauseActive = true;
        s.autoTrackChangeInProgress = true;
        s.transitionGeneration++;
        s.virtualPauseEnterTick = GetTickCount();
        s.formatChangePauseDeferred = true;
        s.pauseDeferralStatus = PauseDeferralStatus::Active;
        s.releaseDeferredStreams();
        s.deferredPreparation = GraphState::DeferredNextTrackPreparation{};
        s.deferredSeek = GraphState::DeferredSeekParams{};
        s.deferredDsStop = false;
        s.deferredGraphStop = false;

        s.transition.transitionId = ++nextTransitionId_;
        s.transition.generation = s.transitionGeneration;
        s.transition.initiatingThreadId = initiatingThreadId;
        s.transition.enterTick = s.virtualPauseEnterTick;
        if (primaryDsBuffer) s.transition.primaryDsBuffer = primaryDsBuffer;
        s.transition.pcmDeliveryComplete = false;
        s.transition.downstreamEosDelivered = false;
        s.transition.rendererOutputComplete = false;
        s.transition.completed = false;
        s.transition.cancelled = false;
    }

    enum class RunClassification {
        NotVirtualPause,
        AutomatedDeferred,
        OldGraphIgnored,
        ManualCancelled
    };

    RunClassification classifyRunOperation(void* targetGraphKey, bool isExplicitManualOp,
                                           DWORD lastInputTick = 0, void** outActiveVirtualGraph = nullptr) const {
        void* activeVirtualKey = nullptr;
        if (!findActiveVirtualPauseGraph(&activeVirtualKey)) {
            if (outActiveVirtualGraph) *outActiveVirtualGraph = nullptr;
            return RunClassification::NotVirtualPause;
        }
        if (outActiveVirtualGraph) {
            *outActiveVirtualGraph = activeVirtualKey;
        }
        const auto* activeState = findGraph(activeVirtualKey);
        if (!activeState || !activeState->virtualPauseActive) {
            return RunClassification::NotVirtualPause;
        }

        // Priority 1: User manual operations (mouse click, keyboard, explicit Play)
        if (isExplicitManualOp) {
            return RunClassification::ManualCancelled;
        }
        if (lastInputTick != 0 && (lastInputTick >= activeState->virtualPauseEnterTick || (GetTickCount() - lastInputTick < 400))) {
            return RunClassification::ManualCancelled;
        }

        // Priority 2: Automated engine operations (no user input observed)
        bool isOldGraph = (targetGraphKey == nullptr || targetGraphKey == activeVirtualKey);
        if (isOldGraph) {
            return RunClassification::OldGraphIgnored;
        } else {
            return RunClassification::AutomatedDeferred;
        }
    }

    enum class SeekClassification {
        NotVirtualPause,
        AutomatedDeferred,
        OldGraphAutomatedPark,
        ManualCancelled
    };

    bool findActiveVirtualPauseGraph(void** outGraphKey) const {
        for (const auto& kv : graphs_) {
            if (kv.second.virtualPauseActive) {
                if (outGraphKey) *outGraphKey = kv.first;
                return true;
            }
        }
        if (outGraphKey) *outGraphKey = nullptr;
        return false;
    }

    SeekClassification classifySeekOperation(void* targetGraphKey, bool isExplicitManualOp, bool isExplicitEngineCall,
                                            LONGLONG seekPos, DWORD lastInputTick = 0, void** outActiveVirtualGraph = nullptr) const {
        void* activeVirtualKey = nullptr;
        if (!findActiveVirtualPauseGraph(&activeVirtualKey)) {
            if (outActiveVirtualGraph) *outActiveVirtualGraph = nullptr;
            return SeekClassification::NotVirtualPause;
        }
        if (outActiveVirtualGraph) {
            *outActiveVirtualGraph = activeVirtualKey;
        }
        const auto* activeState = findGraph(activeVirtualKey);
        if (!activeState || !activeState->virtualPauseActive) {
            return SeekClassification::NotVirtualPause;
        }

        // Priority 1: User manual operations (mouse click, keyboard, seekbar scroll, explicit UI op)
        if (isExplicitManualOp) {
            return SeekClassification::ManualCancelled;
        }
        if (lastInputTick != 0 && (lastInputTick >= activeState->virtualPauseEnterTick || (GetTickCount() - lastInputTick < 400))) {
            return SeekClassification::ManualCancelled;
        }

        // Priority 2: Automated engine operations (no user input observed)
        bool isOldGraph = (targetGraphKey == nullptr || targetGraphKey == activeVirtualKey);

        if (isOldGraph) {
            // Internal call on the OLD graph while it is in Virtual Pause:
            // ~6ms after Pause, the engine calls SetPositions(~20.066s) on the old graph.
            // This is an internal old-graph positioning/parking call; it must NOT cancel the drain
            // and must NOT flush the old graph's draining PCM buffer!
            if (seekPos > 0) {
                return SeekClassification::OldGraphAutomatedPark;
            }
            // If seekPos == 0 on the old graph (e.g. single-graph test environment):
            // If a deferred seek is already queued for this transition generation, park/absorb subsequent seeks.
            if (activeState->deferredSeek.valid) {
                return SeekClassification::OldGraphAutomatedPark;
            }
            return SeekClassification::AutomatedDeferred;
        } else {
            // Seek on a NEW/different graph while old graph is in Virtual Pause:
            // This is the next track's preparation seek (e.g. SetPositions(0) on newGraph).
            // It must be deferred until the old graph's drain completes naturally,
            // then applied to the new graph once!
            return SeekClassification::AutomatedDeferred;
        }
    }

    uint32_t getTransitionGeneration(void* graphKey) const {
        const auto* s = findGraph(graphKey);
        return s ? s->transitionGeneration : 0;
    }

    bool isVirtualPauseActive(void* graphKey) const {
        const auto* s = findGraph(graphKey);
        return s ? s->virtualPauseActive : false;
    }

    bool hasAnyVirtualPauseActive() const {
        for (const auto& kv : graphs_) {
            if (kv.second.virtualPauseActive) return true;
        }
        return false;
    }

    void onDownstreamPcmDelivered(void* graphKey) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.transition.pcmDeliveryComplete = true;
    }

    void onDownstreamEosDelivered(void* graphKey) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.transition.downstreamEosDelivered = true;
    }

    void onRendererOutputComplete(void* graphKey) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.transition.rendererOutputComplete = true;
    }

    bool isDrainFullyComplete(void* graphKey) const {
        const auto* s = findGraph(graphKey);
        return s && s->transition.pcmDeliveryComplete && s->transition.rendererOutputComplete;
    }

    void deferAutomaticSeek(void* activeGraphKey, void* targetGraphKey, void* targetSeeking,
                            LONGLONG current, DWORD currentFlags, LONGLONG stop, DWORD stopFlags,
                            IStream* marshalStream = nullptr) {
        if (!activeGraphKey) return;
        auto& s = ensureGraph(activeGraphKey);
        if (s.deferredPreparation.targetSeekingMarshalStream) {
            s.deferredPreparation.targetSeekingMarshalStream->Release();
            s.deferredPreparation.targetSeekingMarshalStream = nullptr;
        }
        s.deferredPreparation.valid = true;
        s.deferredPreparation.targetGraph = targetGraphKey ? targetGraphKey : activeGraphKey;
        s.deferredPreparation.transitionGeneration = s.transitionGeneration;
        s.deferredPreparation.hasSeek = true;
        s.deferredPreparation.targetSeeking = targetSeeking;
        s.deferredPreparation.targetSeekingMarshalStream = marshalStream;
        s.deferredPreparation.seekCurrent = current;
        s.deferredPreparation.seekCurrentFlags = currentFlags;
        s.deferredPreparation.seekStop = stop;
        s.deferredPreparation.seekStopFlags = stopFlags;

        s.deferredSeek.valid = true;
        s.deferredSeek.targetGraph = s.deferredPreparation.targetGraph;
        s.deferredSeek.targetSeeking = targetSeeking;
        s.deferredSeek.current = current;
        s.deferredSeek.currentFlags = currentFlags;
        s.deferredSeek.stop = stop;
        s.deferredSeek.stopFlags = stopFlags;
    }

    void deferAutomaticSeek(void* graphKey, LONGLONG current, DWORD currentFlags, LONGLONG stop, DWORD stopFlags) {
        deferAutomaticSeek(graphKey, graphKey, nullptr, current, currentFlags, stop, stopFlags, nullptr);
    }

    void deferAutomaticRun(void* activeGraphKey, void* targetGraphKey, void* targetControl,
                           IStream* marshalStream, void* caller) {
        if (!activeGraphKey) return;
        auto& s = ensureGraph(activeGraphKey);
        if (s.deferredPreparation.targetControlMarshalStream) {
            s.deferredPreparation.targetControlMarshalStream->Release();
            s.deferredPreparation.targetControlMarshalStream = nullptr;
        }
        s.deferredPreparation.valid = true;
        s.deferredPreparation.targetGraph = targetGraphKey ? targetGraphKey : activeGraphKey;
        s.deferredPreparation.transitionGeneration = s.transitionGeneration;
        s.deferredPreparation.hasRun = true;
        s.deferredPreparation.targetControl = targetControl;
        s.deferredPreparation.targetControlMarshalStream = marshalStream;
        s.deferredPreparation.runCaller = caller;
    }

    void deferAutomaticDsPlay(void* activeGraphKey, void* buf,
                              DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags, void* caller) {
        if (!activeGraphKey) return;
        auto& s = ensureGraph(activeGraphKey);
        s.deferredPreparation.valid = true;
        s.deferredPreparation.transitionGeneration = s.transitionGeneration;
        s.deferredPreparation.hasDsPlay = true;
        s.deferredPreparation.targetDsBuffer = buf;
        s.deferredPreparation.dsReserved1 = dwReserved1;
        s.deferredPreparation.dsPriority = dwPriority;
        s.deferredPreparation.dsFlags = dwFlags;
        s.deferredPreparation.dsCaller = caller;
    }

    bool hasDeferredSeek(void* graphKey) const {
        const auto* s = findGraph(graphKey);
        return s ? s->deferredPreparation.hasSeek : false;
    }

    bool hasDeferredRun(void* graphKey) const {
        const auto* s = findGraph(graphKey);
        return s ? s->deferredPreparation.hasRun : false;
    }

    bool hasDeferredDsPlay(void* graphKey) const {
        const auto* s = findGraph(graphKey);
        return s ? s->deferredPreparation.hasDsPlay : false;
    }

    bool popDeferredNextTrackPreparation(void* graphKey, GraphState::DeferredNextTrackPreparation& outPrep) {
        if (!graphKey) return false;
        auto& s = ensureGraph(graphKey);
        if (s.transition.cancelled || !s.deferredPreparation.valid) return false;
        outPrep = s.deferredPreparation;
        s.deferredPreparation = GraphState::DeferredNextTrackPreparation{};
        s.deferredSeek = GraphState::DeferredSeekParams{};
        return true;
    }

    bool popDeferredSeek(void* graphKey, GraphState::DeferredSeekParams& outSeek) {
        if (!graphKey) return false;
        auto& s = ensureGraph(graphKey);
        if (s.transition.cancelled || (!s.deferredPreparation.hasSeek && !s.deferredSeek.valid)) return false;
        if (s.deferredPreparation.hasSeek) {
            outSeek.valid = true;
            outSeek.targetGraph = s.deferredPreparation.targetGraph;
            outSeek.targetSeeking = s.deferredPreparation.targetSeeking;
            outSeek.targetSeekingMarshalStream = s.deferredPreparation.targetSeekingMarshalStream;
            s.deferredPreparation.targetSeekingMarshalStream = nullptr; // ownership transferred
            outSeek.current = s.deferredPreparation.seekCurrent;
            outSeek.currentFlags = s.deferredPreparation.seekCurrentFlags;
            outSeek.stop = s.deferredPreparation.seekStop;
            outSeek.stopFlags = s.deferredPreparation.seekStopFlags;
            s.deferredPreparation.hasSeek = false;
        } else {
            outSeek = s.deferredSeek;
        }
        s.deferredSeek = GraphState::DeferredSeekParams{};
        return true;
    }

    bool onVirtualPauseCompleted(void* graphKey, PauseDeferralStatus status = PauseDeferralStatus::CompletedDrain) {
        if (!graphKey) return false;
        auto& s = ensureGraph(graphKey);
        if (s.transition.cancelled || s.transition.completed) return false;
        s.transition.completed = true;
        s.virtualPauseActive = false;
        s.formatChangePauseCompleted = true;
        s.formatChangePauseDeferred = false;
        s.pauseDeferralStatus = status;
        s.eosPending = false;
        return true;
    }

    void cancelVirtualPause(void* graphKey, PauseDeferralStatus status) {
        if (graphKey) {
            cancelFormatChangePause(graphKey, status);
        } else {
            for (auto& kv : graphs_) {
                if (kv.second.virtualPauseActive) {
                    cancelFormatChangePause(kv.first, status);
                }
            }
        }
    }

    void clearDeferredActions(void* graphKey) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.releaseDeferredStreams();
        s.deferredPreparation = GraphState::DeferredNextTrackPreparation{};
        s.deferredSeek = GraphState::DeferredSeekParams{};
        s.deferredDsStop = false;
        s.deferredGraphStop = false;
    }

    bool isCancelled(void* graphKey) const {
        const auto* s = findGraph(graphKey);
        return s ? s->isCancelled() : false;
    }

    void markFormatChangeStopDeferred(void* graphKey) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.formatChangeStopDeferred = true;
    }

    void onFormatChangeCompleted(void* graphKey) {
        if (!graphKey) return;
        auto& s = ensureGraph(graphKey);
        s.formatChangeCompleted = true;
        s.formatChangeStopDeferred = false;
        s.eosPending = false;
    }

    void onReset(void* graphKey, ResetReason reason) {
        if (graphKey) {
            auto it = graphs_.find(graphKey);
            if (it != graphs_.end()) {
                clearSegmentCounters(it->second, reason);
            }
            return;
        }
        for (auto& kv : graphs_) {
            clearSegmentCounters(kv.second, reason);
        }
    }

    void onSeek(void* graphKey) { onReset(graphKey, ResetReason::Seek); }
    void onBeginFlush(void* graphKey) { onReset(graphKey, ResetReason::BeginFlush); }
    void onEndFlush(void* graphKey) { onReset(graphKey, ResetReason::EndFlush); }
    void onStop(void* graphKey) { onReset(graphKey, ResetReason::Stop); }
    void onManualTrackChange(void* graphKey) { onReset(graphKey, ResetReason::ManualTrackChange); }

    const GraphState* findGraph(void* graphKey) const {
        auto it = graphs_.find(graphKey);
        return it != graphs_.end() ? &it->second : nullptr;
    }

    size_t graphCount() const { return graphs_.size(); }

private:
    static constexpr size_t kMaxTrackedGraphs = 64;
    std::map<void*, GraphState> graphs_;

    GraphState& ensureGraph(void* graphKey) {
        auto it = graphs_.find(graphKey);
        if (it != graphs_.end()) return it->second;
        if (graphs_.size() >= kMaxTrackedGraphs) {
            graphs_.erase(graphs_.begin());
        }
        return graphs_[graphKey];
    }

    static void applyPreReadDecision(GraphState& s, PreReadDecision decision) {
        s.preReadDecision = decision;
        if (decision == PreReadDecision::SameFormat24) {
            s.eosPending = false;
        } else if (decision == PreReadDecision::FormatChanged) {
            if (s.active24 && s.upstreamEosCount > 0 && !s.eosDelivered && s.targetBytes > 0) {
                s.eosPending = true;
            }
        }
    }

    static void clearSegmentCounters(GraphState& s, ResetReason reason) {
        if (s.isPauseDeferralActive()) {
            if (reason == ResetReason::Stop) {
                s.pauseDeferralStatus = PauseDeferralStatus::CancelledStop;
            } else if (reason == ResetReason::Seek) {
                s.pauseDeferralStatus = PauseDeferralStatus::CancelledSeek;
            } else if (reason == ResetReason::ManualTrackChange) {
                s.pauseDeferralStatus = PauseDeferralStatus::CancelledTrackChange;
            } else {
                s.pauseDeferralStatus = PauseDeferralStatus::None;
            }
        } else if (s.pauseDeferralStatus == PauseDeferralStatus::CancelledSeek ||
                   s.pauseDeferralStatus == PauseDeferralStatus::CancelledStop ||
                   s.pauseDeferralStatus == PauseDeferralStatus::CancelledTrackChange ||
                   s.pauseDeferralStatus == PauseDeferralStatus::CancelledManualPause ||
                   s.pauseDeferralStatus == PauseDeferralStatus::CancelledQuit) {
            // Preserve explicit cancellation status across segment counter clear
        } else {
            s.pauseDeferralStatus = PauseDeferralStatus::None;
        }
        s.upstreamSamples = 0;
        s.upstreamBytes = 0;
        s.upstreamLastEndTime = -1;
        s.upstreamHasTimestamp = false;
        s.upstreamEosCount = 0;
        s.targetBytes = 0;
        s.targetEndTime = -1;
        s.targetHasTimestamp = false;
        s.downstreamSamples = 0;
        s.downstreamBytes = 0;
        s.downstreamLastEndTime = -1;
        s.downstreamHasTimestamp = false;
        s.hasStopPosition = false;
        s.hasCurrentPosition = false;
        s.formatChangePauseDeferred = false;
        s.formatChangePauseCompleted = false;
        s.formatChangeStopDeferred = false;
        s.formatChangeCompleted = false;
        s.preReadDecision = PreReadDecision::Unknown;
        s.virtualPauseActive = false;
        s.autoTrackChangeInProgress = false;
        s.transition = GraphState::TransitionContext{};
        s.releaseDeferredSeekStream();
        s.deferredSeek = GraphState::DeferredSeekParams{};
        s.deferredDsStop = false;
        s.deferredGraphStop = false;
        s.eosPending = false;
        s.eosDelivered = false;
        s.lastResetReason = reason;
    }

    static bool downstreamReachedTarget(const GraphState& s) {
        if (s.targetBytes == 0 && !s.targetHasTimestamp) {
            return false;
        }
        bool bytesReached = (s.targetBytes > 0) && (s.downstreamBytes >= s.targetBytes);
        if (s.targetHasTimestamp && s.downstreamHasTimestamp) {
            bool timeReached = s.downstreamLastEndTime >= s.targetEndTime;
            return timeReached && (s.targetBytes == 0 || bytesReached);
        }
        return bytesReached;
    }

    static bool shouldDeliverDownstreamEos(const GraphState& s) {
        if (!s.active24 || !s.eosPending || s.eosDelivered || s.isCancelled()) {
            return false;
        }
        if (s.preReadDecision == PreReadDecision::SameFormat24) {
            return false;
        }
        if (s.upstreamEosCount == 0) {
            return false;
        }
        return downstreamReachedTarget(s);
    }
};

} // namespace eosTransition
