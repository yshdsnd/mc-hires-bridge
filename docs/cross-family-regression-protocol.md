# 44.1kHz ↔ 48kHz Cross-Family Transition Test Protocol & Verification Procedure

## 1. Overview & Objectives (概要と検証目的)

This document establishes the test material preparation procedure, playlist naming/ordering conventions, and deterministic evaluation methodology for verifying cross-family track transitions in Sony Music Center for PC 2.7.4 (x86).

### 1.1 Target Transitions (検証対象の曲送り)
Based on commit `60c5bba` (#47), the following three transitions must be verified:
1. **Transition 1: `44.1kHz/16-bit FLAC -> 48kHz/24-bit FLAC`** (Bidirectional Part 1)
   - Track 1 DSEE output: 88.2kHz / 24-bit (Family2x)
   - Track 2 DSEE output: 96.0kHz / 24-bit (Family2x)
2. **Transition 2: `48kHz/24-bit FLAC -> 44.1kHz/16-bit FLAC`** (Bidirectional Part 2)
   - Track 1 DSEE output: 96.0kHz / 24-bit (Family2x)
   - Track 2 DSEE output: 88.2kHz / 24-bit (Family2x)
3. **Transition 3: `44.1kHz/24-bit FLAC -> 48kHz/16-bit FLAC`** (Hi-Res input -> Standard input)
   - Track 1 DSEE output: 88.2kHz / 24-bit (Family2x)
   - Track 2 DSEE output: 96.0kHz / 24-bit (Family2x)

### 1.2 Evaluation Principles (判定原則)
- **Non-Bit-Exact Principle with DSEE HX (DSEE HX下での非PCM完全一致原則)**:
  Because DSEE HX performs high-frequency harmonic synthesis and integer upsampling (88.2kHz or 96kHz 24-bit), the acoustic PCM waveform output to the DirectSound renderer or captured via WASAPI loopback differs from the original source file. **Bit-for-bit identical PCM match is strictly NOT a passing criterion.**
  Evaluation is performed through **energy envelope timing, effective playback duration, single-cue detection, and bridge diagnostic telemetry**.
- **Loopback Discontinuity Handling (ループバック不連続時の判定保留)**:
  When audio sample rates change across Windows shared mixer boundaries, audio capture tools (such as WASAPI loopback) may experience buffer re-negotiation glitches or `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY`. If a discontinuity occurs across the track transition boundary, the test verdict must be marked **`判定保留` (PENDING)** and re-run rather than flagged as a bridge failure.
- **Role Demarcation (担当分担)**:
  - **Antigravity**: Tooling, synthetic test suites, material generation procedure, and evaluation protocol.
  - **Codex**: GUI operations on Music Center for PC, local asset creation, and real-machine recording execution.

---

## 2. Test Material & Reference Preparation Procedure (試験素材・参照PCM作成手順)

To ensure fast iteration and reliable waveform analysis, short clips (e.g., 20.0 to 25.0 seconds) are prepared from any local lossless music tracks using standard `ffmpeg` commands.

### 2.1 Extraction and Transcoding Commands

Run the following commands in PowerShell. Replace `<source_input.flac>` with any local lossless audio file:

#### Asset A1: 44.1kHz / 16-bit / Stereo FLAC (20.0s)
```powershell
ffmpeg -y -ss 00:00:10 -i "<source_input.flac>" -t 20 `
  -ar 44100 -sample_fmt s16 `
  -metadata title="[CF-A1] 44.1k/16b" `
  -metadata artist="RegressionTest" `
  -metadata album="Test_CF_44k16_to_48k24" `
  -metadata track="1" `
  "01_CF_A1_44k16b.flac"

# Extract reference raw PCM and uncompressed WAV
ffmpeg -y -i "01_CF_A1_44k16b.flac" -f s16le -acodec pcm_s16le "01_CF_A1_44k16b_ref.pcm"
ffmpeg -y -i "01_CF_A1_44k16b.flac" -acodec pcm_s16le "01_CF_A1_44k16b_ref.wav"
```

#### Asset A2: 48.0kHz / 24-bit / Stereo FLAC (20.0s)
```powershell
ffmpeg -y -ss 00:00:10 -i "<source_input.flac>" -t 20 `
  -ar 48000 -sample_fmt s32 -bits_per_raw_sample 24 `
  -metadata title="[CF-A2] 48k/24b" `
  -metadata artist="RegressionTest" `
  -metadata album="Test_CF_44k16_to_48k24" `
  -metadata track="2" `
  "02_CF_A2_48k24b.flac"

# Extract reference raw PCM and uncompressed WAV
ffmpeg -y -i "02_CF_A2_48k24b.flac" -f s24le -acodec pcm_s24le "02_CF_A2_48k24b_ref.pcm"
ffmpeg -y -i "02_CF_A2_48k24b.flac" -acodec pcm_s24le "02_CF_A2_48k24b_ref.wav"
```

#### Asset B1: 48.0kHz / 24-bit / Stereo FLAC (20.0s, Transition 2 Track 1)
```powershell
ffmpeg -y -ss 00:00:10 -i "<source_input.flac>" -t 20 `
  -ar 48000 -sample_fmt s32 -bits_per_raw_sample 24 `
  -metadata title="[CF-B1] 48k/24b" `
  -metadata artist="RegressionTest" `
  -metadata album="Test_CF_48k24_to_44k16" `
  -metadata track="1" `
  "01_CF_B1_48k24b.flac"

ffmpeg -y -i "01_CF_B1_48k24b.flac" -f s24le -acodec pcm_s24le "01_CF_B1_48k24b_ref.pcm"
```

#### Asset B2: 44.1kHz / 16-bit / Stereo FLAC (20.0s, Transition 2 Track 2)
```powershell
ffmpeg -y -ss 00:00:10 -i "<source_input.flac>" -t 20 `
  -ar 44100 -sample_fmt s16 `
  -metadata title="[CF-B2] 44.1k/16b" `
  -metadata artist="RegressionTest" `
  -metadata album="Test_CF_48k24_to_44k16" `
  -metadata track="2" `
  "02_CF_B2_44k16b.flac"

ffmpeg -y -i "02_CF_B2_44k16b.flac" -f s16le -acodec pcm_s16le "02_CF_B2_44k16b_ref.pcm"
```

#### Asset C1: 44.1kHz / 24-bit / Stereo FLAC (20.0s, Transition 3 Track 1)
```powershell
ffmpeg -y -ss 00:00:10 -i "<source_input.flac>" -t 20 `
  -ar 44100 -sample_fmt s32 -bits_per_raw_sample 24 `
  -metadata title="[CF-C1] 44.1k/24b" `
  -metadata artist="RegressionTest" `
  -metadata album="Test_CF_44k24_to_48k16" `
  -metadata track="1" `
  "01_CF_C1_44k24b.flac"

ffmpeg -y -i "01_CF_C1_44k24b.flac" -f s24le -acodec pcm_s24le "01_CF_C1_44k24b_ref.pcm"
```

#### Asset C2: 48.0kHz / 16-bit / Stereo FLAC (20.0s, Transition 3 Track 2)
```powershell
ffmpeg -y -ss 00:00:10 -i "<source_input.flac>" -t 20 `
  -ar 48000 -sample_fmt s16 `
  -metadata title="[CF-C2] 48k/16b" `
  -metadata artist="RegressionTest" `
  -metadata album="Test_CF_44k24_to_48k16" `
  -metadata track="2" `
  "02_CF_C2_48k16b.flac"

ffmpeg -y -i "02_CF_C2_48k16b.flac" -f s16le -acodec pcm_s16le "02_CF_C2_48k16b_ref.pcm"
```

### 2.2 Verifying Exact Frame Counts
Obtain the exact frame count of the generated files to supply to `track_regression_tool.exe`:
```powershell
ffprobe -v error -select_streams a:0 -show_entries stream=duration,nb_read_frames -count_frames -of default=noprint_wrappers=1 "01_CF_A1_44k16b.flac"
```

---

## 3. Music Center Naming & Ordering Rules (Music Centerでの曲名・順序管理)

Music Center for PC orders tracks in its internal database by **Album** and **Track Number** metadata tags. To guarantee that pairs are queued strictly consecutively without intermixing:

| Transition Pair | Filename | ID3 Title Tag | ID3 Track | ID3 Album |
|:---|:---|:---|:---:|:---|
| **Pair 1: 44.1k/16 -> 48k/24** | `01_CF_A1_44k16b.flac` | `[CF-A1] 44.1k/16b` | 1 | `Test_CF_44k16_to_48k24` |
| | `02_CF_A2_48k24b.flac` | `[CF-A2] 48k/24b` | 2 | `Test_CF_44k16_to_48k24` |
| **Pair 2: 48k/24 -> 44.1k/16** | `01_CF_B1_48k24b.flac` | `[CF-B1] 48k/24b` | 1 | `Test_CF_48k24_to_44k16` |
| | `02_CF_B2_44k16b.flac` | `[CF-B2] 44.1k/16b` | 2 | `Test_CF_48k24_to_44k16` |
| **Pair 3: 44.1k/24 -> 48k/16** | `01_CF_C1_44k24b.flac` | `[CF-C1] 44.1k/24b` | 1 | `Test_CF_44k24_to_48k16` |
| | `02_CF_C2_48k16b.flac` | `[CF-C2] 48k/16b` | 2 | `Test_CF_44k24_to_48k16` |

### In-App Queueing Instruction
1. Drag and drop the two files of a pair into a dedicated test playlist in Music Center.
2. Confirm the display order is `1 -> 2`.
3. Set playback mode to **Normal Sequential (曲順通り / リピートOFF)**.

---

## 4. Evaluation Methodology (判定手順と合否判定基準)

### 4.1 Evaluation Criteria Overview

| Item | Evaluation Method | Pass Criterion (合格条件) | Failure Sign (不合格兆候) |
|:---|:---|:---|:---|
| **1. 先行曲の開始 (Track 1 Start)** | Waveform audio energy onset & Bridge log | Audio onset begins within 100 ms of playback start; `GRAPH_RENDER_IN` recorded | Silent playback or crash |
| **2. 先行曲末尾の保持 (Track 1 Tail)** | Audio duration vs reference & Bridge log | `missing_tail_ms == 0.0 ms` ($\Delta t \le 50\text{ ms}$); `DRAIN_VERIFY_DIAGNOSTIC_DEFICIT` recorded | Audio cuts off ~300ms or ~2.9s early; `TAIL_TRUNCATED` |
| **3. 次曲の重複頭出し判定 (Track 2 Single Start)** | Audio onset envelope & Bridge log | **Exactly 1 onset pulse**; single `CONTROL_RUN` / `DEFERRED_CONTROL_RUN_EXECUTED` | Stutter (double start within 50-300ms); duplicate Run calls |
| **4. 次曲の開始と次曲自身の末尾 (Track 2 Tail)** | Full duration completion & Playlist stop | Track 2 plays to full expected duration; clean stop without truncation | Premature stop during Track 2 playback |

### 4.2 Detailed Verification Steps

#### Step 1: Pre-Execution Environment Setup
Ensure `MC_BRIDGE_PAUSE_MODE` and `MC_BRIDGE_DSEE_MODE` are configured:
```powershell
$env:MC_BRIDGE_PAUSE_MODE = "defer"
$env:MC_BRIDGE_DSEE_MODE = "family2x"
```

#### Step 2: Audio Capture & Bridge Logging
1. Launch Music Center under `flac24_dsee_experiment.dll`.
2. Start audio loopback capture (WASAPI Loopback capture or line recording tap).
3. Play Track 1 and allow Music Center to transition naturally into Track 2 until Track 2 completes.
4. Stop recording and save the captured PCM/WAV file.

#### Step 3: Loopback Discontinuity Inspection
Before evaluating track tails, inspect the capture recording for loopback buffer overflow/underflow or discontinuity:
- **Discontinuity Check**:
  - In capture tool log: look for `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` or dropped packets.
  - In audio waveform: look for sudden DC offsets, missing buffer chunks (>10ms unbuffered drop), or timecode jitter at the boundary.
- **Handling**:
  - If a discontinuity is found **across the track transition boundary**, mark the verdict as **`判定保留` (PENDING)**.
  - Run `track_regression_tool.exe` with `--discontinuity` flag to record `PENDING`.
  - Repeat the test run with a fresh capture.

#### Step 4: Deterministic Waveform & Log Analysis
Run `track_regression_tool.exe` against the captured stream:
```powershell
.\build-Win32\Release\track_regression_tool.exe <captured.pcm> <capture_rate> <capture_bits> <expected_track1_frames> `
  --scenario "Cross-Family Transition" `
  --transition "44.1k/16b -> 48k/24b" `
  --target "Track 2 Start"
```

Bridge diagnostic log verification in `dshow_probe.log`:
- Confirm `BOUNDARY_CONTROL_PAUSE_ENTER` has `isKnownCaller = 1` and `pendingFormatChange = 1`.
- Confirm `PAUSE_DEFERRED_PENDING` and `DRAIN_WAIT_ENTER` occur.
- Confirm DirectSound buffer drain completes: `PAUSE_DEFERRED_RELEASE` with `CompletedDrain` (status 3).
- Confirm next track execution: `DEFERRED_CONTROL_RUN_EXECUTED` (or direct single `CONTROL_RUN`) with `count == 1`.
- Confirm downstream EOS count remains exactly 1: `eosDeliveredCount == 1`.

---

## 5. Result Recording Schema (実機検証記録フォーマット)

Codex outputs the test result using the standardized table format:

| Case ID | Transition | Track 1 Expected (s) | Track 1 Actual (s) | Missing Tail (ms) | Track 2 Start Mode | Duplicate Cue? | Loopback Quality | Bridge Telemetry | Verdict |
|:---|:---|---:|---:|---:|:---|:---:|:---|:---|:---:|
| **CF-01** | 44.1k/16b -> 48k/24b | 20.00 | 20.00 | 0.0 | Single Start | なし (1回) | Clean (正常) | `CompletedDrain`, 0ms deficit | **PASS** |
| **CF-02** | 48k/24b -> 44.1k/16b | 20.00 | 20.00 | 0.0 | Single Start | なし (1回) | Clean (正常) | `CompletedDrain`, 0ms deficit | **PASS** |
| **CF-03** | 44.1k/24b -> 48k/16b | 20.00 | 20.00 | 0.0 | Single Start | なし (1回) | Clean (正常) | `CompletedDrain`, 0ms deficit | **PASS** |
| **CF-04** | Any with glitch | 20.00 | - | - | - | - | Discontinuity (不連続) | `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` | **PENDING** (判定保留) |

---

## 6. Constraints & Compliance (順守規約)

1. **No Confidential Information**:
   Do not commit copyrighted audio files, Sony disassembled/decompiled code, machine opcodes, user filesystem paths, or raw proprietary logs.
2. **Deterministic Reproducibility**:
   All test clips must be generated using reproducible `ffmpeg` commands from any user-provided lossless tracks.
