#pragma once
// Experimental segment-based EndOfStream (EOS) state machine for format-changing
// track transitions. No fixed track duration (e.g. 23s) assumptions.
#include <windows.h>
#include <dshow.h>
#include <cstdint>
#include <map>

namespace eosTransition {

enum class PreReadDecision {
    Unknown,        // Next track format not yet determined (or 0x31E7A not called)
    SameFormat24,   // Next track is 24-bit stereo at the same sample rate (preserve seamless pre-read)
    FormatChanged   // Next track differs in bit depth, sample rate, or channel count
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

    PreReadDecision preReadDecision = PreReadDecision::Unknown;
    bool eosPending = false;
    bool eosDelivered = false;
    uint32_t downstreamEosDeliveredCount = 0;
    uint32_t seamlessTransitionCount = 0;
    ResetReason lastResetReason = ResetReason::NewSegment;
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
        if (s.eosDelivered) {
            return false;
        }
        s.eosPending = false;
        s.eosDelivered = true;
        ++s.downstreamEosDeliveredCount;
        return true;
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
        s.preReadDecision = PreReadDecision::Unknown;
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
        if (!s.active24 || !s.eosPending || s.eosDelivered) {
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
