#pragma once

#include "track_tail_check.h"
#include "track_regression_compare.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace trackReference {

constexpr uint32_t kAnalysisRate = 24000; // Standard 24 kHz analysis domain (immune to DSEE >16kHz artifacts)

struct AudioBuffer {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    std::vector<float> samples; // Interleaved samples in [-1.0f, +1.0f]

    size_t frameCount() const {
        return (channels > 0) ? (samples.size() / channels) : 0;
    }

    double durationMs() const {
        if (sampleRate == 0 || channels == 0) return 0.0;
        return (static_cast<double>(frameCount()) * 1000.0) / static_cast<double>(sampleRate);
    }
};

struct MatchConfig {
    uint32_t analysisRate = kAnalysisRate;
    double templateDurationMs = 250.0;     // Tail template duration (250 ms)
    double headTemplateDurationMs = 100.0; // Next track head template duration (100 ms to catch short double starts)
    double correlationThreshold = 0.65;    // Minimum Pearson correlation for match
    double minCueSeparationMs = 40.0;      // Minimum separation between distinct cues
    double tailToleranceMs = 50.0;         // Tail completeness threshold (<= 50 ms missing)
    bool forcePending = false;             // Flag loopback discontinuity -> PENDING
};

struct MatchResult {
    bool track1Found = false;
    double track1StartMs = 0.0;
    double track1MatchedEndMs = 0.0;
    double track1ExpectedEndMs = 0.0;
    double missingTailMs = 0.0;
    bool tailComplete = false;
    uint32_t nextTrackCueCount = 0;
    std::vector<double> nextTrackCueTimestampsMs;
    double maxConfidence = 0.0;
    bool loopbackDiscontinuity = false;
    std::string verdict = "FAIL"; // PASS, FAIL, PENDING
    std::string notes;
};

// --- WAV Reader & Helper Functions ---

inline bool readWav(const std::string& path, AudioBuffer& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    char header[12];
    if (!file.read(header, 12)) return false;
    if (std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
        return false;
    }

    uint16_t formatTag = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    bool foundFmt = false;
    bool foundData = false;

    while (file) {
        char chunkId[4];
        uint32_t chunkSize = 0;
        if (!file.read(chunkId, 4)) break;
        if (!file.read(reinterpret_cast<char*>(&chunkSize), 4)) break;

        if (std::memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
            std::vector<uint8_t> fmtData(chunkSize);
            if (!file.read(reinterpret_cast<char*>(fmtData.data()), chunkSize)) return false;
            formatTag = *reinterpret_cast<uint16_t*>(fmtData.data());
            channels = *reinterpret_cast<uint16_t*>(fmtData.data() + 2);
            sampleRate = *reinterpret_cast<uint32_t*>(fmtData.data() + 4);
            bitsPerSample = *reinterpret_cast<uint16_t*>(fmtData.data() + 14);

            // Handle WAVE_FORMAT_EXTENSIBLE (0xFFFE)
            if (formatTag == 0xFFFE && chunkSize >= 40) {
                formatTag = *reinterpret_cast<uint16_t*>(fmtData.data() + 24); // SubFormat GUID Data1
            }
            foundFmt = true;
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            if (!foundFmt || channels == 0 || sampleRate == 0 || bitsPerSample == 0) return false;
            const size_t bytesPerSample = bitsPerSample / 8;
            const size_t totalSamples = chunkSize / bytesPerSample;
            out.sampleRate = sampleRate;
            out.channels = channels;
            out.samples.resize(totalSamples);

            std::vector<uint8_t> rawData(chunkSize);
            if (!file.read(reinterpret_cast<char*>(rawData.data()), chunkSize)) return false;

            if (formatTag == 1) { // PCM Integer
                if (bitsPerSample == 16) {
                    const int16_t* s16 = reinterpret_cast<const int16_t*>(rawData.data());
                    for (size_t i = 0; i < totalSamples; ++i) {
                        out.samples[i] = static_cast<float>(s16[i]) / 32768.0f;
                    }
                } else if (bitsPerSample == 24) {
                    for (size_t i = 0; i < totalSamples; ++i) {
                        const uint8_t* p = rawData.data() + i * 3;
                        int32_t val = static_cast<int32_t>(p[0]) |
                                      (static_cast<int32_t>(p[1]) << 8) |
                                      (static_cast<int32_t>(p[2]) << 16);
                        if (val & 0x800000) val |= 0xFF000000;
                        out.samples[i] = static_cast<float>(val) / 8388608.0f;
                    }
                } else if (bitsPerSample == 32) {
                    const int32_t* s32 = reinterpret_cast<const int32_t*>(rawData.data());
                    for (size_t i = 0; i < totalSamples; ++i) {
                        out.samples[i] = static_cast<float>(s32[i]) / 2147483648.0f;
                    }
                }
            } else if (formatTag == 3) { // IEEE Float
                if (bitsPerSample == 32) {
                    const float* f32 = reinterpret_cast<const float*>(rawData.data());
                    std::memcpy(out.samples.data(), f32, totalSamples * sizeof(float));
                }
            }
            foundData = true;
            break;
        } else {
            // Skip unknown chunk
            file.seekg(chunkSize, std::ios::cur);
        }
        // Word alignment padding
        if (chunkSize & 1) file.seekg(1, std::ios::cur);
    }

    return foundFmt && foundData;
}

inline bool writeWav(const std::string& path, const AudioBuffer& in, uint16_t outBits = 16) {
    if (in.samples.empty() || in.sampleRate == 0 || in.channels == 0) return false;
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    const size_t bytesPerSample = outBits / 8;
    const uint16_t blockAlign = in.channels * bytesPerSample;
    const uint32_t avgBytesPerSec = in.sampleRate * blockAlign;
    const uint32_t dataBytes = static_cast<uint32_t>(in.samples.size() * bytesPerSample);
    const uint32_t riffSize = 36 + dataBytes;

    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&riffSize), 4);
    file.write("WAVE", 4);

    file.write("fmt ", 4);
    uint32_t fmtSize = 16;
    file.write(reinterpret_cast<const char*>(&fmtSize), 4);
    uint16_t formatTag = (outBits == 32) ? 3 : 1; // 3 for IEEE float, 1 for PCM
    file.write(reinterpret_cast<const char*>(&formatTag), 2);
    file.write(reinterpret_cast<const char*>(&in.channels), 2);
    file.write(reinterpret_cast<const char*>(&in.sampleRate), 4);
    file.write(reinterpret_cast<const char*>(&avgBytesPerSec), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&outBits), 2);

    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataBytes), 4);

    if (outBits == 16) {
        std::vector<int16_t> buf(in.samples.size());
        for (size_t i = 0; i < in.samples.size(); ++i) {
            float s = std::max(-1.0f, std::min(1.0f, in.samples[i]));
            buf[i] = static_cast<int16_t>(s * 32767.0f);
        }
        file.write(reinterpret_cast<const char*>(buf.data()), dataBytes);
    } else if (outBits == 24) {
        for (size_t i = 0; i < in.samples.size(); ++i) {
            float s = std::max(-1.0f, std::min(1.0f, in.samples[i]));
            int32_t v = static_cast<int32_t>(s * 8388607.0f);
            uint8_t b[3] = {
                static_cast<uint8_t>(v & 0xFF),
                static_cast<uint8_t>((v >> 8) & 0xFF),
                static_cast<uint8_t>((v >> 16) & 0xFF)
            };
            file.write(reinterpret_cast<const char*>(b), 3);
        }
    } else if (outBits == 32) {
        file.write(reinterpret_cast<const char*>(in.samples.data()), dataBytes);
    }

    return true;
}

inline bool writeRawFloat32(const std::string& path, const AudioBuffer& in) {
    if (in.samples.empty()) return false;
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file.write(reinterpret_cast<const char*>(in.samples.data()), in.samples.size() * sizeof(float));
    return true;
}

inline bool readRawFloat32(const std::string& path, uint32_t sampleRate, uint16_t channels, AudioBuffer& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    std::streamsize fileSize = file.tellg();
    if (fileSize <= 0 || (fileSize % sizeof(float)) != 0) return false;

    file.seekg(0, std::ios::beg);
    const size_t totalSamples = static_cast<size_t>(fileSize / sizeof(float));

    out.sampleRate = sampleRate;
    out.channels = channels;
    out.samples.resize(totalSamples);
    if (!file.read(reinterpret_cast<char*>(out.samples.data()), fileSize)) return false;

    return true;
}

inline bool loadAudio(const std::string& path, uint32_t defaultRate, uint16_t defaultChannels, AudioBuffer& out) {
    // Check if file is a WAV file
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    char magic[4]{};
    file.read(magic, 4);
    file.close();

    if (std::memcmp(magic, "RIFF", 4) == 0) {
        return readWav(path, out);
    }
    // Default to raw float32
    return readRawFloat32(path, defaultRate, defaultChannels, out);
}

// Convert arbitrary audio buffer to mono resampled at targetRate (e.g. 24 kHz)
inline std::vector<float> toMonoResampled(const AudioBuffer& in, uint32_t targetRate = kAnalysisRate) {
    if (in.samples.empty() || in.sampleRate == 0 || in.channels == 0) return {};

    const size_t inFrames = in.frameCount();
    std::vector<float> inMono(inFrames);
    for (size_t i = 0; i < inFrames; ++i) {
        if (in.channels == 1) {
            inMono[i] = in.samples[i];
        } else {
            const float left = in.samples[i * in.channels];
            const float right = in.samples[i * in.channels + 1];
            const float mid = (left + right) * 0.5f;
            const float side = (left - right) * 0.5f;
            // Guard against anti-phase cancellation (L == -R)
            inMono[i] = (std::abs(mid) >= std::abs(side)) ? mid : side;
        }
    }

    if (in.sampleRate == targetRate) {
        return inMono;
    }

    // High precision linear interpolation resampling to targetRate (e.g. 24,000 Hz)
    const double ratio = static_cast<double>(in.sampleRate) / static_cast<double>(targetRate);
    const size_t outFrames = static_cast<size_t>(std::floor(static_cast<double>(inFrames) / ratio));
    std::vector<float> out(outFrames);

    for (size_t i = 0; i < outFrames; ++i) {
        const double srcPos = static_cast<double>(i) * ratio;
        const size_t baseIdx = static_cast<size_t>(srcPos);
        const double alpha = srcPos - static_cast<double>(baseIdx);

        if (baseIdx + 1 < inFrames) {
            out[i] = static_cast<float>((1.0 - alpha) * inMono[baseIdx] + alpha * inMono[baseIdx + 1]);
        } else if (baseIdx < inFrames) {
            out[i] = inMono[baseIdx];
        } else {
            out[i] = 0.0f;
        }
    }

    return out;
}

// Computes Pearson normalized cross-correlation of template against signal at offset
inline double computePccAt(
    const std::vector<float>& signal,
    size_t signalOffset,
    const std::vector<float>& tpl,
    double tplMean,
    double tplStdDev) {

    const size_t M = tpl.size();
    if (signalOffset + M > signal.size() || tplStdDev <= 1e-7) return 0.0;

    double sumW = 0.0;
    double sumW2 = 0.0;
    for (size_t k = 0; k < M; ++k) {
        const double w = signal[signalOffset + k];
        sumW += w;
        sumW2 += w * w;
    }

    const double meanW = sumW / static_cast<double>(M);
    const double varW = (sumW2 / static_cast<double>(M)) - (meanW * meanW);
    if (varW <= 1e-12) return 0.0; // Silence or zero variance
    const double stdDevW = std::sqrt(varW);

    double cross = 0.0;
    for (size_t k = 0; k < M; ++k) {
        cross += (signal[signalOffset + k] - meanW) * (tpl[k] - tplMean);
    }

    return (cross / static_cast<double>(M)) / (stdDevW * tplStdDev);
}

// Scans signal for correlation peaks of template above threshold
inline void findTemplatePeaks(
    const std::vector<float>& signal,
    size_t searchStart,
    size_t searchEnd,
    const std::vector<float>& tpl,
    double threshold,
    size_t minSeparation,
    std::vector<size_t>& outPeaks,
    std::vector<double>& outScores) {

    const size_t M = tpl.size();
    if (tpl.empty() || searchStart >= signal.size() || searchStart + M > signal.size()) return;
    if (searchEnd > signal.size() - M) searchEnd = signal.size() - M;
    if (searchStart >= searchEnd) return;

    double sumT = 0.0;
    double sumT2 = 0.0;
    for (float v : tpl) {
        sumT += v;
        sumT2 += v * v;
    }
    const double meanT = sumT / static_cast<double>(M);
    const double varT = (sumT2 / static_cast<double>(M)) - (meanT * meanT);
    if (varT <= 1e-12) return;
    const double stdDevT = std::sqrt(varT);

    std::vector<double> scores(searchEnd - searchStart, 0.0);
    // Coarse scan with stride 2 for efficiency, then refine
    for (size_t idx = searchStart; idx < searchEnd; ++idx) {
        scores[idx - searchStart] = computePccAt(signal, idx, tpl, meanT, stdDevT);
    }

    // Local peak detection
    for (size_t i = 1; i + 1 < scores.size(); ++i) {
        const double s = scores[i];
        if (s >= threshold && s > scores[i - 1] && s >= scores[i + 1]) {
            const size_t peakIdx = searchStart + i;
            if (!outPeaks.empty() && (peakIdx - outPeaks.back()) < minSeparation) {
                // Keep the higher peak if within minSeparation
                if (s > outScores.back()) {
                    outPeaks.back() = peakIdx;
                    outScores.back() = s;
                }
            } else {
                outPeaks.push_back(peakIdx);
                outScores.push_back(s);
            }
        }
    }
}

// Computes best Pearson correlation in a small window around target offset
inline double findBestPcc(
    const std::vector<float>& signal,
    size_t targetOffset,
    size_t jitterFrames,
    const std::vector<float>& tpl,
    double tplMean,
    double tplStdDev) {

    if (tpl.empty()) return 0.0;
    const size_t M = tpl.size();
    if (signal.size() < M) return 0.0;

    const size_t start = (targetOffset > jitterFrames) ? (targetOffset - jitterFrames) : 0;
    const size_t end = std::min(signal.size() - M, targetOffset + jitterFrames);
    if (start > end) return 0.0;

    double bestScore = -1.0;
    for (size_t pos = start; pos <= end; ++pos) {
        double score = computePccAt(signal, pos, tpl, tplMean, tplStdDev);
        if (score > bestScore) bestScore = score;
    }
    return bestScore;
}

struct ProbeDef {
    double offsetMs;
    double durationMs;
};

// Verifies whether loopback signal following candidatePos continues along R2 across multi-stage probes
inline bool verifyContinuation(
    const std::vector<float>& sig,
    size_t candidatePos,
    const std::vector<float>& r2,
    size_t r2Start,
    uint32_t rate,
    double threshold,
    size_t& outMatchedProbes,
    size_t& outTotalProbes,
    double& outAvgScore) {

    // Multi-stage probe checkpoints along Track 2 (150ms, 300ms, 600ms, 1000ms, 1400ms)
    const ProbeDef probes[] = {
        { 150.0, 100.0 },
        { 300.0, 100.0 },
        { 600.0, 150.0 },
        { 1000.0, 200.0 },
        { 1400.0, 200.0 }
    };

    outMatchedProbes = 0;
    outTotalProbes = 0;
    outAvgScore = 0.0;
    double totalScoreSum = 0.0;

    const size_t jitterFrames = static_cast<size_t>(5.0 * rate / 1000.0);

    for (const auto& pr : probes) {
        const size_t offsetFrames = static_cast<size_t>(pr.offsetMs * rate / 1000.0);
        const size_t durFrames = static_cast<size_t>(pr.durationMs * rate / 1000.0);

        if (r2Start + offsetFrames + durFrames > r2.size() ||
            candidatePos + offsetFrames + durFrames > sig.size()) {
            continue;
        }

        const size_t tplStart = r2Start + offsetFrames;
        if (tplStart + durFrames > r2.size()) continue;

        std::vector<float> probeTpl(durFrames);
        std::memcpy(probeTpl.data(), r2.data() + tplStart, durFrames * sizeof(float));

        double sumP = 0.0, sumP2 = 0.0;
        for (float v : probeTpl) { sumP += v; sumP2 += v * v; }
        const double meanP = sumP / static_cast<double>(durFrames);
        const double varP = (sumP2 / static_cast<double>(durFrames)) - (meanP * meanP);
        if (varP <= 1e-12) continue;
        const double stdDevP = std::sqrt(varP);

        outTotalProbes++;
        double bestPcc = findBestPcc(sig, candidatePos + offsetFrames, jitterFrames, probeTpl, meanP, stdDevP);
        totalScoreSum += std::max(0.0, bestPcc);

        if (bestPcc >= threshold) {
            outMatchedProbes++;
        }
    }

    if (outTotalProbes == 0) return false;
    outAvgScore = totalScoreSum / static_cast<double>(outTotalProbes);

    if (outTotalProbes == 1) return (outMatchedProbes == 1 && outAvgScore >= threshold);
    if (outTotalProbes == 2) return (outMatchedProbes == 2 && outAvgScore >= (threshold - 0.05));
    return (outMatchedProbes * 4 >= outTotalProbes * 3) && (outAvgScore >= (threshold - 0.05));
}

// Extracts active audio template (skipping any initial silence in reference)
inline std::vector<float> extractTemplate(
    const std::vector<float>& src,
    size_t startFrame,
    size_t frameCount) {

    if (src.empty()) return {};
    if (startFrame >= src.size()) startFrame = 0;
    if (startFrame + frameCount > src.size()) {
        frameCount = src.size() - startFrame;
    }
    std::vector<float> tpl(frameCount);
    std::memcpy(tpl.data(), src.data() + startFrame, frameCount * sizeof(float));
    return tpl;
}

// Main reference comparison engine
inline MatchResult evaluateLoopback(
    const AudioBuffer& loopback,
    const AudioBuffer& ref1,
    const AudioBuffer& ref2,
    const MatchConfig& cfg = MatchConfig{}) {

    MatchResult res{};
    res.loopbackDiscontinuity = cfg.forcePending;

    if (cfg.forcePending) {
        res.verdict = "PENDING";
        res.notes = "Loopback discontinuity across transition boundary; verdict pending re-run";
        return res;
    }

    if (loopback.samples.empty() || ref1.samples.empty() || ref2.samples.empty()) {
        res.verdict = "PENDING";
        res.notes = "Insufficient input audio data (empty buffer)";
        return res;
    }

    // Resample all streams to common analysis rate (24 kHz) mono
    const std::vector<float> sig = toMonoResampled(loopback, cfg.analysisRate);
    const std::vector<float> r1 = toMonoResampled(ref1, cfg.analysisRate);
    const std::vector<float> r2 = toMonoResampled(ref2, cfg.analysisRate);

    const size_t tplFrames = static_cast<size_t>(cfg.templateDurationMs * cfg.analysisRate / 1000.0);
    const size_t headTplFrames = static_cast<size_t>(cfg.headTemplateDurationMs * cfg.analysisRate / 1000.0);
    const size_t minSepFrames = static_cast<size_t>(cfg.minCueSeparationMs * cfg.analysisRate / 1000.0);

    // 1. Locate Track 1 in Loopback (skipping leading silence)
    // Use an anchor template from the early-middle portion of Track 1 (e.g. 500ms into Track 1)
    const size_t anchorStart = (r1.size() > tplFrames * 3) ? (tplFrames * 2) : 0;
    const std::vector<float> t1Anchor = extractTemplate(r1, anchorStart, tplFrames);

    std::vector<size_t> t1Peaks;
    std::vector<double> t1Scores;
    findTemplatePeaks(sig, 0, sig.size(), t1Anchor, cfg.correlationThreshold, minSepFrames, t1Peaks, t1Scores);

    if (t1Peaks.empty()) {
        res.verdict = "PENDING";
        res.notes = "Track 1 onset not confidently matched in loopback";
        return res;
    }

    res.track1Found = true;
    res.maxConfidence = t1Scores[0];
    const size_t t1AnchorIdxInLoop = t1Peaks[0];
    const size_t t1StartInLoop = (t1AnchorIdxInLoop >= anchorStart) ? (t1AnchorIdxInLoop - anchorStart) : 0;
    res.track1StartMs = static_cast<double>(t1StartInLoop) * 1000.0 / cfg.analysisRate;

    const size_t expectedT1EndInLoop = t1StartInLoop + r1.size();
    res.track1ExpectedEndMs = static_cast<double>(expectedT1EndInLoop) * 1000.0 / cfg.analysisRate;

    // 2. Evaluate Track 1 Tail Completeness
    // Extract tail template from the final portion of Track 1
    const size_t tailStartInRef1 = (r1.size() > tplFrames) ? (r1.size() - tplFrames) : 0;
    const std::vector<float> t1Tail = extractTemplate(r1, tailStartInRef1, tplFrames);

    const size_t expectedTailPeak = t1StartInLoop + tailStartInRef1;
    const size_t searchMargin = static_cast<size_t>(400.0 * cfg.analysisRate / 1000.0);
    const size_t tailSearchStart = (expectedTailPeak > searchMargin) ? (expectedTailPeak - searchMargin) : 0;
    const size_t tailSearchEnd = expectedTailPeak + searchMargin;

    std::vector<size_t> tailPeaks;
    std::vector<double> tailScores;
    findTemplatePeaks(sig, tailSearchStart, tailSearchEnd, t1Tail, cfg.correlationThreshold, minSepFrames, tailPeaks, tailScores);

    if (!tailPeaks.empty()) {
        const size_t matchedTailEndInLoop = tailPeaks[0] + tplFrames;
        res.track1MatchedEndMs = static_cast<double>(matchedTailEndInLoop) * 1000.0 / cfg.analysisRate;
        if (expectedT1EndInLoop >= matchedTailEndInLoop) {
            const size_t missingFrames = expectedT1EndInLoop - matchedTailEndInLoop;
            res.missingTailMs = static_cast<double>(missingFrames) * 1000.0 / cfg.analysisRate;
        } else {
            res.missingTailMs = 0.0;
        }
        res.tailComplete = (res.missingTailMs <= cfg.tailToleranceMs);
    } else {
        // Tail template not found near expected end -> Truncation detected.
        // Search backward along ref1 to determine where the tail was cut off.
        const size_t probeFrames = static_cast<size_t>(100.0 * cfg.analysisRate / 1000.0);
        size_t lastMatchedEnd = t1StartInLoop;
        const size_t stepFrames = static_cast<size_t>(25.0 * cfg.analysisRate / 1000.0);

        for (size_t offset = (r1.size() > probeFrames ? r1.size() - probeFrames : 0);
             offset >= probeFrames;
             offset = (offset > stepFrames ? offset - stepFrames : 0)) {

            std::vector<float> probe = extractTemplate(r1, offset, probeFrames);
            const size_t loopTarget = t1StartInLoop + offset;
            if (loopTarget + probeFrames <= sig.size()) {
                double meanP = 0.0, meanP2 = 0.0;
                for (float v : probe) { meanP += v; meanP2 += v * v; }
                meanP /= probeFrames;
                double stdDevP = std::sqrt(std::max(0.0, (meanP2 / probeFrames) - meanP * meanP));
                double score = computePccAt(sig, loopTarget, probe, meanP, stdDevP);
                if (score >= cfg.correlationThreshold) {
                    lastMatchedEnd = loopTarget + probeFrames;
                    break;
                }
            }
            if (offset == 0) break;
        }

        res.track1MatchedEndMs = static_cast<double>(lastMatchedEnd) * 1000.0 / cfg.analysisRate;
        if (expectedT1EndInLoop > lastMatchedEnd) {
            res.missingTailMs = static_cast<double>(expectedT1EndInLoop - lastMatchedEnd) * 1000.0 / cfg.analysisRate;
        } else {
            res.missingTailMs = 0.0;
        }
        res.tailComplete = (res.missingTailMs <= cfg.tailToleranceMs);
    }

    // 3. Evaluate Next Track (Track 2) Cue Occurrences & Trajectory Verification
    // Find active start of ref2 (skip initial zero-padding if any)
    size_t ref2ActiveStart = 0;
    while (ref2ActiveStart < r2.size() && std::abs(r2[ref2ActiveStart]) < 0.001f) {
        ++ref2ActiveStart;
    }
    const std::vector<float> t2Head = extractTemplate(r2, ref2ActiveStart, headTplFrames);

    // Search for Track 2 start candidates after Track 1 tail
    const size_t t2SearchStart = (res.track1MatchedEndMs > 100.0)
                                     ? static_cast<size_t>((res.track1MatchedEndMs - 100.0) * cfg.analysisRate / 1000.0)
                                     : t1StartInLoop;
    const size_t t2SearchEnd = sig.size();

    std::vector<size_t> candidatePeaks;
    std::vector<double> candidateScores;
    findTemplatePeaks(sig, t2SearchStart, t2SearchEnd, t2Head, cfg.correlationThreshold, minSepFrames, candidatePeaks, candidateScores);

    if (candidatePeaks.empty()) {
        res.nextTrackCueCount = 0;
        if (sig.size() <= t2SearchStart + headTplFrames) {
            res.verdict = "PENDING";
            res.notes = "Insufficient loopback duration after Track 1 to detect next track onset (判定保留)";
        } else {
            res.verdict = "FAIL";
            res.notes = "Next track start not detected";
        }
        return res;
    }

    // Identify the continuous playback anchor (p_cont) via multi-stage continuation probes along Track 2
    size_t bestContIdx = static_cast<size_t>(-1);
    double bestContScore = -1.0;
    size_t maxTestedProbes = 0;

    for (size_t i = 0; i < candidatePeaks.size(); ++i) {
        const size_t p = candidatePeaks[i];
        size_t matched = 0, total = 0;
        double avgScore = 0.0;
        bool verified = verifyContinuation(sig, p, r2, ref2ActiveStart, cfg.analysisRate, cfg.correlationThreshold, matched, total, avgScore);
        if (total > maxTestedProbes) maxTestedProbes = total;
        if (verified && avgScore > bestContScore) {
            bestContScore = avgScore;
            bestContIdx = i;
        }
    }

    if (bestContIdx == static_cast<size_t>(-1)) {
        // No candidate confirmed as continuous playback.
        // Check whether this is due to insufficient recording length (evidence shortage -> PENDING / 判定保留)
        const size_t requiredContFrames = static_cast<size_t>(250.0 * cfg.analysisRate / 1000.0);
        if (maxTestedProbes == 0 || (sig.size() - candidatePeaks[0] < requiredContFrames)) {
            res.verdict = "PENDING";
            res.notes = "Insufficient loopback duration after next track onset to confirm continuous playback trajectory (判定保留)";
            return res;
        }
        res.nextTrackCueCount = static_cast<uint32_t>(candidatePeaks.size());
        for (size_t p : candidatePeaks) {
            const size_t cueStart = (p >= ref2ActiveStart) ? (p - ref2ActiveStart) : 0;
            res.nextTrackCueTimestampsMs.push_back(static_cast<double>(cueStart) * 1000.0 / cfg.analysisRate);
        }
        res.verdict = "FAIL";
        res.notes = "Next track onset detected but playback discontinued without continuous trajectory";
        return res;
    }

    const size_t p_cont = candidatePeaks[bestContIdx];
    res.maxConfidence = std::max(res.maxConfidence, bestContScore);

    // Differentiate between:
    // (a) Ongoing similar fragments / repeating rhythm motifs (通常再生中の似た断片) -> part of continuous trajectory, not cues
    // (b) True short restart / duplicate cueing (真の短い再スタート) -> aborted prior cues or subsequent restarts
    std::vector<size_t> finalCues;
    const size_t jitterFrames = static_cast<size_t>(5.0 * cfg.analysisRate / 1000.0);

    for (size_t i = 0; i < candidatePeaks.size(); ++i) {
        const size_t p = candidatePeaks[i];

        if (p == p_cont) {
            finalCues.push_back(p);
            continue;
        }

        if (p > p_cont) {
            // Candidate occurs AFTER confirmed continuous start.
            // Check if loopback at p is explained by ongoing continuous playback trajectory from p_cont:
            const size_t offsetFromCont = p - p_cont;
            if (ref2ActiveStart + offsetFromCont + headTplFrames <= r2.size()) {
                std::vector<float> trajTpl = extractTemplate(r2, ref2ActiveStart + offsetFromCont, headTplFrames);
                double sumT = 0.0, sumT2 = 0.0;
                for (float v : trajTpl) { sumT += v; sumT2 += v * v; }
                const double meanT = sumT / static_cast<double>(headTplFrames);
                const double varT = (sumT2 / static_cast<double>(headTplFrames)) - (meanT * meanT);
                double trajPcc = 0.0;
                if (varT > 1e-12) {
                    const double stdDevT = std::sqrt(varT);
                    trajPcc = findBestPcc(sig, p, jitterFrames, trajTpl, meanT, stdDevT);
                }

                size_t matched = 0, total = 0;
                double avgScore = 0.0;
                bool restartsFromZero = verifyContinuation(sig, p, r2, ref2ActiveStart, cfg.analysisRate, cfg.correlationThreshold, matched, total, avgScore);

                if (trajPcc >= (cfg.correlationThreshold - 0.15) && !restartsFromZero) {
                    // Confirmed as ongoing similar fragment / repeating beat along trajectory (not a restart)
                    continue;
                }

                if (restartsFromZero && trajPcc < 0.50) {
                    // True subsequent restart from 0
                    finalCues.push_back(p);
                    continue;
                }
            }

            // Fallback: if it verifies continuation from 0 and does not match ongoing trajectory
            size_t matched = 0, total = 0;
            double avgScore = 0.0;
            if (verifyContinuation(sig, p, r2, ref2ActiveStart, cfg.analysisRate, cfg.correlationThreshold, matched, total, avgScore)) {
                finalCues.push_back(p);
            }
        } else {
            // Candidate occurs BEFORE confirmed continuous start (p < p_cont).
            // Check if this is an aborted prior cue (真の短い再スタート):
            if (p_cont - p < minSepFrames) {
                // Within separation window of p_cont, considered part of same transient
                continue;
            }

            // Check multi-interval consistency of head template (複数区間の連続した整合):
            // Divide head into two sequential halves [0..50ms] and [50..100ms]
            const size_t halfFrames = headTplFrames / 2;
            if (ref2ActiveStart + headTplFrames <= r2.size()) {
                std::vector<float> head1 = extractTemplate(r2, ref2ActiveStart, halfFrames);
                std::vector<float> head2 = extractTemplate(r2, ref2ActiveStart + halfFrames, halfFrames);

                double mean1 = 0.0, var1 = 0.0;
                for (float v : head1) mean1 += v;
                mean1 /= static_cast<double>(halfFrames);
                for (float v : head1) var1 += (v - mean1) * (v - mean1);
                var1 /= static_cast<double>(halfFrames);
                double stdDev1 = (var1 > 1e-12) ? std::sqrt(var1) : 0.0;

                double mean2 = 0.0, var2 = 0.0;
                for (float v : head2) mean2 += v;
                mean2 /= static_cast<double>(halfFrames);
                for (float v : head2) var2 += (v - mean2) * (v - mean2);
                var2 /= static_cast<double>(halfFrames);
                double stdDev2 = (var2 > 1e-12) ? std::sqrt(var2) : 0.0;

                double pcc1 = (stdDev1 > 0.0) ? findBestPcc(sig, p, jitterFrames, head1, mean1, stdDev1) : 0.0;
                double pcc2 = (stdDev2 > 0.0) ? findBestPcc(sig, p + halfFrames, jitterFrames, head2, mean2, stdDev2) : 0.0;

                if (pcc1 >= 0.50 && pcc2 >= 0.50 && ((pcc1 + pcc2) * 0.5) >= (cfg.correlationThreshold - 0.05)) {
                    // Confirmed genuine aborted prior cue
                    finalCues.push_back(p);
                }
            }
        }
    }

    std::sort(finalCues.begin(), finalCues.end());
    res.nextTrackCueCount = static_cast<uint32_t>(finalCues.size());
    res.nextTrackCueTimestampsMs.clear();
    for (size_t p : finalCues) {
        const size_t cueStart = (p >= ref2ActiveStart) ? (p - ref2ActiveStart) : 0;
        res.nextTrackCueTimestampsMs.push_back(static_cast<double>(cueStart) * 1000.0 / cfg.analysisRate);
    }

    // 4. Formulate Verdict
    if (res.nextTrackCueCount == 0) {
        res.verdict = "FAIL";
        res.notes = "Next track start not detected";
    } else if (res.nextTrackCueCount >= 2) {
        res.verdict = "FAIL";
        std::ostringstream ss;
        ss << "Duplicate cueing detected (二重頭出し: " << res.nextTrackCueCount << " cues at ";
        for (size_t i = 0; i < res.nextTrackCueTimestampsMs.size(); ++i) {
            ss << std::fixed << std::setprecision(1) << res.nextTrackCueTimestampsMs[i] << "ms";
            if (i + 1 < res.nextTrackCueTimestampsMs.size()) ss << ", ";
        }
        ss << ")";
        res.notes = ss.str();
    } else if (!res.tailComplete) {
        res.verdict = "FAIL";
        std::ostringstream ss;
        ss << "Track 1 tail truncated (~" << std::fixed << std::setprecision(1) << res.missingTailMs << " ms missing)";
        res.notes = ss.str();
    } else {
        res.verdict = "PASS";
        res.notes = "Track 1 tail complete, Next track single start verified";
    }

    return res;
}

// Converts MatchResult to trackRegression::Record for standardized output formatting
inline trackRegression::Record toRegressionRecord(
    const std::string& caseId,
    const std::string& scenario,
    const std::string& transition,
    const MatchResult& m,
    uint32_t sampleRate = 192000,
    uint16_t bitDepth = 32) {

    trackRegression::Record r;
    r.testCaseId = caseId;
    r.scenario = scenario;
    r.formatTransition = transition;
    r.sampleRate = sampleRate;
    r.bitDepth = bitDepth;
    r.track1EffectiveEndMs = m.track1MatchedEndMs;
    r.missingTailMs = m.missingTailMs;
    r.tailComplete = m.tailComplete;
    r.nextTrackDetected = (m.nextTrackCueCount > 0);
    r.nextTrackStartMs = (!m.nextTrackCueTimestampsMs.empty()) ? m.nextTrackCueTimestampsMs[0] : 0.0;
    r.postEosSilenceMs = (r.nextTrackStartMs > m.track1MatchedEndMs) ? (r.nextTrackStartMs - m.track1MatchedEndMs) : 0.0;
    r.passed = (m.verdict == "PASS");
    r.verdict = m.verdict;
    r.notes = m.notes;
    return r;
}

// Synthesizes a test reference waveform with dynamic frequency modulation (chirp).
// Optionally embeds recurring rhythm transient motifs (e.g. at 0ms, 120ms, 160ms, 1000ms)
// to verify trajectory continuity vs false cue rejection under realistic musical structures.
inline AudioBuffer synthesizeReferenceTrack(
    uint32_t rate,
    double durationSec,
    double baseFreq,
    double modRate,
    bool antiPhase = false,
    bool addRepeatingMotifs = false) {

    AudioBuffer b;
    b.sampleRate = rate;
    b.channels = 2;
    const size_t frames = static_cast<size_t>(durationSec * rate);
    b.samples.resize(frames * 2);

    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const double chirpFreq = baseFreq + 400.0 * (t / durationSec);
        double wave = 0.60 * std::sin(2.0 * 3.14159265358979323846 * chirpFreq * t) +
                      0.20 * std::sin(4.0 * 3.14159265358979323846 * chirpFreq * t);

        if (addRepeatingMotifs) {
            // Embed recurring rhythm transient motifs at 0ms, 120ms, 160ms, 1000ms
            auto addPulse = [&](double pulseStartSec) {
                if (t >= pulseStartSec && t < pulseStartSec + 0.050) {
                    const double pt = t - pulseStartSec;
                    const double env = std::exp(-pt / 0.012);
                    wave += 0.50 * env * std::sin(2.0 * 3.14159265358979323846 * 880.0 * pt);
                }
            };
            addPulse(0.0);
            addPulse(0.120);
            addPulse(0.160);
            addPulse(1.000);
        }

        const float left = static_cast<float>(wave * 0.40);
        const float right = antiPhase ? -left : left;
        b.samples[i * 2] = left;
        b.samples[i * 2 + 1] = right;
    }
    return b;
}

// Appends reference-based synthetic test cases to regression suite (TC-4.1 to TC-4.6)
inline void appendSyntheticReferenceTests(std::vector<trackRegression::Record>& suite) {
    // Generate reference tracks:
    // Ref 1: 44.1 kHz / 16-bit representation (3.0s, base 997 Hz chirp)
    AudioBuffer ref1 = synthesizeReferenceTrack(44100, 3.0, 997.0, 1.0, false);
    // Ref 2: 48.0 kHz / 24-bit representation (2.0s, base 1750 Hz chirp, antiPhase, with repeating rhythm motifs)
    AudioBuffer ref2 = synthesizeReferenceTrack(48000, 2.0, 1750.0, 1.0, true, true);

    const uint32_t loopRate = 192000;
    const size_t leadSilenceFrames = loopRate / 5; // 200 ms leading silence
    const size_t gapSilenceFrames = loopRate / 20;  // 50 ms gap

    AudioBuffer ref1_192 = synthesizeReferenceTrack(loopRate, 3.0, 997.0, 1.0, false);
    AudioBuffer ref2_192 = synthesizeReferenceTrack(loopRate, 2.0, 1750.0, 1.0, true, true);

    auto appendSamples = [](AudioBuffer& dst, const AudioBuffer& src, size_t maxFrames) {
        size_t frames = std::min(maxFrames, src.frameCount());
        dst.samples.insert(dst.samples.end(), src.samples.begin(), src.samples.begin() + frames * 2);
    };

    // --- TC-4.1: Normal Transition with Repeating Motifs (通常再生中の似た断片) ---
    // Loopback: [200ms silence] + [Ref1 full (3.0s)] + [50ms silence] + [Ref2 continuous (1.5s)]
    // Evaluates that ongoing peaks at +120ms, +160ms, +1000ms are confirmed as continuous trajectory (1 cue)
    {
        AudioBuffer loop;
        loop.sampleRate = loopRate;
        loop.channels = 2;
        loop.samples.resize(leadSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref1_192, ref1_192.frameCount());
        loop.samples.resize(loop.samples.size() + gapSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref2_192, static_cast<size_t>(1.5 * loopRate));

        MatchResult m = evaluateLoopback(loop, ref1, ref2);
        suite.push_back(toRegressionRecord("TC-4.1", "4. Ref-Based Normal (Motifs)", "44.1k/16b -> 48k/24b", m));
    }

    // --- TC-4.2: Tail Truncation with Repeating Motifs (~300ms tail missing) ---
    // Loopback: [200ms silence] + [Ref1 cut to 2.7s] + [50ms silence] + [Ref2 continuous (1.5s)]
    // Evaluates that tail truncation is isolated and repeating motifs do not generate false duplicate cues
    {
        AudioBuffer loop;
        loop.sampleRate = loopRate;
        loop.channels = 2;
        loop.samples.resize(leadSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref1_192, static_cast<size_t>(2.7 * loopRate));
        loop.samples.resize(loop.samples.size() + gapSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref2_192, static_cast<size_t>(1.5 * loopRate));

        MatchResult m = evaluateLoopback(loop, ref1, ref2);
        suite.push_back(toRegressionRecord("TC-4.2", "4. Ref-Based Cutoff (Motifs)", "44.1k/16b -> 48k/24b", m));
    }

    // --- TC-4.3: True Short Restart / Duplicate Cueing (真の短い再スタート) ---
    // Loopback: [200ms silence] + [Ref1 full (3.0s)] + [50ms silence] + [Ref2 start 120ms] + [Ref2 restarts from 0 and plays 1.5s]
    // Evaluates that aborted initial cue at T0 and continuous start at T1 are correctly detected (2 cues)
    {
        AudioBuffer loop;
        loop.sampleRate = loopRate;
        loop.channels = 2;
        loop.samples.resize(leadSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref1_192, ref1_192.frameCount());
        loop.samples.resize(loop.samples.size() + gapSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref2_192, static_cast<size_t>(0.12 * loopRate)); // First aborted cue: 120ms
        appendSamples(loop, ref2_192, static_cast<size_t>(1.5 * loopRate));  // Second restarted cue: 1.5s

        MatchResult m = evaluateLoopback(loop, ref1, ref2);
        suite.push_back(toRegressionRecord("TC-4.3", "4. Ref-Based Double Cue", "44.1k/16b -> 48k/24b", m));
    }

    // --- TC-4.4: Loopback Discontinuity -> PENDING (判定保留) ---
    {
        AudioBuffer loop;
        loop.sampleRate = loopRate;
        loop.channels = 2;
        loop.samples.resize(leadSilenceFrames * 2, 0.0f);

        MatchConfig cfg;
        cfg.forcePending = true;

        MatchResult m = evaluateLoopback(loop, ref1, ref2, cfg);
        suite.push_back(toRegressionRecord("TC-4.4", "4. Ref-Based Discontinuity", "44.1k/16b -> 48k/24b", m));
    }

    // --- TC-4.5: Cross-Format Pair of Same Song (同じ楽曲の異形式ペア) ---
    // Ref 1: 44.1k/16b representation; Ref 2: 48k/24b representation of the same musical piece
    // Verifies that format/depth differences between reference WAV and 192k loopback do not induce false cues
    {
        AudioBuffer loop;
        loop.sampleRate = loopRate;
        loop.channels = 2;
        loop.samples.resize(leadSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref1_192, ref1_192.frameCount());
        loop.samples.resize(loop.samples.size() + gapSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref2_192, static_cast<size_t>(1.5 * loopRate));

        MatchResult m = evaluateLoopback(loop, ref1, ref2);
        suite.push_back(toRegressionRecord("TC-4.5", "4. Ref-Based Cross-Format", "44.1k/16b -> 48k/24b", m));
    }

    // --- TC-4.6: Insufficient Loopback Duration -> PENDING (判定保留) ---
    // Loopback ends shortly (150ms) after Track 2 onset; onset is matched but trajectory cannot be confirmed
    {
        AudioBuffer loop;
        loop.sampleRate = loopRate;
        loop.channels = 2;
        loop.samples.resize(leadSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref1_192, ref1_192.frameCount());
        loop.samples.resize(loop.samples.size() + gapSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref2_192, static_cast<size_t>(0.15 * loopRate)); // Truncated capture (only 150ms into T2)

        MatchResult m = evaluateLoopback(loop, ref1, ref2);
        suite.push_back(toRegressionRecord("TC-4.6", "4. Ref-Based Short Capture", "44.1k/16b -> 48k/24b", m));
    }
}

// Generates synthetic WAV and raw test fixtures on disk for offline/CLI validation
inline bool generateFixtures(const std::string& dir) {
    AudioBuffer ref1 = synthesizeReferenceTrack(44100, 3.0, 997.0, 1.0, false);
    AudioBuffer ref2 = synthesizeReferenceTrack(48000, 2.0, 1750.0, 1.0, true, true);

    std::string ref1Path = dir + "/ref1_44k16b.wav";
    std::string ref2Path = dir + "/ref2_48k24b.wav";
    if (!writeWav(ref1Path, ref1, 16)) return false;
    if (!writeWav(ref2Path, ref2, 24)) return false;

    const uint32_t loopRate = 192000;
    const size_t leadSilenceFrames = loopRate / 5;
    const size_t gapSilenceFrames = loopRate / 20;

    AudioBuffer ref1_192 = synthesizeReferenceTrack(loopRate, 3.0, 997.0, 1.0, false);
    AudioBuffer ref2_192 = synthesizeReferenceTrack(loopRate, 2.0, 1750.0, 1.0, true, true);

    auto appendSamples = [](AudioBuffer& dst, const AudioBuffer& src, size_t maxFrames) {
        size_t frames = std::min(maxFrames, src.frameCount());
        dst.samples.insert(dst.samples.end(), src.samples.begin(), src.samples.begin() + frames * 2);
    };

    // Normal (with repeating motifs)
    {
        AudioBuffer loop;
        loop.sampleRate = loopRate;
        loop.channels = 2;
        loop.samples.resize(leadSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref1_192, ref1_192.frameCount());
        loop.samples.resize(loop.samples.size() + gapSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref2_192, static_cast<size_t>(1.5 * loopRate));
        writeRawFloat32(dir + "/loopback_normal_192k.raw", loop);
    }

    // Cutoff
    {
        AudioBuffer loop;
        loop.sampleRate = loopRate;
        loop.channels = 2;
        loop.samples.resize(leadSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref1_192, static_cast<size_t>(2.7 * loopRate));
        loop.samples.resize(loop.samples.size() + gapSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref2_192, static_cast<size_t>(1.5 * loopRate));
        writeRawFloat32(dir + "/loopback_cutoff_192k.raw", loop);
    }

    // Double Cue
    {
        AudioBuffer loop;
        loop.sampleRate = loopRate;
        loop.channels = 2;
        loop.samples.resize(leadSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref1_192, ref1_192.frameCount());
        loop.samples.resize(loop.samples.size() + gapSilenceFrames * 2, 0.0f);
        appendSamples(loop, ref2_192, static_cast<size_t>(0.12 * loopRate));
        appendSamples(loop, ref2_192, static_cast<size_t>(1.5 * loopRate));
        writeRawFloat32(dir + "/loopback_doublecue_192k.raw", loop);
    }

    return true;
}

} // namespace trackReference
