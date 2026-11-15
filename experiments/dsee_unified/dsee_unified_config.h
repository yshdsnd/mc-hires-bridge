#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace dseeUnified {

// DSEE HX configuration structure observed on x86 interface slot 3:
// this pointer + nine DWORDs passed by value (ret 0x28).
struct DseeConfig {
    DWORD mode;        // 0 = OFF/Bypass, 1 = DSEE HX ON
    DWORD channels;    // Typically 2
    DWORD format;      // 16 for standard PCM parameterization, 24 for 24-bit PCM tag
    DWORD inputRate;   // Decoded PCM sample rate (e.g. 44100, 48000, 96000, 192000)
    DWORD outputRate;  // Desired DSEE output sample rate
    DWORD inputBits;   // Decoded PCM bit depth (16 or 24)
    DWORD outputBits;  // Desired DSEE output bit depth (16 or 24)
    DWORD unknown;     // Reserved / padding, typically 0
    DWORD quality;     // DSEE quality preset, typically 0
};
static_assert(sizeof(DseeConfig) == 36, "DseeConfig must be 36 bytes");

enum class CodecType {
    Unknown,
    Flac,
    Aac,
    Alac,
    Wav,
    Mp3,
    Cd
};

inline const char* codecName(CodecType c) {
    switch (c) {
        case CodecType::Flac: return "FLAC";
        case CodecType::Aac:  return "AAC";
        case CodecType::Alac: return "ALAC";
        case CodecType::Wav:  return "WAV";
        case CodecType::Mp3:  return "MP3";
        case CodecType::Cd:   return "CD";
        default:              return "UNKNOWN";
    }
}

inline CodecType identifyCodecFromModule(const wchar_t* moduleName) {
    if (!moduleName) return CodecType::Unknown;
    if (_wcsicmp(moduleName, L"OpcFlac.dll") == 0) return CodecType::Flac;
    if (_wcsicmp(moduleName, L"OpcMp4.dll") == 0) return CodecType::Aac; // AAC/ALAC shared container module
    if (_wcsicmp(moduleName, L"OpcWavEx.dll") == 0) return CodecType::Wav;
    if (_wcsicmp(moduleName, L"OpcCd3.dll") == 0) return CodecType::Mp3; // CD / MP3 engine
    if (_wcsicmp(moduleName, L"OpcApe.dll") == 0) return CodecType::Flac;
    return CodecType::Unknown;
}

inline CodecType identifyCallerCodec(void* returnAddress) {
    if (!returnAddress) return CodecType::Unknown;
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(returnAddress), &m) || !m) {
        return CodecType::Unknown;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return identifyCodecFromModule(name ? name + 1 : path);
}

inline bool isTargetInputRate(DWORD rate) {
    return rate == 44100 || rate == 48000;
}

inline bool isTargetInputBits(DWORD bits) {
    return bits == 16 || bits == 24;
}

inline bool isHiResRate(DWORD rate) {
    return rate >= 88200;
}

// Harmonization mode for Issue #42
enum class HarmonizeMode {
    Family2x,   // 44.1k -> 88.2k/24b, 48k -> 96k/24b (integer 2x within family)
    Common96k,  // Request 96k/24b (48k -> 96k/24b; 44.1k DSEE internal integer multiple 176.4k/24b)
    Native24b,  // 44.1k -> 44.1k/24b, 48k -> 48k/24b (bit-depth expansion without rate change)
    Family4x    // 44.1k -> 176.4k/24b, 48k -> 192k/24b (maximum 4x integer upsampling)
};

// Computes adjusted DseeConfig for 44.1kHz/48kHz PCM when DSEE HX is ON
inline bool computeUnifiedDseeConfig(const DseeConfig& req, DseeConfig& adjusted, HarmonizeMode mode = HarmonizeMode::Family2x) {
    // Only adjust when DSEE HX is ON and input is 44.1kHz or 48kHz
    if (req.mode != 1) return false;
    if (!isTargetInputRate(req.inputRate)) return false;
    if (req.channels != 2) return false;

    adjusted = req;
    adjusted.mode = 1;
    adjusted.outputBits = 24;
    adjusted.format = 16; // Standard parameterization format

    switch (mode) {
        case HarmonizeMode::Common96k:
            // Issue #42 test: Request 96kHz/24bit common specification
            adjusted.outputRate = 96000;
            break;
        case HarmonizeMode::Native24b:
            adjusted.outputRate = req.inputRate;
            break;
        case HarmonizeMode::Family4x:
            adjusted.outputRate = (req.inputRate == 44100) ? 176400 : 192000;
            break;
        case HarmonizeMode::Family2x:
        default:
            adjusted.outputRate = (req.inputRate == 44100) ? 88200 : 96000;
            break;
    }
    return true;
}

// Graph switch & recreation tracking (Issue #42 Requirement 8)
enum class GraphTransitionType {
    None,
    Initial,
    Recreated,   // New graph builder instance created
    ReusedSameFormat, // Graph reused without format change
    ReusedFormatSwitched // Graph reused across format switch (same-graph track change)
};

struct GraphTracker {
    SRWLOCK lock = SRWLOCK_INIT;
    uint32_t generation = 0;
    void* activeGraph = nullptr;
    DWORD activeRate = 0;
    WORD activeBits = 0;
    CodecType activeCodec = CodecType::Unknown;
    uint64_t recreationCount = 0;
    uint64_t reuseCount = 0;

    GraphTransitionType onGraphEvent(void* graph, DWORD rate, WORD bits, CodecType codec) {
        AcquireSRWLockExclusive(&lock);
        GraphTransitionType type = GraphTransitionType::None;
        if (!activeGraph) {
            type = GraphTransitionType::Initial;
            ++generation;
            activeGraph = graph;
            activeRate = rate;
            activeBits = bits;
            activeCodec = codec;
        } else if (activeGraph != graph) {
            type = GraphTransitionType::Recreated;
            ++generation;
            ++recreationCount;
            activeGraph = graph;
            activeRate = rate;
            activeBits = bits;
            activeCodec = codec;
        } else {
            bool formatChanged = (activeRate != rate || activeBits != bits || activeCodec != codec);
            type = formatChanged ? GraphTransitionType::ReusedFormatSwitched : GraphTransitionType::ReusedSameFormat;
            ++reuseCount;
            activeRate = rate;
            activeBits = bits;
            activeCodec = codec;
        }
        ReleaseSRWLockExclusive(&lock);
        return type;
    }

    void reset() {
        AcquireSRWLockExclusive(&lock);
        generation = 0;
        activeGraph = nullptr;
        activeRate = 0;
        activeBits = 0;
        activeCodec = CodecType::Unknown;
        recreationCount = 0;
        reuseCount = 0;
        ReleaseSRWLockExclusive(&lock);
    }
};

} // namespace dseeUnified
