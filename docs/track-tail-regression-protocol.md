# Real-Machine Regression Protocol for Track-Tail Preservation & Manual Operations

## 1. Overview & Objective

This document defines the standardized measurement procedure, recording schema, and analysis tooling for verifying the EOS (End of Stream) deferral mechanism in Music Center for PC. It serves as the regression comparison protocol for Issue #21 Phase 3.

The goal is to evaluate real-machine playback behavior across three critical scenarios:
1. **Scenario 1: `24 -> 16 bit` Track Tail Cutoff & Next Track Start (末尾欠落と次曲開始)**
   Verifies whether the final ~300 ms of 24-bit audio is fully played before transitioning to a 16-bit track, compared to legacy behavior where the tail was flushed and truncated.
2. **Scenario 2: `96 kHz/24 bit` Seamless Pre-Reading (別曲同士の先読み)**
   Verifies that sequential tracks of identical format continue contiguously across pre-read boundaries without artificial silence gaps or pre-read stalls.
3. **Scenario 3: Manual Operations During Deferral Window (保留中の一時停止・シーク・曲送り・停止・終了)**
   Verifies that user interactions (Pause, Seek, TrackChange, Stop, Exit) occurring within the ~300 ms deferral window cancel the pending EOS immediately, take immediate effect with low latency ($\le 15\text{ ms}$), and cause zero stale EOS intrusions.

> [!NOTE]
> Per project assignments, actual GUI manipulation on Music Center and execution with copyrighted audio files are handled by Codex. Antigravity provides the test protocol, deterministic analysis tooling, recording format, and synthetic regression validation.

---

## 2. Verification Architecture & Tools

The regression test protocol utilizes the non-intrusive diagnostic probes and the automated analysis CLI built into the repository:

| Tool | Source | Purpose |
|:---|:---|:---|
| `wasapi_pcm_probe.dll` | `diagnostics/probe.cpp`, `diagnostics/wasapi_capture.h` | Taps WASAPI `IAudioRenderClient::ReleaseBuffer` to capture submitted PCM directly to `%TEMP%\WASAPITap-*.pcm`. |
| `dshow_probe.dll` / `end_probe.dll` | `diagnostics/dshow_probe.cpp`, `diagnostics/end_probe.cpp` | Hooks DirectShow filters (`OmgEqxlFilter.ax`, renderer pins, `IMediaControl`, `IMediaSeeking`) to record exact event timestamps. |
| `track_regression_tool.exe` | `diagnostics/track_regression_tool.cpp`, `diagnostics/track_regression_compare.h` | Analyzes captured PCM files or synthetic test suites, outputs Markdown/TSV/JSON records, and determines PASS/FAIL verdicts. |

---

## 3. Recording Schema & Data Formats

Each regression run must capture the following fields:

| Field Name | Type | Description | Target / Threshold |
|:---|:---:|:---|:---|
| `test_case_id` | String | Unique identifier (e.g., `TC-1.1`, `TC-2.1`, `TC-3.1`) | - |
| `scenario` | String | Scenario description | Scenario 1, 2, or 3 |
| `format_transition` | String | Format change description (e.g., `24b/48k -> 16b/48k`) | - |
| `sample_rate` | UInt32 | Sample rate in Hz (e.g., 48000, 96000) | - |
| `bit_depth` | UInt16 | Bit depth (16 or 24) | - |
| `expected_track1_frames` | UInt64 | Exact expected audio frames of Track 1 | Exact match with file length |
| `track1_effective_end_frame` | UInt64 | Frame index of last active non-zero sample in Track 1 | Equals `expected_track1_frames` |
| `track1_effective_end_ms` | Float | Timestamp of last active sample in Track 1 (ms) | Track 1 duration |
| `missing_tail_frames` | UInt64 | Frames missing from expected Track 1 tail | **0 frames** |
| `missing_tail_ms` | Float | Missing tail duration in milliseconds | **0.0 ms** |
| `tail_status` | String | `欠落なし` (Complete) vs `欠落あり` (Truncated) | **欠落なし** |
| `next_track_detected` | Bool | Whether Next Track audio was observed | Expected `true` when transition exists |
| `next_track_start_frame` | UInt64 | Frame index where Next Track audio begins | - |
| `next_track_start_ms` | Float | Timestamp where Next Track audio begins (ms) | - |
| `post_eos_silence_ms` | Float | Duration of silence padding between Track 1 end and Next Track | **0.0 ms** for seamless; $\le 100\text{ ms}$ for normal |
| `manual_op_type` | String | `None`, `Pause`, `Seek`, `TrackChange`, `Stop`, `Exit` | - |
| `manual_op_input_time_ms` | Float | Timestamp when user interaction was triggered | - |
| `intended_target` | String | Target of operation (e.g. Pause, Target position, Next track) | Matches user intent |
| `actual_reflection_time_ms` | Float | Timestamp when audio engine / renderer applied the operation | - |
| `reflection_latency_ms` | Float | Time difference (`actual_reflection_time_ms - manual_op_input_time_ms`) | $\le \mathbf{15.0\text{ ms}}$ |
| `stale_eos_intrusions` | UInt32 | Spurious EOS deliveries arriving during/after manual op | **0** |
| `verdict` | String | Evaluation verdict (`PASS` or `FAIL`) | **PASS** |
| `notes` | String | Supplementary diagnostic details or log references | - |

### 3.1 TSV Format Header

```tsv
test_case_id	scenario	format_transition	sample_rate	bit_depth	expected_track1_frames	track1_effective_end_frame	track1_effective_end_ms	missing_tail_frames	missing_tail_ms	tail_status	next_track_detected	next_track_start_frame	next_track_start_ms	post_eos_silence_ms	manual_op_type	manual_op_input_time_ms	intended_target	actual_reflection_time_ms	reflection_latency_ms	stale_eos_intrusions	verdict	notes
```

---

## 4. Real-Machine Execution Procedure

### 4.1 Preparation
1. Build the diagnostics tools using CMake (Win32 Release):
   ```powershell
   cmake -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_DIAGNOSTICS=ON -DMC_BRIDGE_BUILD_EXPERIMENTS=ON
   cmake --build build-Win32 --config Release
   ```
2. Prepare the target audio tracks:
   - **Track A (24-bit/48kHz or 96kHz)**: Known exact frame count, clean audio up to the final millisecond.
   - **Track B (16-bit/44.1kHz or 48kHz)**: Target format for Scenario 1.
   - **Track C (24-bit/96kHz)**: Same format target for Scenario 2.

### 4.2 Scenario 1: `24 -> 16 bit` Track Tail Verification
1. Queue Track A (24-bit) followed by Track B (16-bit) in Music Center.
2. Launch Music Center under `wasapi_pcm_probe.dll` (or inject via `diagnostics/inject.exe`).
3. Allow playback to proceed naturally across the track boundary into Track B.
4. Locate the recorded PCM file: `%TEMP%\WASAPITap-<PID>-<index>-<rate>-<bits>-<valid>-<channels>.pcm`.
5. Run analysis with `track_regression_tool.exe`:
   ```powershell
   .\build-Win32\Release\track_regression_tool.exe <pcm_file> 48000 24 <expected_track_a_frames> `
     --scenario "1. 24->16b Tail" `
     --transition "24b/48k -> 16b/48k" `
     --target "Track B start"
   ```
6. **Pass Criteria**: `missing_tail_ms == 0.0 ms`, `tail_status == 欠落なし`, `stale_eos_intrusions == 0`.

### 4.3 Scenario 2: `96 kHz/24 bit` Seamless Pre-Reading Verification
1. Queue two contiguous 24-bit/96kHz tracks (Track A and Track C) in Music Center.
2. Capture the output stream via `wasapi_pcm_probe.dll`.
3. Allow playback across the track transition.
4. Run analysis with `track_regression_tool.exe`:
   ```powershell
   .\build-Win32\Release\track_regression_tool.exe <pcm_file> 96000 24 <expected_track_a_frames> `
     --scenario "2. 96k Seamless" `
     --transition "24b/96k -> 24b/96k" `
     --target "Track C start"
   ```
5. **Pass Criteria**: `missing_tail_ms == 0.0 ms`, `post_eos_silence_ms == 0.0 ms`, continuous waveform without pre-read drop.

### 4.4 Scenario 3: Manual Operations During Deferral Window
1. Start playback of Track A (24-bit).
2. Note the deferral window: the final ~300 ms before track completion (e.g. at 04:59.700 of a 05:00.000 track).
3. In the final 200 ms, trigger one of the following manual operations:
   - **Case 3.1 Pause**: Click Pause (or Spacebar).
   - **Case 3.2 Seek**: Click track progress bar to seek backwards/forwards.
   - **Case 3.3 TrackChange**: Click Next Track button.
   - **Case 3.4 Stop**: Click Stop button.
   - **Case 3.5 Exit**: Close Music Center window (`WM_CLOSE`).
4. Read input dispatch timestamp ($T_{\text{input}}$) from `dshow_probe.log` or input tap, and reflection timestamp ($T_{\text{reflect}}$) from renderer event log.
5. Run analysis:
   ```powershell
   .\build-Win32\Release\track_regression_tool.exe <pcm_file> 48000 24 <expected_track_a_frames> `
     --scenario "3. Manual Op" `
     --transition "24b/48k Deferral" `
     --op "Pause" `
     --input-ms <input_ms> `
     --reflect-ms <reflect_ms> `
     --target "Renderer Pause"
   ```
6. **Pass Criteria**:
   - $\text{Latency} = T_{\text{reflect}} - T_{\text{input}} \le 15.0\text{ ms}$.
   - Zero stale EOS intrusions (`stale_eos_intrusions == 0`).
   - No audio buzz, double trigger, or application freeze.

---

## 5. Synthetic Regression Verification Results

The automated synthetic suite verifies the deterministic discrimination between complete playback, truncated tail, and manual operation latencies:

```
# Real-Machine Regression Synthetic Verification Suite

| Case ID | Scenario | Transition | Rate/Bit | Track1 End (ms) | Missing (ms) | Tail Status | Next Start (ms) | Gap (ms) | Manual Op | Input (ms) | Intended | Reflect (ms) | Latency (ms) | Stale EOS | Verdict |
|:---|:---|:---|:---:|---:|---:|:---:|---:|---:|:---:|---:|:---|---:|---:|:---:|:---:|
| TC-1.1 | 1. 24->16b Tail | 24b/48k -> 16b/48k | 48k/24b | 5000.0 | 0.0 | 欠落なし | 5100 | 100.0 | None | - | Track 2 start | - | - | 0 | **PASS** |
| TC-1.2 | 1. 24->16b Tail | 24b/48k -> 16b/48k | 48k/24b | 4700.0 | 300.0 | 欠落あり | 5000 | 300.0 | None | - | Track 2 start | - | - | 0 | **FAIL** |
| TC-1.3 | 1. 24->16b Tail | 24b/48k -> 16b/48k | 48k/24b | 2100.0 | 2900.0 | 欠落あり | - | 0.0 | None | - | Track 2 start | - | - | 0 | **FAIL** |
| TC-1.4 | 1. 24->16b Tail | 24b/48k -> 16b/48k | 48k/24b | 5000.0 | 0.0 | 欠落あり | 5000 | 0.0 | None | - | Track 2 start | - | - | 0 | **PENDING** (判定保留) |
| TC-2.1 | 2. 96k Seamless | 24b/96k -> 24b/96k | 96k/24b | 5000.0 | 0.0 | 欠落なし | 5000 | 0.0 | None | - | Track 2 (anti-phase) | - | - | 0 | **PASS** |
| TC-2.2 | 2. 96k Seamless | 24b/96k -> 24b/96k | 96k/24b | 5000.0 | 0.0 | 欠落なし | 5250 | 250.0 | None | - | Track 2 (anti-phase) | - | - | 0 | **FAIL** |
| TC-3.1 | 3. Manual Op | 24b/48k Deferral | 48k/24b | 5000.0 | 0.0 | 欠落なし | - | 0.0 | Pause | 4950 | Renderer Pause | 4952 | 2.100 | 0 | **PASS** |
| TC-3.2 | 3. Manual Op | 24b/48k Deferral | 48k/24b | 5000.0 | 0.0 | 欠落なし | - | 0.0 | Seek | 4970 | Position 00:30.000 | 4974 | 4.300 | 0 | **PASS** |
| TC-3.3 | 3. Manual Op | 24b/48k Deferral | 48k/24b | 5000.0 | 0.0 | 欠落なし | - | 0.0 | TrackChange | 4980 | Next Track | 4983 | 3.500 | 0 | **PASS** |
| TC-3.4 | 3. Manual Op | 24b/48k Deferral | 48k/24b | 5000.0 | 0.0 | 欠落なし | - | 0.0 | Stop | 4960 | Playback Stopped | 4961 | 1.800 | 0 | **PASS** |
| TC-3.5 | 3. Manual Op | 24b/48k Deferral | 48k/24b | 5000.0 | 0.0 | 欠落なし | - | 0.0 | Exit | 4990 | Process/Graph Terminate | 4991 | 1.200 | 0 | **PASS** |

Summary:
Total Cases: 11 | Passed: 7 | Pending (判定保留): 1 | Expected Failures/Truncations Detected: 3
```

- **TC-1.1**: Verifies 100% complete track tail when deferral is active (`missing_tail_ms == 0.0`).
- **TC-1.2 & TC-1.3**: Accurately detect simulated EQ cutoff (~300 ms lost) and premature stop (~2.9 s lost) as `欠落あり` (**FAIL**).
- **TC-1.4**: Accurately flags loopback capture discontinuity occurring across the track transition boundary as `判定保留` (**PENDING**), preventing false failures due to recording tool glitches.
- **TC-2.1 & TC-2.2**: Accurately discriminate between continuous 0 ms gap and a 250 ms pre-read deadline miss.
- **TC-3.1 ~ TC-3.5**: Confirm manual operations during deferral achieve $\le 4.3\text{ ms}$ latency with 0 stale EOS events.

---

## 6. Compliance & Safety Constraints

1. **Rule #26 Strict Compliance**:
   No disassembled bytecode sequences, instruction mnemonics, or decompiled functions from third-party binaries are published or checked into this repository.
2. **Privacy Protection**:
   Do not commit raw personal audio files, measurement logs with private directory paths, or licensed music metadata. Use deterministic synthetic streams or anonymized benchmark files.
3. **Packaging Isolation**:
   Diagnostic tools (`wasapi_pcm_probe`, `end_probe`, `track_regression_tool`) remain opt-in and are strictly excluded from default installation archives.
