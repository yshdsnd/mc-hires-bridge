# DSEE HX ON Unified 24-bit Output Specification & Investigation

## 1. Overview and Problem Statement

When DSEE HX is enabled in Sony Music Center for PC (x86), standard resolution tracks (44.1kHz and 48kHz PCM, across 16-bit and 24-bit sources) are processed by Sony's DSEE HX upsampling filter (`OmgDseeHxFilter.ax`).
Under default operation:
- 44.1kHz/16-bit and 48kHz/16-bit inputs are constrained to 16-bit PCM output.
- Downstream DirectShow audio graphs route through `OmgEqxlFilter.ax` (Graphic Equalizer), which strictly rejects 24-bit PCM as well as 88.2kHz and 176.4kHz sample rates (`0x80040207: VFW_E_NOT_CONNECTED`).
- Track transitions across different sampling rates (44.1kHz ↔ 48kHz), bit depths (16-bit ↔ 24-bit), and audio container formats (FLAC, AAC, MP3, ALAC, WAV) trigger downstream audio device reconfiguration in the Windows DirectSound shared engine.

Issue #42 specifies:
1. When DSEE HX is ON: Harmonize 44.1kHz and 48kHz PCM outputs to 24-bit across codecs.
2. Investigate whether 96kHz/24-bit can serve as a universal target across both 44.1kHz and 48kHz rate families.
3. Bypass `OmgEqxlFilter.ax` when 24-bit output is negotiated so that the 24-bit stream reaches DirectSound renderer unhindered.
4. Maintain bit-perfect original playback when DSEE HX is OFF or for native Hi-Res audio (>= 88.2kHz, 96kHz, 192kHz, DSD).
5. Track and differentiate DirectShow graph lifecycle during track transitions (`GRAPH_RECREATED` vs `GRAPH_REUSED`).

---

## 2. DSEE HX Universal Control via Slot 3

PE scan across all Sony.Earth player libraries revealed that every decoder DLL invokes slot 3 of interface `{6AF76DBE-8634-4FD3-93ED-982EDE1BFDD5}` on `CLSID_DseeHxFilter` (`{A481D160-AB4D-4B97-93FE-C1F4B4B781DF}`):

| Calling Module | Container / Codec | TimeDateStamp | Slot 3 Call Site RVA |
| :--- | :--- | :--- | :--- |
| `OpcFlac.dll` | FLAC | `0x63EB12E0` | Intercepted via COM vtable slot 3 |
| `OpcMp4.dll` | AAC / ALAC | `0x63EB12E2` | Intercepted via COM vtable slot 3 |
| `OpcWavEx.dll` | WAV (PCM) | `0x63EB12E7` | Intercepted via COM vtable slot 3 |
| `OpcCd3.dll` | Audio CD / MP3 | `0x63EB12DA` | Intercepted via COM vtable slot 3 |
| `OpcDSPlay.dll` | Shared Player Engine | `0x63EB1303` | Intercepted via COM vtable slot 3 |

Hooking slot 3 of `OmgDseeHxFilter.ax` (`RVA 0x36CA`, TimeDateStamp `0x63EB12C1`) provides a single centralized control point that governs DSEE HX output format across **all codecs**.

### Interface Parameter Layout

The method signature on x86 stdcall:
```cpp
struct DseeConfig {
    DWORD mode;        // 0 = OFF/Bypass, 1 = DSEE HX ON
    DWORD channels;    // Typically 2 (stereo)
    DWORD format;      // 16 = standard PCM parameterization tag
    DWORD inputRate;   // Decoded PCM sample rate (e.g. 44100, 48000)
    DWORD outputRate;  // Desired DSEE output sample rate
    DWORD inputBits;   // Decoded PCM bit depth (16 or 24)
    DWORD outputBits;  // Desired DSEE output bit depth (16 or 24)
    DWORD unknown;     // Reserved (0)
    DWORD quality;     // Quality preset (0)
};
```

---

## 3. Sample Rate Harmonization Investigation Results (Requirement 3)

We tested setting the common target `outputRate = 96000` and `outputBits = 24` on real Sony filters with synthetic test audio files:

### Live Test Matrix & Observed Behavior

| Input Stream | Target Config Requested | Setter Return | Negotiated DirectSound Format | Observed DSP Behavior |
| :--- | :--- | :--- | :--- | :--- |
| **48kHz / 16-bit** | `96000 Hz, 24-bit` | `S_OK` | **`96000 Hz, 24-bit, 2ch`** | Exact 2x integer oversampling. Clean connection. |
| **48kHz / 24-bit** | `96000 Hz, 24-bit` | `S_OK` | **`96000 Hz, 24-bit, 2ch`** | Exact 2x integer oversampling with 24-bit depth preserved. |
| **44.1kHz / 16-bit** | `96000 Hz, 24-bit` | `S_OK` | **`176400 Hz, 24-bit, 2ch`** | `OmgDseeHxFilter` DSP **does not support cross-family fractional resampling** (44.1k → 96k). It clamps to the nearest 4x integer multiple (176.4kHz). |
| **44.1kHz / 24-bit** | `96000 Hz, 24-bit` | `S_OK` | **`176400 Hz, 24-bit, 2ch`** | Same DSP clamp to 176.4kHz/24-bit. |
| **44.1kHz / 16-bit** | `88200 Hz, 24-bit` | `S_OK` | **`88200 Hz, 24-bit, 2ch`** | Exact 2x integer oversampling within 44.1kHz family. |
| **44.1kHz / 16-bit** | `44100 Hz, 24-bit` | `S_OK` | **`44100 Hz, 24-bit, 2ch`** | Native sample rate preserved; bit depth expanded to 24-bit. |
| **48kHz / 16-bit** | `48000 Hz, 24-bit` | `S_OK` | **`48000 Hz, 24-bit, 2ch`** | Native sample rate preserved; bit depth expanded to 24-bit. |

### Technical Conclusion for Requirement 3
- `OmgDseeHxFilter` is strictly an **integer-multiple upsampling filter** (1x, 2x, 4x within the input rate family). Cross-family resampling (e.g. 44.1kHz → 96kHz or 48kHz → 88.2kHz) is not implemented in Sony's DSP.
- Setting 96kHz for 44.1kHz causes DSEE HX to output 176.4kHz/24-bit.
- Consequently, two optimal rate harmonization modes are provided:
  1. **`Family2x` (Default / Recommended)**:
     - 44.1kHz family → **88.2kHz / 24-bit**
     - 48.0kHz family → **96.0kHz / 24-bit**
     Provides harmonic integer-ratio upsampling with minimal DSP distortion.
  2. **`Native24b`**:
     - 44.1kHz family → **44.1kHz / 24-bit**
     - 48.0kHz family → **48.0kHz / 24-bit**
     Provides unified 24-bit depth without sample rate alteration.

---

## 4. Equalizer Bypass Mechanism (Requirement 4)

DirectShow graph inspection confirmed that Sony's graph topology is:
```
[AsyncReader] -> [Decoder/Parser] -> [OmgDseeHxFilter] -> [SeamlessFilter] -> [OmgEqxlFilter (EQ)] -> [DirectSoundRender]
```
`OmgEqxlFilter.ax` rejects all 24-bit PCM and rates above 48kHz.
`dseeUnified::applyEqBypass(IGraphBuilder* graph)` performs dynamic topology rewiring prior to graph start:
1. Disconnects `SeamlessFilter` output pin from `OmgEqxlFilter` input pin.
2. Disconnects `OmgEqxlFilter` output pin from `DirectSoundRender` input pin.
3. Directly connects `SeamlessFilter` output pin to `DirectSoundRender` input pin.
4. If `ConnectDirect` fails, an automatic rollback reconnects `SeamlessFilter -> OmgEqxlFilter -> DirectSoundRender` to prevent playback stall.

---

## 5. Preservation of Native Hi-Res & DSEE OFF (Requirement 5)

When:
- DSEE HX is turned OFF in the Music Center settings (`config.mode == 0`), OR
- The input stream is native Hi-Res (>= 88.2kHz, 96kHz, 192kHz, DSD), OR
- The input is non-stereo (`channels != 2`):

The hook immediately passes the configuration directly to `originalDsee` untouched. This preserves bit-perfect delivery for existing Hi-Res FLAC/WAV/DSD playback.

---

## 6. Graph Lifecycle Tracking (Requirement 8)

`dseeUnified::GraphTracker` observes `IGraphBuilder` instances across track transitions:
- `GRAPH_RECREATED`: Logged when a new `IGraphBuilder` COM instance is instantiated (e.g. manual track change, stopping and starting playback, or codec teardown).
- `GRAPH_REUSED`: Logged when the existing `IGraphBuilder` instance is reused. Distinguishes between:
  - `hr = S_OK`: Reused with same output format.
  - `hr = S_FALSE`: Reused across format switch (e.g. 44.1kHz ↔ 48kHz or 16-bit ↔ 24-bit).

---

## 7. Diagnostic and Build Integration

- **Target `flac24_dsee_experiment`**:
  Experimental DLL implementing DSEE HX unified 24-bit hooks.
- **Target `dsee_unified_tests`**:
  Headless automated test executable covering configuration logic, codec identification, Hi-Res bypass, constraint verification, mock DirectShow graph routing & rollback, and live Sony filter negotiation.
