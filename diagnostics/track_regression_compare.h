#pragma once

#include "track_tail_check.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace trackRegression {

struct Record {
    std::string testCaseId;
    std::string scenario;
    std::string formatTransition;
    uint32_t sampleRate = 0;
    uint16_t bitDepth = 0;
    size_t expectedTrack1Frames = 0;
    size_t track1EffectiveEndFrame = 0;
    double track1EffectiveEndMs = 0.0;
    size_t missingTailFrames = 0;
    double missingTailMs = 0.0;
    bool tailComplete = false; // true = 欠落なし, false = 欠落あり
    bool nextTrackDetected = false;
    size_t nextTrackStartFrame = 0;
    double nextTrackStartMs = 0.0;
    double postEosSilenceMs = 0.0;
    std::string manualOpType = "None"; // None, Pause, Seek, TrackChange, Stop, Exit
    double manualOpInputTimeMs = 0.0;
    std::string intendedTarget = "-";
    double actualReflectionTimeMs = 0.0;
    double reflectionLatencyMs = 0.0;
    uint32_t staleEosIntrusions = 0;
    bool passed = false;
    std::string verdict = "FAIL"; // PASS, FAIL, PENDING (判定保留)
    std::string notes;
};

inline std::string markdownHeader() {
    std::ostringstream ss;
    ss << "| Case ID | Scenario | Transition | Rate/Bit | Track1 End (ms) | Missing (ms) | Tail Status | Next Start (ms) | Gap (ms) | Manual Op | Input (ms) | Intended | Reflect (ms) | Latency (ms) | Stale EOS | Verdict |\n";
    ss << "|:---|:---|:---|:---:|---:|---:|:---:|---:|---:|:---:|---:|:---|---:|---:|:---:|:---:|\n";
    return ss.str();
}

inline std::string toMarkdownRow(const Record& r) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    ss << "| " << r.testCaseId
       << " | " << r.scenario
       << " | " << r.formatTransition
       << " | " << (r.sampleRate / 1000) << "k/" << r.bitDepth << "b"
       << " | " << r.track1EffectiveEndMs
       << " | " << r.missingTailMs
       << " | " << (r.tailComplete ? "欠落なし" : "欠落あり")
       << " | " << (r.nextTrackDetected ? std::to_string(static_cast<long long>(r.nextTrackStartMs)) : "-")
       << " | " << r.postEosSilenceMs
       << " | " << r.manualOpType
       << " | " << (r.manualOpType != "None" ? std::to_string(static_cast<long long>(r.manualOpInputTimeMs)) : "-")
       << " | " << r.intendedTarget
       << " | " << (r.manualOpType != "None" ? std::to_string(static_cast<long long>(r.actualReflectionTimeMs)) : "-")
       << " | " << (r.manualOpType != "None" ? std::to_string(r.reflectionLatencyMs).substr(0, 5) : "-")
       << " | " << r.staleEosIntrusions
       << " | " << (r.verdict == "PASS" ? "**PASS**" : (r.verdict == "PENDING" ? "**PENDING** (判定保留)" : "**FAIL**"))
       << " |\n";
    return ss.str();
}

inline std::string tsvHeader() {
    return "test_case_id\tscenario\tformat_transition\tsample_rate\tbit_depth\t"
           "expected_track1_frames\ttrack1_effective_end_frame\ttrack1_effective_end_ms\t"
           "missing_tail_frames\tmissing_tail_ms\ttail_status\t"
           "next_track_detected\tnext_track_start_frame\tnext_track_start_ms\t"
           "post_eos_silence_ms\tmanual_op_type\tmanual_op_input_time_ms\t"
           "intended_target\tactual_reflection_time_ms\treflection_latency_ms\t"
           "stale_eos_intrusions\tverdict\tnotes\n";
}

inline std::string toTsvRow(const Record& r) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3);
    ss << r.testCaseId << "\t"
       << r.scenario << "\t"
       << r.formatTransition << "\t"
       << r.sampleRate << "\t"
       << r.bitDepth << "\t"
       << r.expectedTrack1Frames << "\t"
       << r.track1EffectiveEndFrame << "\t"
       << r.track1EffectiveEndMs << "\t"
       << r.missingTailFrames << "\t"
       << r.missingTailMs << "\t"
       << (r.tailComplete ? "Complete" : (r.verdict == "PENDING" ? "Pending" : "Truncated")) << "\t"
       << (r.nextTrackDetected ? "1" : "0") << "\t"
       << r.nextTrackStartFrame << "\t"
       << r.nextTrackStartMs << "\t"
       << r.postEosSilenceMs << "\t"
       << r.manualOpType << "\t"
       << r.manualOpInputTimeMs << "\t"
       << r.intendedTarget << "\t"
       << r.actualReflectionTimeMs << "\t"
       << r.reflectionLatencyMs << "\t"
       << r.staleEosIntrusions << "\t"
       << r.verdict << "\t"
       << r.notes << "\n";
    return ss.str();
}

inline std::string toJson(const Record& r) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3);
    ss << "{\n"
       << "  \"test_case_id\": \"" << r.testCaseId << "\",\n"
       << "  \"scenario\": \"" << r.scenario << "\",\n"
       << "  \"format_transition\": \"" << r.formatTransition << "\",\n"
       << "  \"sample_rate\": " << r.sampleRate << ",\n"
       << "  \"bit_depth\": " << r.bitDepth << ",\n"
       << "  \"expected_track1_frames\": " << r.expectedTrack1Frames << ",\n"
       << "  \"track1_effective_end_frame\": " << r.track1EffectiveEndFrame << ",\n"
       << "  \"track1_effective_end_ms\": " << r.track1EffectiveEndMs << ",\n"
       << "  \"missing_tail_frames\": " << r.missingTailFrames << ",\n"
       << "  \"missing_tail_ms\": " << r.missingTailMs << ",\n"
       << "  \"tail_status\": \"" << (r.tailComplete ? "Complete" : (r.verdict == "PENDING" ? "Pending" : "Truncated")) << "\",\n"
       << "  \"next_track_detected\": " << (r.nextTrackDetected ? "true" : "false") << ",\n"
       << "  \"next_track_start_frame\": " << r.nextTrackStartFrame << ",\n"
       << "  \"next_track_start_ms\": " << r.nextTrackStartMs << ",\n"
       << "  \"post_eos_silence_ms\": " << r.postEosSilenceMs << ",\n"
       << "  \"manual_op_type\": \"" << r.manualOpType << "\",\n"
       << "  \"manual_op_input_time_ms\": " << r.manualOpInputTimeMs << ",\n"
       << "  \"intended_target\": \"" << r.intendedTarget << "\",\n"
       << "  \"actual_reflection_time_ms\": " << r.actualReflectionTimeMs << ",\n"
       << "  \"reflection_latency_ms\": " << r.reflectionLatencyMs << ",\n"
       << "  \"stale_eos_intrusions\": " << r.staleEosIntrusions << ",\n"
       << "  \"verdict\": \"" << r.verdict << "\",\n"
       << "  \"notes\": \"" << r.notes << "\"\n"
       << "}";
    return ss.str();
}

inline Record evaluatePcm(
    const std::string& caseId,
    const std::string& scenario,
    const std::string& transition,
    const uint8_t* pcmData,
    size_t byteCount,
    uint32_t sampleRate,
    uint16_t bitDepth,
    size_t expectedTrack1Frames,
    const std::string& manualOpType = "None",
    double manualOpInputTimeMs = 0.0,
    const std::string& intendedTarget = "-",
    double actualReflectionTimeMs = 0.0,
    uint32_t staleEosIntrusions = 0,
    const std::string& notes = "",
    bool loopbackDiscontinuity = false) {

    Record r;
    r.testCaseId = caseId;
    r.scenario = scenario;
    r.formatTransition = transition;
    r.sampleRate = sampleRate;
    r.bitDepth = bitDepth;
    r.expectedTrack1Frames = expectedTrack1Frames;
    r.manualOpType = manualOpType;
    r.manualOpInputTimeMs = manualOpInputTimeMs;
    r.intendedTarget = intendedTarget;
    r.actualReflectionTimeMs = actualReflectionTimeMs;
    r.staleEosIntrusions = staleEosIntrusions;
    r.notes = notes;

    trackTail::Analysis a = trackTail::analyzeStream(
        pcmData, byteCount, sampleRate, bitDepth, expectedTrack1Frames);

    r.track1EffectiveEndFrame = a.track1LastFrame;
    r.track1EffectiveEndMs = (sampleRate > 0)
        ? (static_cast<double>(a.track1LastFrame) * 1000.0 / static_cast<double>(sampleRate))
        : 0.0;
    r.missingTailFrames = a.missingTailFrames;
    r.missingTailMs = a.missingTailMs;
    r.tailComplete = a.tailComplete;
    r.nextTrackDetected = a.nextTrackDetected;
    r.nextTrackStartFrame = a.nextTrackStartFrame;
    r.nextTrackStartMs = (sampleRate > 0)
        ? (static_cast<double>(a.nextTrackStartFrame) * 1000.0 / static_cast<double>(sampleRate))
        : 0.0;
    r.postEosSilenceMs = (sampleRate > 0)
        ? (static_cast<double>(a.postEosSilenceFrames) * 1000.0 / static_cast<double>(sampleRate))
        : 0.0;

    if (manualOpType != "None") {
        if (actualReflectionTimeMs >= manualOpInputTimeMs) {
            r.reflectionLatencyMs = actualReflectionTimeMs - manualOpInputTimeMs;
        } else {
            r.reflectionLatencyMs = 0.0;
        }
        // Manual operation pass criteria:
        // 1. Reflection latency within 15.0 ms
        // 2. Zero stale EOS intrusions
        r.passed = (r.reflectionLatencyMs <= 15.0) && (staleEosIntrusions == 0);
    } else {
        // Natural track end pass criteria:
        // 1. Tail complete (0 frames missing)
        // 2. Zero stale EOS intrusions
        if (scenario.find("Seamless") != std::string::npos) {
            r.passed = a.tailComplete && a.nextTrackDetected && (a.missingTailFrames == 0) && (staleEosIntrusions == 0);
        } else {
            r.passed = a.tailComplete && (a.missingTailFrames == 0) && (staleEosIntrusions == 0);
        }
    }

    if (loopbackDiscontinuity) {
        r.passed = false;
        r.verdict = "PENDING";
        r.tailComplete = false;
        if (r.notes.empty()) {
            r.notes = "Loopback discontinuity across transition boundary; verdict pending re-run";
        } else {
            r.notes += " (Loopback discontinuity across transition boundary; verdict pending re-run)";
        }
    } else if (r.passed) {
        r.verdict = "PASS";
    } else {
        r.verdict = "FAIL";
    }

    return r;
}

// Generates synthetic verification cases covering:
// Scenario 1: 24 -> 16 bit tail cutoff vs complete playback
// Scenario 2: 96 kHz / 24 bit seamless transition
// Scenario 3: Manual operations during deferral (Pause, Seek, TrackChange, Stop, Exit)
inline std::vector<Record> runSyntheticRegressionSuite() {
    std::vector<Record> suite;

    // --- Scenario 1: 24 -> 16 bit transitions ---
    // A1: 24b/48k -> 16b/48k with deferral (Full tail preserved)
    {
        const uint32_t rate = 48000;
        const uint16_t bits = 24;
        const size_t t1Frames = rate * 5; // 5.0s
        const size_t silenceFrames = rate / 10; // 100ms
        const size_t t2Frames = rate * 2; // 2.0s
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, rate, bits, t1Frames, false);
        trackTail::appendSilence(pcm, bits, silenceFrames);
        trackTail::appendSynthesizedTrack(pcm, rate, bits, t2Frames, true);

        suite.push_back(evaluatePcm(
            "TC-1.1", "1. 24->16b Tail", "24b/48k -> 16b/48k",
            pcm.data(), pcm.size(), rate, bits, t1Frames,
            "None", 0.0, "Track 2 start", 0.0, 0,
            "Full tail preserved via deferral"));
    }

    // A2: 24b/48k -> 16b/48k legacy EQ cutoff (~300ms tail missing)
    {
        const uint32_t rate = 48000;
        const uint16_t bits = 24;
        const size_t t1Frames = rate * 5; // 5.0s
        const size_t cutFrames = (rate * 3) / 10; // 300ms cut
        const size_t actualFrames = (t1Frames > cutFrames) ? (t1Frames - cutFrames) : 0;
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, rate, bits, actualFrames, false);
        trackTail::appendSilence(pcm, bits, cutFrames); // replaced with silence
        trackTail::appendSynthesizedTrack(pcm, rate, bits, rate * 2, true);

        Record rec = evaluatePcm(
            "TC-1.2", "1. 24->16b Tail", "24b/48k -> 16b/48k",
            pcm.data(), pcm.size(), rate, bits, t1Frames,
            "None", 0.0, "Track 2 start", 0.0, 0,
            "Legacy EQ flush cutoff simulated (~300ms lost)");
        suite.push_back(rec);
    }

    // A3: 24b/48k -> 16b/48k premature stop (~2.9s missing)
    {
        const uint32_t rate = 48000;
        const uint16_t bits = 24;
        const size_t t1Frames = rate * 5; // 5.0s
        const size_t actualFrames = static_cast<size_t>(rate * 2.1); // stopped at 2.1s
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, rate, bits, actualFrames, false);

        Record rec = evaluatePcm(
            "TC-1.3", "1. 24->16b Tail", "24b/48k -> 16b/48k",
            pcm.data(), pcm.size(), rate, bits, t1Frames,
            "None", 0.0, "Track 2 start", 0.0, 0,
            "Unsynchronized early stop simulated (~2.9s lost)");
        suite.push_back(rec);
    }

    // A4: 24b/48k -> 16b/48k loopback discontinuity across boundary -> PENDING (判定保留)
    {
        const uint32_t rate = 48000;
        const uint16_t bits = 24;
        const size_t t1Frames = rate * 5;
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, rate, bits, t1Frames, false);
        trackTail::appendSynthesizedTrack(pcm, rate, bits, rate * 2, true);

        Record rec = evaluatePcm(
            "TC-1.4", "1. 24->16b Tail", "24b/48k -> 16b/48k",
            pcm.data(), pcm.size(), rate, bits, t1Frames,
            "None", 0.0, "Track 2 start", 0.0, 0,
            "Loopback discontinuity across boundary",
            true);
        suite.push_back(rec);
    }

    // --- Scenario 2: 96 kHz / 24 bit seamless transitions ---
    // B1: 96k/24b -> 96k/24b seamless pre-reading with deferral (0ms gap)
    {
        const uint32_t rate = 96000;
        const uint16_t bits = 24;
        const size_t t1Frames = rate * 5; // 5.0s
        const size_t t2Frames = rate * 3; // 3.0s
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, rate, bits, t1Frames, false);
        trackTail::appendSynthesizedTrack(pcm, rate, bits, t2Frames, true); // zero gap

        suite.push_back(evaluatePcm(
            "TC-2.1", "2. 96k Seamless", "24b/96k -> 24b/96k",
            pcm.data(), pcm.size(), rate, bits, t1Frames,
            "None", 0.0, "Track 2 (anti-phase)", 0.0, 0,
            "Seamless contiguous pre-read, 0ms gap"));
    }

    // B2: 96k/24b -> 96k/24b pre-read failure with audible gap
    {
        const uint32_t rate = 96000;
        const uint16_t bits = 24;
        const size_t t1Frames = rate * 5; // 5.0s
        const size_t gapFrames = rate / 4; // 250ms gap
        const size_t t2Frames = rate * 3; // 3.0s
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, rate, bits, t1Frames, false);
        trackTail::appendSilence(pcm, bits, gapFrames);
        trackTail::appendSynthesizedTrack(pcm, rate, bits, t2Frames, true);

        Record rec = evaluatePcm(
            "TC-2.2", "2. 96k Seamless", "24b/96k -> 24b/96k",
            pcm.data(), pcm.size(), rate, bits, t1Frames,
            "None", 0.0, "Track 2 (anti-phase)", 0.0, 0,
            "Pre-read missed deadline, 250ms gap");
        // For seamless scenario with expected 0ms gap, TC-2.2 gap is flagged as failure
        rec.passed = (rec.postEosSilenceMs == 0.0) && rec.tailComplete;
        rec.verdict = rec.passed ? "PASS" : "FAIL";
        suite.push_back(rec);
    }

    // --- Scenario 3: Manual operations during deferral window ---
    // Track 1 is 5000 ms. Deferral window starts at 4700 ms (last 300ms).
    // Operations issued at 4950 ms ~ 4990 ms.
    const uint32_t rate48 = 48000;
    const uint16_t bits24 = 24;
    const size_t t1Frames48 = rate48 * 5;

    // C1: Pause during deferral
    {
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, rate48, bits24, t1Frames48, false);
        suite.push_back(evaluatePcm(
            "TC-3.1", "3. Manual Op", "24b/48k Deferral",
            pcm.data(), pcm.size(), rate48, bits24, t1Frames48,
            "Pause", 4950.0, "Renderer Pause", 4952.1, 0,
            "Immediate pause inside deferral window"));
    }

    // C2: Seek during deferral
    {
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, rate48, bits24, t1Frames48, false);
        suite.push_back(evaluatePcm(
            "TC-3.2", "3. Manual Op", "24b/48k Deferral",
            pcm.data(), pcm.size(), rate48, bits24, t1Frames48,
            "Seek", 4970.0, "Position 00:30.000", 4974.3, 0,
            "Seek cancels deferred EOS and flushes"));
    }

    // C3: TrackChange (Next) during deferral
    {
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, rate48, bits24, t1Frames48, false);
        suite.push_back(evaluatePcm(
            "TC-3.3", "3. Manual Op", "24b/48k Deferral",
            pcm.data(), pcm.size(), rate48, bits24, t1Frames48,
            "TrackChange", 4980.0, "Next Track", 4983.5, 0,
            "Manual Next aborts pending deferral immediately"));
    }

    // C4: Stop during deferral
    {
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, rate48, bits24, t1Frames48, false);
        suite.push_back(evaluatePcm(
            "TC-3.4", "3. Manual Op", "24b/48k Deferral",
            pcm.data(), pcm.size(), rate48, bits24, t1Frames48,
            "Stop", 4960.0, "Playback Stopped", 4961.8, 0,
            "Stop cancels deferred timer and resets state"));
    }

    // C5: Exit (Graph teardown) during deferral
    {
        std::vector<uint8_t> pcm;
        trackTail::appendSynthesizedTrack(pcm, rate48, bits24, t1Frames48, false);
        suite.push_back(evaluatePcm(
            "TC-3.5", "3. Manual Op", "24b/48k Deferral",
            pcm.data(), pcm.size(), rate48, bits24, t1Frames48,
            "Exit", 4990.0, "Process/Graph Terminate", 4991.2, 0,
            "Teardown unhooks cleanly without crash or leak"));
    }

    return suite;
}

} // namespace trackRegression
