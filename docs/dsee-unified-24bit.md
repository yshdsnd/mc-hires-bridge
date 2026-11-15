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
  Experimental DLL implementing DSEE HX unified 24-bit hooks combined with synchronous track-tail preservation (`MC_BRIDGE_PAUSE_MODE=defer`).
- **Target `dsee_unified_tests`**:
  Headless automated test executable covering configuration logic, codec identification, Hi-Res bypass, constraint verification, mock DirectShow graph routing & rollback, live Sony filter negotiation, decoder bit depth synchronization, and 5-stage graph format inspection.

---

## 8. Issue #43: Pipeline Format Correlation, 24-bit Input Synchronization, and Synchronous Track-Tail Integration

### 8.1 Format Inspection Across All Five Pipeline Stages

To eliminate ambiguity between setter return values (`S_OK`), status badge display (`HR-BR ON`), and actual audio hardware formats, `inspectGraphFormats` and `recordGraphInspection` correlate negotiated `WAVEFORMATEX` parameters across every active stage in the same graph generation:

```
[Decoded PCM Source] -> [DSEE HX In / Out] -> [Seamless In / Out] -> [EQ In / Out] -> [DirectSound Render In]
```

Diagnostics emitted for each track transition generation (`gen`):
- `GRAPH_GEN`: Current graph transition generation and active graph pointer.
- `GRAPH_DECODE_PCM`: Decoded PCM output format from source filter (e.g. `OmgFlacSrc`).
- `GRAPH_DSEE_IN` / `GRAPH_DSEE_OUT`: DSEE HX filter input and output pin connection formats. If DSEE is bypassed/absent, `GRAPH_DSEE_ABSENT` is emitted.
- `GRAPH_SEAMLESS_IN` / `GRAPH_SEAMLESS_OUT`: Seamless buffer filter input and output formats.
- `GRAPH_EQ_IN` / `GRAPH_EQ_OUT`: Graphic Equalizer input and output formats. When bypassed, `GRAPH_EQ_BYPASSED` is emitted.
- `GRAPH_RENDER_IN`: Actual connection media type on `CLSID_DSoundRender` input pin.
- `GRAPH_RENDER_PEER`: Indicates peer connection: `1` for direct connection from SeamlessFilter, `0` via EQ.

### 8.2 Root Cause and Resolution of 24-bit Native FLAC DSEE Mismatch

#### Root Cause
1. During graph construction, `OpcFlac.dll` initialized `OmgDseeHxFilter` via slot 3 with `config.inputBits = 16` based on the default DirectSound profile before `setterHook` intercepted `OmgFlacSrc` to override decoding to 24-bit.
2. Because DSEE HX received `inputBits = 16`, `OmgDseeHxFilter` configured its internal DSP and input pin for 16-bit PCM.
3. When `OmgFlacSrc` delivered 24-bit PCM, `OmgDseeHxFilter::ReceiveConnection` rejected the media type (`0x80040207`).
4. `OpcFlac.dll` fell back to bypassing DSEE entirely, wiring `OmgFlacSrc -> SeamlessFilter` directly (outputting 48kHz/24bit native without DSEE HX).

#### Resolution
- `setActiveDecoderBits(24)` is recorded in `setterHook` as soon as native 24-bit capability is detected.
- `computeUnifiedDseeConfig` checks `getActiveDecoderBits()` and synchronizes `adjusted.inputBits = 24`.
- `dseeUnifiedSetterHook` logs `DSEE_IN_BITS_SYNC 16 24` and configures `OmgDseeHxFilter` with `inputBits = 24`.
- Both 48kHz/16-bit and 48kHz/24-bit FLAC tracks now negotiate through `OmgDseeHxFilter` and arrive at the exact same renderer format:
  - Default / `MC_BRIDGE_DSEE_MODE=96k`: **`96000 Hz, 24-bit, 2ch`**
  - `MC_BRIDGE_DSEE_MODE=native24`: **`48000 Hz, 24-bit, 2ch`**

### 8.3 Single Experimental DLL with Synchronous Track-Tail Preservation (`MC_BRIDGE_PAUSE_MODE=defer`)

In `flac24_dsee_experiment`:
1. Both `EXPERIMENT_DSEE_UNIFIED` and `EXPERIMENT_EOS_TRANSITION` are compiled together into a single DLL.
2. When EQ is bypassed, `OmgEqxlFilter.ax` receives 0 samples. To keep downstream sample accounting functional:
   - DirectSoundRender input pin (`CLSID_DSoundRender`) is hooked with `dsRenderReceiveHook`, `dsRenderEosHook`, `dsRenderBeginFlushHook`, and `dsRenderEndFlushHook`.
   - When `isEqBypassed` is true, downstream sample counts (`downstreamBytes`) are incremented on `DirectSoundRender::Receive`.
   - Synchronous track-tail deferral (`deferFormatChangePause`) tracks downstream PCM consumption directly at the renderer input and unblocks naturally at the track tail.
3. When DSEE is OFF or for native Hi-Res audio, existing routing is untouched (`applyEqBypass` is skipped and standard downstream paths operate normally).

### 8.4 Cross-Family (44.1kHz vs 48kHz) Common Sample Rate Analysis

- The user clarified that `Family2x` (88.2kHz vs 96kHz) is within-family integer upsampling, not cross-family sample rate unification.
- Investigation confirmed that Sony's `OmgDseeHxFilter.ax` internal DSP algorithm is an integer-ratio polyphase upsampler:
  - 44.1kHz can only upsample to 44.1k, 88.2k, or 176.4k.
  - 48.0kHz can only upsample to 48.0k, 96.0k, or 192.0k.
- Setting `outputRate = 96000` for 44.1kHz causes DSEE HX to reject fractional resampling and clamp to 176.4kHz/24-bit.

---

## 9. Issue #44: 44.1kHz Family Output Policy, Windows Shared Mode Integration, and Dual Graph Topology Bypass

### 9.1 Output Policy and Windows Shared Engine Responsibility

Under Music Center 2.7.4 with DSEE HX enabled and Windows DirectSound shared output:
1. **Device Shared Format Conversion**:
   - DirectSound / WASAPI shared mixer in Windows automatically converts any stream format accepted by the DirectSound renderer to the Windows default shared format (e.g., 24-bit 96kHz or 24-bit 192kHz configured in Windows Sound Control Panel).
   - Therefore, **an in-graph DirectShow resampler filter is unnecessary and undesirable**.
   - DSEE HX performs clean integer-ratio upsampling within each family without introducing DSP interpolation artifacts, passing pristine 24-bit PCM to DirectSound.
2. **Harmonization Modes**:
   - **`Family2x` (Default / Standard baseline)**:
     - 44.1kHz (16-bit & 24-bit) → **88.2kHz / 24-bit**
     - 48.0kHz (16-bit & 24-bit) → **96.0kHz / 24-bit**
   - **`Family4x` (Candidate mode)**:
     - 44.1kHz (16-bit & 24-bit) → **176.4kHz / 24-bit**
     - 48.0kHz (16-bit & 24-bit) → **192.0kHz / 24-bit**
   - **Native Hi-Res Pass-Through**:
     - 96kHz/24-bit and 192kHz/24-bit native sources pass through DSEE HX untouched (bit-perfect preservation).
   - **Equalizer**:
     - Disabled / bypassed (`applyEqBypass`), ensuring 24-bit output reaches DirectSound.

### 9.2 Dual Graph Topologies during Format-Changing Track Transitions

Live graph inspection during continuous playback across tracks of differing formats revealed two distinct DirectShow graph topologies constructed by `OpcDSPlay.dll` / `SeamlessManager.dll`:

- **Topology A (Same-Format / Seamless)**:
  `[Decoded Source] -> [OmgDseeHxFilter] -> [SeamlessFilter] -> [OmgEqxlFilter (EQ)] -> [DirectSoundRender]`
  - Used when consecutive tracks have identical sampling rates and bit depths.
  - `applyEqBypass` disconnects `SeamlessFilter -> EQ` and connects `SeamlessFilter -> DirectSoundRender`.
- **Topology B (Format-Changing Transition)**:
  `[Decoded Source] -> [OmgDseeHxFilter] -> [OmgEqxlFilter (EQ)] -> [DirectSoundRender]`
  - `SeamlessManager.dll` omits `SeamlessFilter` entirely because seamless buffer splicing cannot bridge differing sampling rates or bit depths.
  - The previous implementation expected `SeamlessFilter` to precede EQ and skipped bypass (`DSEE_EQ_SKIPPED`).
  - **Generalized `applyEqBypass`**:
    - Queries the upstream pin connected to `eqIn` via `eqIn->ConnectedTo(&upstreamOut)`.
    - Whether `upstreamOut` belongs to `SeamlessFilter` or `OmgDseeHxFilter`, it disconnects `upstreamOut -> eqIn`, disconnects `eqOut -> dsIn`, and connects `upstreamOut -> dsIn`.
    - Telemetry logs `GRAPH_RENDER_PEER: 2` for direct DSEE connection, `1` for direct Seamless connection, or `0` for EQ.

### 9.3 End-of-Stream (EOS) and Sample Accounting in Topology B

When `SeamlessFilter` is absent in Topology B:
1. Upstream hook `seamlessReceiveHook` is never invoked, leaving `upstreamSamples` at 0.
2. In `dsRenderReceiveHook`: when `upstreamSamples == 0` and EQ is bypassed, `onUpstreamSample` is recorded to ensure `upstreamBytes == downstreamBytes`.
3. In `dsRenderEosHook`: when `upstreamEosCount == 0`, `onUpstreamEndOfStream` is synthesized before `onDownstreamEndOfStream`.
4. This ensures `isFormatChangePending` is properly set, enabling synchronous track-tail pause deferral (`deferFormatChangePause`) to protect track tails across format transitions (e.g. 44.1kHz ↔ 48kHz or 24-bit ↔ 16-bit).

---

## 10. Issue #45: Downstream PCM Measurement Across EQ Topologies and Diagnostic Distinction

### 10.1 Root Cause of 0 / Target Bytes PCM Counter in Prior Measurements

In Issue #43 measurements, on the preceding track's EQ bypass graph, `DRAIN_HEARTBEAT_PCM` reported `0 / 13,248,000 B` continuously during the deferral window until timing out after ~2.92 seconds.

Investigation identified the root cause:
1. In COM, DirectShow's `CFilterGraph` implements multiple interfaces (`IGraphBuilder`, `IMediaControl`, `IFilterGraph`, etc.) via multiple inheritance.
2. `QueryInterface(IID_IUnknown)` returns the canonical identity pointer of the COM object (`unk`), which on MSVC is offset from `IGraphBuilder*` (`0x13C1B88` vs `0x13C1B40`, offset +0x48).
3. `prepareDseeGraph` invoked `onEosEqBypassed(graph.Get(), bypassed)`, recording the bypass state on `graph.Get()` (`IGraphBuilder*`).
4. Meanwhile, `dsRenderReceiveHook` determined the graph key via `graphKeyFromMemPin(self) -> graphKeyFromGraph(fi.pGraph)`, which queries `IUnknown` and obtained `unk` (`0x13C1B88`).
5. As a result, `eosTracker.isEqBypassed(key)` evaluated to `false` on the canonical key. `dsRenderReceiveHook` skipped recording downstream samples, and because EQ was bypassed, `eqReceiveHook` was never called either. `downstreamBytes` remained at 0 while `targetBytes` was 13,248,000.

### 10.2 Dual Canonical Resolution and Fallback

1. **`onEosEqBypassed`**:
   Accepts `IUnknown* graphObj` and queries `graphKeyFromObject(graphObj)` to resolve the canonical `IUnknown*` key. It registers `setEqBypassed(canonicalKey, bypassed)` and redundantly registers the raw interface pointer `setEqBypassed(rawKey, bypassed)`.
2. **`isEqBypassed` Fallback**:
   If exact key matching misses, `isEqBypassed` inspects any active 24-bit graph with `eqBypassed == true`, ensuring `dsRenderReceiveHook` never misses the bypass state.
3. **Pin Hook Telemetry**:
   `setupEosTransitionHooks` now explicitly validates and reports `DirectSoundRender.Pin` hook status (`OK`/`FAILED`) through `eosDiag::getHookStatusReporter()`.
4. **Progress Telemetry**:
   `dsRenderReceiveHook` records `DSRENDER_SAMPLE_PROGRESS` correlating track transition generation (`gen`), cumulative downstream bytes, target bytes, and EOS state.

### 10.3 Distinction between Diagnostic Deficit ("診断未達") and Track Tail Truncation ("曲末欠落")

In synchronous mode (`deferFormatChangePause`), completion is evaluated using two-stage drain verification:
1. **Renderer Output Evaluation**:
   - `c >= s && s > 0` (DirectShow reference clock reached or passed stop position).
   - DirectSound buffer status indicates buffer playback has stopped (`!(dsStatus & DSBSTATUS_PLAYING)`).
   - DirectSound buffer cursor reached end of audio data.
2. **Classification on Deferral Timeout**:
   - If renderer finished playing (or was within 50ms tolerance with EOS delivered):
     - Classified as **`DIAGNOSTIC_DEFICIT` (診断未達)**: `recordDiag("DRAIN_VERIFY_DIAGNOSTIC_DEFICIT", ...)`.
     - Audio hardware successfully output all audio samples to completion (missing tail: `0 ms`).
     - `exitStatus` is marked `CompletedDrain`.
   - If renderer was stopped while still actively playing prior to stop position / EOS:
     - Classified as **`TRACK_TAIL_TRUNCATED` (曲末欠落)**: `recordDiag("DRAIN_VERIFY_TAIL_TRUNCATED", missingTailMs, ...)`.
     - Missing tail duration is precisely calculated and reported.

### 10.4 Bi-Directional Transitions and Track 2 Tail Verification

`flac24_dsee_experiment` supports:
- Track 1 -> Track 2 (e.g. 48kHz/24bit -> 48kHz/16bit): Topology A EQ bypass, downstream PCM tracking, full drain.
- Track 2 -> Track 3 (Track 2's own tail): Topology B EQ bypass, downstream PCM tracking, full drain.
- Reverse order (e.g. 48kHz/16bit -> 48kHz/24bit): Topology B -> Topology A/B transitions with symmetrical tail protection and single-start playback.

---

## 11. Issue #46: Maintaining 44.1kHz DSEE Output (88.2kHz) up to DirectSound Renderer and Peer Code 3 Resolution

### 11.1 Problem Analysis: Origin of Peer Code 3 and Downsampler Insertion

In actual playback on Music Center 2.7.4 with DSEE HX ON (Family2x mode) using commit `5b35497`:
- **48kHz FLAC**: DSEE input was 48kHz/16bit (or 24bit), DSEE output was 96kHz/24bit. After EQ bypass, renderer received 96kHz/24bit with Peer Code 1 (`isDirectSeamless = true`).
- **44.1kHz FLAC**: DSEE input was 44.1kHz/16bit, DSEE output was 88.2kHz/24bit. However, after EQ bypass, the actual renderer input received **44.1kHz/24bit**, and the direct peer was classified as **Peer Code 3 ("その他")**.

Investigation of filter format acceptance via live `QueryAccept` revealed the root cause:
1. `OmgEqxlFilter.ax` (Sony Equalizer) accepts:
   - 44.1kHz / 16-bit: ACCEPT
   - 44.1kHz / 24-bit: ACCEPT
   - 48kHz / 16-bit: ACCEPT
   - 48kHz / 24-bit: ACCEPT
   - 96kHz / 24-bit: ACCEPT
   - **88.2kHz / 24-bit: REJECT**
2. DirectSound Audio Renderer (`CLSID_DSoundRender`) accepts:
   - 44.1kHz, 48kHz, **88.2kHz**, 96kHz, 176.4kHz, 192kHz (all 16-bit & 24-bit PCM): ACCEPT.
3. Because `OmgEqxlFilter` strictly rejects 88.2kHz, Music Center's graph construction automatically inserts a frequency sampling converter / downsampler filter in front of `OmgEqxlFilter` to resample the 88.2kHz stream down to 44.1kHz.
4. In prior implementation, `applyEqBypass` only disconnected `OmgEqxlFilter` and connected whatever pin was feeding `eqIn` directly to `dsIn`. Because the downsampler sat in front of `eqIn`, `applyEqBypass` connected the downsampler's output to DirectSound! This left the downsampler in place, feeding 44.1kHz/24bit to DirectSound, and because that filter was neither `SeamlessFilter` nor `OmgDseeHxFilter`, it was recorded as Peer Code 3 ("その他").

### 11.2 Direct Native DSEE Routing without Extra Resamplers

To maintain DSEE HX's native integer upsampling output (88.2kHz/24bit) all the way to DirectSound renderer without adding any external resampler:
1. **Target Upstream Pin Selection**:
   - When DSEE HX is active and generating Hi-Res audio (`dseeOut` rate >= 88.2kHz):
     - If `SeamlessFilter` is present and its output format matches `dseeOut` rate (>= 88.2kHz, e.g. 96kHz in 48kHz FLAC), `seamOut -> dsIn` is selected (Peer Code 1), preserving seamless track transitions.
     - If `SeamlessFilter` is downsampling (outputting 44.1kHz) or absent/rejecting 88.2kHz, `dseeOut -> dsIn` is selected directly (Peer Code 2).
   - When DSEE is OFF or for native Hi-Res audio:
     - If `SeamlessFilter` is present, `seamOut -> dsIn` is selected.
     - Otherwise, standard upstream pin (`upstreamEqOut -> dsIn`) is selected.
2. **Direct Connection to DirectSound**:
   - `targetOut` (`dseeOut`) is connected directly to `dsIn` (`DirectSoundRender`).
   - The intermediate downsampler and `OmgEqxlFilter` pins are disconnected and remain idle in the graph.
   - DirectSound natively receives **88.2kHz / 24-bit PCM**.
   - If connection fails, full rollback to original pin connections is performed.

### 11.3 Peer Code 3 Identification and Full Linear Chain Traversal

`GraphInspection` and diagnostic logging have been expanded:
1. **Linear Chain Traversal**:
   - Starting from `decoderFilter` output pin, follows `ConnectedTo` across all intermediate transform filters until reaching `DirectSoundRender`.
   - Records CLSID, filter name (`achName`), pin names, input format, and output format for up to 8 nodes in `out.chain[0..N]`.
   - Pinpoints the exact filter node where sample rate reduction occurs.
2. **Peer Diagnostics**:
   - For any DirectSound peer, queries `FILTER_INFO` and `GetClassID`.
   - Records `GRAPH_PEER_CLSID_1` / `GRAPH_PEER_CLSID_2` (full 128-bit GUID) and `GRAPH_RENDER_PEER` (0 = EQ, 1 = Seamless, 2 = DSEE, 3 = Other).

### 11.4 Test Verification

- **Unit Test 13 in `dsee_unified_tests.cpp`**:
  - `13A`: 44.1kHz FLAC with DSEE HX ON and intermediate downsampler -> `applyEqBypass` successfully connects `dseeOut -> dsIn` directly at 88.2kHz/24bit; reports Peer Code 2 (`isDirectDsee = true`); second invocation reports `AlreadyBypassed`.
  - `13B`: 48kHz FLAC with DSEE HX ON -> `applyEqBypass` successfully connects `seamOut -> dsIn` at 96kHz/24bit; reports Peer Code 1 (`isDirectSeamless = true`).
  - `13C`: Native 192kHz FLAC with DSEE OFF -> `applyEqBypass` preserves bit-perfect 192kHz/24bit output via `seamOut -> dsIn`.
- **Regression**: All 13 DSEE unified test suites and all 77 EOS transition test cases pass with 0 failures.

---

## 12. Issue #47: Retaining Synchronous Pause Deferral until Audio Renderer Drain Completion

### 12.1 Problem Analysis: Premature Pause Deferral Bypass after Downstream PCM Tracking

In commit `5b35497` (#45), live playback of `48kHz/24bit FLAC -> 48kHz/16bit FLAC` in synchronous mode (both DSEE 96kHz/24bit, EQ bypassed) successfully recorded downstream PCM progress to DirectSound audio renderer.
However:
1. `SeamlessFilter` delivered EOS to the renderer input pin.
2. Approximately 1.55 seconds later, Music Center's playback engine issued an automatic `Pause()` (`IMediaControl::Pause`).
3. At this moment, synchronous pause deferral (`deferFormatChangePause`) **did not start** (`pendingFormatChange` evaluated to `0`), and the next track started ~20.23 seconds after track 1 start.
4. Compared against reference waveform, ~2.9 seconds of the 23-second preceding track tail was truncated.
5. In #43, the same track sequence preserved the full track tail because `downstreamBytes` was stuck at 0, which inadvertently kept the deferral condition active.

Investigation of `isFormatChangePending(graphKey)` revealed the root cause:
- Prior logic checked:
  ```cpp
  if (!s->eosDelivered) return true;
  if (s->hasStopPosition && s->hasCurrentPosition && s->currentPosition < s->stopPosition) return true;
  if (s->targetBytes > 0 && s->downstreamBytes < s->targetBytes) return true;
  return false;
  ```
- Once #45 fixed downstream tracking, `downstreamBytes >= targetBytes` became true.
- Because `eosDelivered` was already true (sent 1.55s earlier) and live `currentPosition` was not queried immediately before Pause, `isFormatChangePending` fell through to `return false;`.
- Consequently, `controlPauseHook` treated format change as already done and executed `originalControlPause`, abruptly pausing the DirectSound renderer while ~2.9 seconds of audio was still actively playing out of the buffer.

### 12.2 Three-Stage Drain State Separation

To prevent premature graph pause and ensure complete acoustic output, three distinct drain states are strictly separated in `eosTransition::Tracker`:

1. **Downstream PCM Delivery Complete (`pcmDeliveryComplete` / `downstreamReachedTarget`)**:
   - All PCM bytes of the segment have been pushed to the downstream audio renderer's input pin (`downstreamBytes >= targetBytes`).
2. **EOS Delivery Complete (`downstreamEosDelivered` / `eosDelivered`)**:
   - `IPin::EndOfStream()` notification has been delivered to downstream renderer input pin.
   - Guarded by single-delivery logic (`downstreamEosDeliveredCount == 1`) to prevent duplicate EOS calls.
3. **Renderer Audio Output Drain Complete (`rendererOutputComplete`)**:
   - The DirectSound audio renderer circular buffer has finished playing out all buffered samples to the hardware speakers (`!(dsStatus & DSBSTATUS_PLAYING)` or reference clock `c >= s && s > 0`).

Full drain completion (`isDrainFullyComplete`) requires all three stages to be satisfied:
```cpp
bool isDrainFullyComplete = pcmDeliveryComplete && downstreamEosDelivered && rendererOutputComplete;
```

### 12.3 Synchronous Pause Deferral Logic Update

1. **`isFormatChangePending(graphKey)` Branching**:
   - `isFormatChangePending` continues returning `true` even after `downstreamBytes >= targetBytes` and `eosDelivered == true`, as long as `rendererOutputComplete` is false.
   - It only returns `false` when:
     - `rendererOutputComplete` is true, or
     - Live playback position has reached/passed stop position (`currentPosition >= stopPosition && eosDelivered && downstreamBytes >= targetBytes`), or
     - Format change deferral has already completed (`pauseDeferralStatus == CompletedDrain`).
2. **Live Seeking Position Refresh**:
   - In `controlPauseHook` and `filterPauseHook`, before evaluating `isFormatChangePending`, `self->QueryInterface(IID_PPV_ARGS(&seeking))` is invoked to refresh `currentPosition` and `stopPosition` with live DirectShow stream time.
3. **`deferFormatChangePause` Completion Marking**:
   - When DirectSound buffer finishes playing (`dsStopped`) or stream position reaches stop position (`posReached`), `eosTracker.onRendererOutputComplete(key)` is recorded.
   - Subsequent `filterPause` or `controlPause` checks see `rendererOutputComplete == true` and `pauseDeferralStatus == CompletedDrain`, avoiding redundant secondary deferrals.
4. **Duplicate EOS Prevention**:
   - `deferFormatChangePause` checks `!g->eosDelivered` before calling `downPin->EndOfStream()`. Since EOS was already delivered when final PCM arrived, duplicate delivery is suppressed.

### 12.4 Test Verification

- **Regression Test 77 in `async_transition_tests.h`**:
  - Recreates the exact measured sequence:
    1. 48kHz/24bit FLAC -> DSEE 96kHz/24bit, EQ bypassed, format change pre-read prepared.
    2. Upstream finishes, downstream receives all 13,248,000 bytes, EOS delivered to renderer (`isDownstreamEosDelivered == true`, count = 1).
    3. Live playback position at 20.23 seconds (< 23.00 seconds stop position), audio remaining in DirectSound buffer (`rendererOutputComplete == false`).
    4. Verified `isFormatChangePending == true` at 20.23 seconds.
    5. Automatic `Pause` arrives: synchronous pause deferral begins (`isPauseDeferralActive == true`).
    6. Verified `onDownstreamEndOfStream` rejects duplicate delivery (count remains 1).
    7. DirectSound buffer plays out remaining ~2.77 seconds to completion (`c >= s`).
    8. Deferral completes with `CompletedDrain`.
    9. Verified `rendererOutputComplete == true`, `isDrainFullyComplete == true`, `isFormatChangePending == false`.
- **Suite Results**:
  - `eos_transition_tests.exe`: 77/77 tests passed (0 failures).
  - `dsee_unified_tests.exe`: 13/13 test suites passed (0 failures).
  - `flac24_dsee_experiment.dll`: Built cleanly.




