#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace trackTail {

constexpr uint32_t kDefaultRate = 48000;
constexpr uint16_t kStereoChannels = 2;
constexpr size_t kFrames300MsAt48k = 14400; // 0.300 s * 48000 Hz

struct Analysis {
    uint32_t rate = kDefaultRate;
    uint16_t bits = 24;
    uint16_t channels = kStereoChannels;
    size_t totalFrames = 0;
    size_t unalignedBytes = 0;
    size_t expectedTrack1Frames = 0;
    size_t track1ActiveFrames = 0;
    size_t track1LastFrame = 0;
    size_t missingTailFrames = 0;
    double missingTailMs = 0.0;
    size_t postEosSilenceFrames = 0;
    bool nextTrackDetected = false;
    size_t nextTrackStartFrame = 0;
    size_t nextTrackFrames = 0;
    bool tailComplete = false;
    bool approx300msMissing = false;
};

inline uint16_t blockAlign(uint16_t bits, uint16_t channels = kStereoChannels) {
    return static_cast<uint16_t>((bits / 8) * channels);
}

inline void writeSampleLE(uint8_t* dst, uint16_t bits, int32_t sample) {
    if (bits == 16) {
        int16_t s16 = static_cast<int16_t>(sample);
        std::memcpy(dst, &s16, sizeof(s16));
    } else if (bits == 24) {
        dst[0] = static_cast<uint8_t>(sample & 0xFF);
        dst[1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
        dst[2] = static_cast<uint8_t>((sample >> 16) & 0xFF);
    }
}

inline int32_t readSampleLE(const uint8_t* src, uint16_t bits) {
    if (bits == 16) {
        int16_t s16 = 0;
        std::memcpy(&s16, src, sizeof(s16));
        return static_cast<int32_t>(s16);
    }
    if (bits == 24) {
        uint32_t u = static_cast<uint32_t>(src[0]) |
                     (static_cast<uint32_t>(src[1]) << 8) |
                     (static_cast<uint32_t>(src[2]) << 16);
        if (u & 0x800000U) u |= 0xFF000000U;
        return static_cast<int32_t>(u);
    }
    return 0;
}

// Synthesize deterministic 48 kHz stereo 16/24-bit PCM without personal audio.
// Track 1 uses in-phase stereo (L == +R) with a boosted tail envelope in the final 300 ms.
// Track 2 uses anti-phase stereo (L == -R) so the analyzer can deterministically separate
// Track 1, post-EOS silence padding, and Next-track start.
inline void appendSynthesizedTrack(
    std::vector<uint8_t>& out,
    uint32_t rate,
    uint16_t bits,
    size_t frames,
    bool isNextTrack = false) {
    const uint16_t align = blockAlign(bits, kStereoChannels);
    const size_t startByte = out.size();
    out.resize(startByte + frames * align);

    const double fullScale = (bits == 24) ? 8388607.0 : 32767.0;
    const int32_t minMagnitude = (bits == 24) ? 4096 : 32;
    const size_t tailFrames = (rate > 0) ? (static_cast<size_t>(rate) * 3 / 10) : kFrames300MsAt48k;
    const size_t tailStart = (frames > tailFrames) ? (frames - tailFrames) : 0;
    const double baseFreq = isNextTrack ? 1750.0 : 997.0;
    const double harmonicFreq = isNextTrack ? 3500.0 : 2991.0;

    for (size_t i = 0; i < frames; ++i) {
        const bool inTail = !isNextTrack && (i >= tailStart);
        const double env = inTail ? 0.45 : 0.25;
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const double wave = 0.75 * std::sin(2.0 * 3.14159265358979323846 * baseFreq * t) +
                            0.25 * std::cos(2.0 * 3.14159265358979323846 * harmonicFreq * t);
        int32_t val = static_cast<int32_t>( env * wave * fullScale );
        if (val >= 0 && val < minMagnitude) val = minMagnitude;
        if (val < 0 && val > -minMagnitude) val = -minMagnitude;
        // For 24-bit, ensure low 8 bits are also populated so 24-bit precision is exercised.
        if (bits == 24 && (val & 0xFF) == 0) {
            val |= static_cast<int32_t>(((i * 73U) & 0x7FU) + 1U);
        }

        const int32_t left = val;
        const int32_t right = isNextTrack ? -val : val;
        uint8_t* framePtr = out.data() + startByte + i * align;
        writeSampleLE(framePtr, bits, left);
        writeSampleLE(framePtr + (bits / 8), bits, right);
    }
}

inline void appendSilence(std::vector<uint8_t>& out, uint16_t bits, size_t silenceFrames) {
    const uint16_t align = blockAlign(bits, kStereoChannels);
    out.resize(out.size() + silenceFrames * align, 0);
}

inline Analysis analyzeStream(
    const uint8_t* data,
    size_t byteCount,
    uint32_t rate,
    uint16_t bits,
    size_t expectedTrack1Frames) {
    Analysis r{};
    r.rate = rate;
    r.bits = bits;
    r.channels = kStereoChannels;
    r.expectedTrack1Frames = expectedTrack1Frames;

    const uint16_t align = blockAlign(bits, kStereoChannels);
    if (!data || align == 0 || rate == 0) return r;

    r.unalignedBytes = byteCount % align;
    r.totalFrames = byteCount / align;
    const uint16_t sampleBytes = bits / 8;

    size_t silenceAfterTrack1 = 0;
    size_t consecutiveOpposite = 0;
    size_t candidateNextStart = 0;
    for (size_t i = 0; i < r.totalFrames; ++i) {
        const uint8_t* framePtr = data + i * align;
        const int32_t left = readSampleLE(framePtr, bits);
        const int32_t right = readSampleLE(framePtr + sampleBytes, bits);

        if (left == 0 && right == 0) {
            consecutiveOpposite = 0;
            if (r.track1LastFrame > 0 && !r.nextTrackDetected) {
                ++silenceAfterTrack1;
            }
            continue;
        }

        const bool oppositeSign = (left > 0 && right < 0) || (left < 0 && right > 0);

        if (!r.nextTrackDetected) {
            if (oppositeSign && r.track1LastFrame > 0) {
                if (consecutiveOpposite == 0) candidateNextStart = i;
                ++consecutiveOpposite;
                if (consecutiveOpposite >= 4) {
                    r.nextTrackDetected = true;
                    r.nextTrackStartFrame = candidateNextStart;
                    r.nextTrackFrames = consecutiveOpposite;
                    // Adjust silenceAfterTrack1 to only count silence before candidateNextStart
                    const size_t gap = (candidateNextStart >= r.track1LastFrame)
                                           ? (candidateNextStart - r.track1LastFrame)
                                           : 0;
                    if (silenceAfterTrack1 > gap) silenceAfterTrack1 = gap;
                }
            } else {
                consecutiveOpposite = 0;
                ++r.track1ActiveFrames;
                r.track1LastFrame = i + 1;
                silenceAfterTrack1 = 0;
            }
        } else {
            ++r.nextTrackFrames;
        }
    }

    r.postEosSilenceFrames = silenceAfterTrack1;
    if (expectedTrack1Frames > r.track1LastFrame) {
        r.missingTailFrames = expectedTrack1Frames - r.track1LastFrame;
    } else {
        r.missingTailFrames = 0;
    }
    r.missingTailMs = static_cast<double>(r.missingTailFrames) * 1000.0 / static_cast<double>(rate);
    const size_t minActiveFrames = (expectedTrack1Frames * 95) / 100;
    r.tailComplete = (r.unalignedBytes == 0) &&
                     (r.missingTailFrames == 0) &&
                     (r.track1LastFrame >= expectedTrack1Frames) &&
                     (r.track1ActiveFrames >= minActiveFrames);
    r.approx300msMissing = (r.missingTailMs >= 200.0 && r.missingTailMs <= 400.0);
    return r;
}

} // namespace trackTail
