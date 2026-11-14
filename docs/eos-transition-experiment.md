# 形式が変わる曲送りのEOS処理（試験版）

[Issue #20](https://github.com/yshdsnd/mc-hires-bridge/issues/20) に対応する試験実装。Music Center for PC 2.7.4 x86 での実再生検証（`48kHz/24bit -> 48kHz/16bit`、`96kHz/24bit -> 96kHz/24bit`、`192kHz/24bit`、シーク・手動曲変更）が完了するまで、通常のランチャー・配布ZIP（`flac24_override.dll`）には含めない。

## 背景と原因

1. **`OmgEqxlFilter.ax`（EQ）の内部ブロック保持**:
   - `OmgEqxlFilter.ax`（CLSID `{5C968488-5E35-4CFE-98A1-A51C838797DB}`）は `8,192` フレーム単位（48 kHz で約 `170.67 ms`）で出力し、`IPin::EndOfStream` を受けるまで末尾の未出力フレーム（48 kHz で `10,773`〜`15,232` フレーム＝約 `0.224`〜`0.317` 秒、24.000 秒の音源では `14,400` フレーム＝`0.300` 秒）を内部バッファーに保持し続ける。
2. **`SeamlessFilter.ax` の EOS 非転送**:
   - `SeamlessFilter.ax`（CLSID `{6AF73F1F-1266-46E6-9A36-EE75E4BA10BB}`、PE timestamp `0x63EB12BA`）は、上流 `OmgFlacSrc.ax` から `SeamlessInput->EndOfStream()` を再生終端の約 3 秒前に受け取るが、通常のシームレス待機状態では下流（`OmgEqxlFilter.ax`）へ `EndOfStream` をそのまま転送しない。
   - 同形式の次曲が続く場合は `SeamlessInput` に次曲の `OmgFlacSrc.ax` が接続されて PCM が連続するため問題ないが、`48kHz/24bit -> 48kHz/16bit` のように形式が変わる曲送りでは次曲が `SeamlessFilter.ax` に接続されず、`OmgEqxlFilter.ax` が最終 PCM サンプルを受け取った後も `EndOfStream` が届かないまま再生位置が終端へ達し、最終約 `0.30` 秒が無音（未フラッシュ）になる。

## 設計（23秒固定判定の廃止と区間追跡）

固定秒数（23 秒など）の判定は一切使わず、`experiments/eos_transition/eos_transition.h` の `eosTransition::Tracker` で DirectShow グラフ（`IFilterGraph*`）ごとに再生区間とストリーム終端を追跡する。

1. **上流（`SeamlessFilter.ax` 入力ピン）の追跡**:
   - `IPin::NewSegment(tStart, tStop, rate)` で区間情報を記録し、新しい区間のカウンタを初期化する（ただし上流 `EndOfStream` 後に Flush/Seek/Stop なしで新しい `NewSegment` が来た場合は同形式のシームレス継続として扱う）。
   - `IMemInputPin::Receive(pSample)` でサンプル長（`GetActualDataLength()`）と `SeamlessFilter` 通過後のサンプル終端時刻（`GetTime(&sStart, &sEnd)`）を累積する。
   - `IPin::EndOfStream()` 初回呼び出し時に、その区間の最終到達目標（`targetBytes = upstreamBytes`、`targetEndTime = upstreamLastEndTime`）を確定し、同形式先読みが確定していない限り `eosPending = true` にする。同一区間での重複 `EndOfStream()` 呼び出しは無視し、`eosDelivered` をリセットしない。
2. **下流（`OmgEqxlFilter.ax` 入力ピン）への最大1回 EOS 送出**:
   - `IMemInputPin::Receive(pSample)` の元呼び出し成功後に下流の累積バイト数とサンプル終端時刻を更新する。
   - `active24 && eosPending && !eosDelivered && preReadDecision != SameFormat24` の状態で、下流が上流区間の最終 PCM（`sEnd >= targetEndTime` かつ `downstreamBytes >= targetBytes`、時刻なしサンプルでは `downstreamBytes >= targetBytes`）を受け取った瞬間に、`eosDelivered = true` / `eosPending = false` に遷移して `OmgEqxlFilter.ax` 入力ピンへ `IPin::EndOfStream()` を**1回だけ**送る。
   - 下流ピンへ直接 `IPin::EndOfStream()` が呼ばれた場合も、`eosDelivered == true` なら重複呼び出しを抑止して最大 1 回を保証する。

## 同形式の先読み維持と、形式変更を判定できない場合の挙動

### 1. 同形式の先読み（`SameFormat24`）の維持

- `SeamlessFilter.ax` の独自通知 `3841`（`0xF01`、終端約 3 秒前）を抑止しないため、Music Center は通常どおり次曲の先読みを開始できる。
- 次曲が同じサンプルレート・2ch・24bit FLAC の場合、終端の約 2.95 秒前に `OpcFlac.dll` の先読み呼び出し（RVA `0x31E7A`）で `SET_24` が適用され（`PreReadDecision::SameFormat24`）、続いて次曲の `OmgFlacSrc.ax` から `SeamlessInput->Receive()` へ新しい PCM サンプルが流入する。
- `eosTransition::Tracker` は以下のいずれかを検知した時点で `eosPending = false` に解除し、曲境界で `OmgEqxlFilter.ax` へ `EndOfStream` を送らない：
  1. 先読み setter（RVA `0x31E7A`）で現在グラフと同一サンプルレート・2ch・24bit の `SET_24` が成功したとき。
  2. 上流 `SeamlessInput->EndOfStream()` の後に、Flush/Seek/Stop を挟まず `SeamlessInput` に次曲の新しい PCM サンプルまたは `NewSegment` が到着したとき。
- これにより、`48kHz/24bit -> 48kHz/24bit` や `96kHz/24bit -> 96kHz/24bit` のシームレス先読み・連続再生をそのまま維持する。

### 2. 形式変更を判定できない場合（`PreReadDecision::Unknown`）の挙動

Music Center 2.7.4 x86 では、以下のケースで先読み用の RVA `0x31E7A` が呼ばれないため、setter フックだけでは「次曲が別形式か、再生終了か」を事前に区別できない（`PreReadDecision::Unknown`）：

1. **`OpcFlac.dll` 内部の FLAC ヘッダー事前比較で弾かれる場合**:
   `OpcFlac.dll`（RVA `0x33290` 内の `0x33386`〜`0x3340D`）は、2 個目の `OmgFlacSrc.ax` を生成する前に FLAC の `STREAMINFO` ヘッダー（`sample_rate` / `channels` / `bits_per_sample`）を前曲と比較し、1 つでも異なると RVA `0x32360`（`0x31E7A`）を呼ばずに先読み接続を見送る（例：`48kHz/24bit -> 48kHz/16bit`、`96kHz/24bit -> 48kHz/24bit`）。
2. **次曲が FLAC 以外の形式（WAV / MP3 / AAC / DSD 等）の場合**:
   別の再生プラグインが使われるため、`OpcFlac.dll` の RVA `0x31E7A` は呼ばれない。
3. **プレイリスト／アルバムの最終曲で次曲が存在しない場合**:
   先読み自体が行われないため、RVA `0x31E7A` は呼ばれない。

**本試験版の挙動**:
- 上流 `SeamlessInput->EndOfStream()` から下流 `OmgEqxlFilter.ax` が最終 PCM を受け取るまでの間（`SeamlessFilter` の出力キュー約 3 秒分のドレイン期間）に、同形式の先読み（RVA `0x31E7A` での同レート `SET_24`、または `SeamlessInput` への次曲サンプル流入）が発生しなかった場合、`PreReadDecision::Unknown` は「現在の `SeamlessFilter` ストリームはこの区間で終了する（シームレス継続しない）」と判定し、下流 `OmgEqxlFilter.ax` が当該区間の最終 PCM を受け取った直後に `IPin::EndOfStream()` を**1回だけ**送る。
- これにより、RVA `0x31E7A` で `FormatChanged` を検知できるケースだけでなく、`OpcFlac.dll` のヘッダー比較で `0x31E7A` 以前に先読みが見送られる `48kHz/24bit -> 48kHz/16bit` や他形式遷移・最終曲でも、末尾約 `0.30` 秒の EQ 内部保持フレームが確実にフラッシュされる。

## 保留EOS状態のクリア条件

以下のいずれかが発生した時点で、対象グラフの `eosPending` / `eosDelivered` / 区間累積カウンタを即座にクリアし、古い区間の目標値による誤発火を防ぐ：

- **シーク**: `IMediaSeeking::SetPositions`（位置変更フラグを伴う呼び出し）
- **Flush**: `SeamlessFilter.ax` または `OmgEqxlFilter.ax` 入力ピンの `IPin::BeginFlush` / `IPin::EndFlush`
- **停止**: `IMediaControl::Stop` / `IMediaFilter::Stop`
- **手動曲変更・新規トラック開始**: `OpcFlac.dll` 通常開始呼び出し（RVA `0x32A85`）

## ビルドとヘッドレス状態遷移テスト

```powershell
cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_EXPERIMENTS=ON
cmake --build build-Win32 --config Release --target flac24_eos_experiment eos_transition_tests
.\build-Win32\Release\eos_transition_tests.exe
```

`eos_transition_tests.exe` は GUI や Music Center 本体の起動を必要とせず、以下を検証する：

1. 非 23 秒の任意長（`1.0s`、シーク後の `29.016s`、非ブロック境界の `51,733` フレーム＝約 `1.0778s`）および時刻なしサンプルでの最終 PCM 到達後 EOS 発火
2. 上流・下流の重複 `EndOfStream` 呼び出しに対する最大 1 回送出保証
3. 同形式 24bit 先読み（`SameFormat24` および次曲サンプル流入）での保留 EOS 解除と連続再生維持
4. 形式変更（16bit / サンプルレート変更）および形式変更未確定（`Unknown`）時の最終 PCM 到達後 EOS 送出、16bit 非対象グラフでの非干渉
5. `Seek` / `BeginFlush` / `EndFlush` / `Stop` / `ManualTrackChange` による保留 EOS クリアと再開始
6. 複数グラフ同時追跡時の独立性
7. Win32 環境での実 `OmgEqxlFilter.ax` COM インスタンスとの結合テスト（EOS 送出前の `10,773` フレーム内部保持と、最終チャンクでの EOS 1 回送出による全 `51,733` フレーム＋同形式 2 曲連続 `48,000` フレームの出力確認）

## Music Center 本体での試験起動手順

> [!IMPORTANT]
> `flac24_eos_experiment.dll` は通常版 `flac24_override.dll` や診断用 `end_probe.dll` と同じ DirectShow / MinHook 対象をフックするため、**絶対に同一プロセスへ重ねてロードしないこと**。切り替える際は必ず Music Center を完全終了して再起動する。

1. 通常の `MC HiRes Bridge.exe` を使わず、`MusicCenter.exe` を単体で起動し、再生を始める前に対象 PID を確認する。
2. 診断ツールの `inject.exe` をビルドしていない場合は `-DMC_BRIDGE_BUILD_DIAGNOSTICS=ON` を付けて `inject` をビルドする。

```powershell
cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_DIAGNOSTICS=ON -DMC_BRIDGE_BUILD_EXPERIMENTS=ON
cmake --build build-Win32 --config Release --target inject flac24_eos_experiment
```

3. 初回再生前に対象 PID へ `flac24_eos_experiment.dll` をロードする：

```powershell
$targetPid = (Get-Process MusicCenter).Id
$dll = (Resolve-Path .\build-Win32\Release\flac24_eos_experiment.dll).Path
& .\build-Win32\Release\inject.exe $targetPid $dll
```

4. Music Center 上で `HR-BR RDY` 表示と `%TEMP%\Flac24Override-<PID>.tsv` の `READY` を確認してから再生を開始する。
   - ログには通常の `SET_24` / `PASS_THROUGH` / `STATUS_ON` に加え、`EOS_NEW_SEGMENT`、`EOS_UPSTREAM`、`EOS_PREREAD_24` / `EOS_PREREAD_DIFF`、`EOS_DELIVERED_DOWNSTREAM`（または `EOS_DELIVERED_IMMEDIATE`）、`EOS_RESET_*` が記録される。
5. 試験終了時は Music Center を完全終了して再起動すると解除される。
