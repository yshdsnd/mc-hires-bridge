# 形式が変わる曲送りのEOS処理・停止保留（試験版）

[Issue #20](https://github.com/yshdsnd/mc-hires-bridge/issues/20) および [Issue #21](https://github.com/yshdsnd/mc-hires-bridge/issues/21) に対応する試験実装。Music Center for PC 2.7.4 x86 での実再生検証（`48kHz/24bit -> 48kHz/16bit`、`96kHz/24bit -> 96kHz/24bit`、`192kHz/24bit`、シーク・手動曲変更）が完了するまで、通常のランチャー・配布ZIP（`flac24_override.dll`）には含めない。

## 背景と問題の根本原因

Music Center 2.7.4 x86 における `48kHz/24bit -> 48kHz/16bit` などの形式変更時、以下の2つの独立した問題が連鎖して発生していた：

### 1. 終端約 2.9 秒前の早期停止（Early Stop）の逆アセンブラ解析

`OmgFlacSrc.ax` は曲終端の約 2.9〜3.0 秒前にデコードを完了し、`SeamlessInput->EndOfStream()` を呼び出す。これを受けて `SeamlessFilter.ax` の内部ワーカースレッドが DirectShow イベント `0x0F01`（`3841`）を発火する。

このイベント処理の流れと早期停止のメカニズム：
1. `OpcDSPlay.dll` が DirectShow イベント `3841` を検知し、上位の `OmgPcMan.dll` へイベント `0x13` として中継する。
2. `OmgPcMan.dll` は次曲の先読みのため `COpcFlacPlayer::Open(nextTrackPath, flags=1)`（`OpcFlac.dll` RVA `0x33290`）を呼び出す。
3. `COpcFilterGraphManager::LoadNextSource`（`OpcFlac.dll` RVA `0x35720`）が実行され、次曲（16bit）のピン接続を試みるが、既存グラフ（24bit 出力に構成済み）との形式不一致により `0x80040200`（`VFW_E_INVALIDMEDIATYPE`）で失敗する。
4. **早期停止の核心（RVA `0x33439`）**:
   `COpcFlacPlayer::Open` は先読み接続失敗を検知すると、即座に自クラスの `COpcFlacPlayer::Close()`（RVA `0x319E0`）を呼び出す。この `Close()` が `COpcFilterGraphManager::Close()` を経由して `COpcDSPlay::Stop()` -> `IMediaControl::Stop()` を呼び出す。
5. この結果、DirectSound レンダラーのパイプライン内に約 2.9 秒分（例: 23 秒音源では 20.161 秒時点）の 24bit PCM がまだ再生されずに残っているにもかかわらず、DirectShow グラフ全体が強制停止され、音声が切り捨てられてしまう。
6. `COpcFlacPlayer::Open` は上位の `OmgPcMan.dll` にエラー `0x8000FFFF`（`E_UNEXPECTED`）を返し、`OmgPcMan.dll` は直ちに非シームレスのフォールバックとして通常再生 `Open(flags=0)` / `Play()` を開始するため、前曲の末尾約 2.9 秒が完全に消失したまま次曲へ切り替わっていた。

### 2. `OmgEqxlFilter.ax`（EQ）の内部ブロック保持（末尾約 0.30 秒の欠落）

仮に早期停止を抑止した場合でも、EQ フィルターによる末尾欠落が発生する：
1. `OmgEqxlFilter.ax`（CLSID `{5C968488-5E35-4CFE-98A1-A51C838797DB}`）は `8,192` フレーム単位（48 kHz で約 `170.67 ms`）で処理・出力する。
2. `IPin::EndOfStream` を受け取るまで、内部バッファーに残った未出力フレーム（48 kHz で `10,773`〜`15,232` フレーム＝約 `0.224`〜`0.317` 秒、24.000 秒の音源では `14,400` フレーム＝`0.300` 秒）を出力ピンへフラッシュしない。
3. `SeamlessFilter.ax` はシームレス待機時に下流へ `EndOfStream` を転送しないため、形式変更時は EQ 内部に約 0.30 秒分の PCM が閉じ込められたまま再生終端に達する。

---

## 設計と解決アーキテクチャ

本試験版（`flac24_eos_experiment.dll`）では、23 秒固定判定などの決め打ちを一切排除し、DirectShow グラフごとに動的な状態機械（`eosTransition::Tracker`）と COM フック群で協調制御を行う。

### 1. `IMediaSeeking` COM フックの修正と位置追跡（Issue #21）

`IMediaSeeking` は COM インターフェース（`IUnknown` から継承）であり、vtable のインデックス配置は以下の通り厳密に定義されている：
- Index 11: `GetStopPosition(LONGLONG* pStop)`
- Index 12: `GetCurrentPosition(LONGLONG* pCurrent)`
- Index 14: `SetPositions(LONGLONG* pCurrent, DWORD dwCurrentFlags, LONGLONG* pStop, DWORD dwStopFlags)`

**修正点**:
- 従来の誤った vtable インデックス 11 へのシークフックを index 14（`SetPositions`）へ修正。
- `AM_SEEKING_NoPositioning`（`0x00`）フラグの安全な取り扱い：
  上位から `AM_SEEKING_NoPositioning` で `pCurrent` や `pStop` に nullptr または無効ポインタが渡された場合でも、ポインタを参照せずシークリセットを行わない。
- Index 11（`GetStopPosition`）および Index 12（`GetCurrentPosition`）をフックし、グラフごとの曲終端位置（`stopPosition`）と現在再生位置（`currentPosition`）を常時追跡する。

### 2. 形式変更時の停止保留（Stop Deferral）

1. `OpcFlac.dll` の先読み接続失敗により `IMediaControl::Stop()` が呼ばれた際、フック `controlStopHook` が `tracker.isFormatChangePending(graph)` を判定する。
2. 形式変更が保留中（上流 EOS 到達済み、かつ現在位置が終端位置 `stopPos` 未満、または下流受信バイト数が目標未満）の場合：
   - 即座の `originalControlStop` 呼び出しを保留する。
   - まだ EQ へ `EndOfStream` を送っていなければ、`OmgEqxlFilter.ax` の入力ピンをグラフから探索し、`IPin::EndOfStream()` を**1回だけ**送出して EQ 内の全 PCM を DirectSound へ吐き出させる。
   - `PeekMessage` / `MsgWaitForMultipleObjectsEx` によるメッセージポンプを行いながら、DirectSound が前曲の最終 PCM を `stopPos` まで再生し切るのを待機する（最大 4.5 秒の安全上限付き）。
   - 再生完了後、`originalControlStop` を実行してグラフを正常停止し、次曲への切り替えを完了させる。

### 3. 先読みイベント `3841`（`0x0F01`）と各遷移パターンにおける挙動

- `SeamlessFilter.ax` の独自通知 `3841` は一切抑止せず通常どおり上位へ配送する。
- **同形式の連続再生（`SameFormat24`）**:
  先読み setter（RVA `0x31E7A`）で同レート・2ch・24bit が適用されるか、Flush/Seek なしで次曲のサンプルが `SeamlessInput` に流入した瞬間に `eosPending` を解除する。停止保留も発動せず、シームレス連続再生が完全に維持される。
- **形式変更時（`FormatChanged` / `Unknown`）**:
  先読み失敗による早期 `Stop()` が保留され、EQ への EOS 送出と DirectSound ドレインを経て、末尾まで欠落なく再生された後に安全に次曲へ遷移する。
- **プレイリスト終端（次曲なし）**:
  `OmgPcMan.dll` が `SetEndOfTrack()` を呼ぶか、先読みが発生しないまま終端へ向かう。下流が最終 PCM を受信した時点で EQ へ EOS が送られ、DirectShow の `EC_COMPLETE` まで全フレームが正常出力される。

### 4. 保留状態のクリア条件

以下のいずれかが発生した時点で、対象グラフの保留状態・区間カウンタを即座にリセットし、誤発火を防止する：
- **シーク**: `IMediaSeeking::SetPositions`（`AM_SEEKING_PositioningBitsMask != AM_SEEKING_NoPositioning`）
- **Flush**: `SeamlessFilter.ax` または `OmgEqxlFilter.ax` 入力ピンの `IPin::BeginFlush` / `IPin::EndFlush`
- **停止完了**: `IMediaControl::Stop` の正規実行後
- **手動曲変更・新規トラック開始**: `OpcFlac.dll` 通常開始呼び出し（RVA `0x32A85`）

---

## ビルドとヘッドレス検証テスト

```powershell
cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_EXPERIMENTS=ON
cmake --build build-Win32 --config Release --target flac24_eos_experiment eos_transition_tests
.\build-Win32\Release\eos_transition_tests.exe
```

`eos_transition_tests.exe` は以下の全 10 項目をヘッドレス（GUI・本体起動不要）で検証する：

1. **非 23 秒の任意長および時刻なしサンプル**: 短尺 1.0s、シーク後 29.016s、非ブロック境界フレームでの目標到達後 EOS 発火
2. **重複 EOS 抑止**: 上流・下流からの複数回 `EndOfStream` に対する最大 1 回送出保証
3. **同形式 24bit 先読み維持**: `SameFormat24` および次曲サンプル流入時の EOS 発火抑止と連続再生維持
4. **形式変更（16bit / レート変更）および未確定（`Unknown`）時**: 目標到達後の EOS 送出と、16bit グラフへの非干渉
5. **状態クリア**: `Seek` / `BeginFlush` / `EndFlush` / `Stop` / `ManualTrackChange` による状態リセットと再開始
6. **複数グラフ同時追跡**: 複数 DirectShow グラフ間の完全な分離独立
7. **DirectShow 実 EQ フィルター結合**: `OmgEqxlFilter.ax` インスタンスを用いた `10,773` フレーム内部保持と EOS 1 回送出による全 `51,733` フレーム出力検証
8. **COM vtable フックおよび測位フラグ安全性（Issue #21）**:
   - `IMediaSeeking` vtable index 11 (`GetStopPosition`)、index 12 (`GetCurrentPosition`)、index 14 (`SetPositions`) のフック動作検証
   - `AM_SEEKING_NoPositioning` 渡却時の nullptr 安全性、非シーク時の状態保持検証
   - 実シーク時の位置更新と `ResetReason::Seek` リセット検証
9. **形式変更時の停止保留と `trackTail::analyzeStream` 検証（Issue #21）**:
   - 20.161 秒時点の早期 `Stop()` を保留し、`OmgEqxlFilter.ax` へ EOS を 1 回送出後、23.000 秒まで PCM が出力されることを `#19` の末尾解析器で検証（`tailComplete == true`, `missingTailFrames == 0`, `missingTailMs == 0.0`）
10. **プレイリスト終端ドレインと `trackTail::analyzeStream` 検証（Issue #21）**:
    - 次曲が存在しないケースで最終サンプル受信時に EOS が送出され、全 PCM が出力されることを検証

---

## Music Center 本体での実機検証手順

> [!IMPORTANT]
> `flac24_eos_experiment.dll` は通常版 `flac24_override.dll` と同一の DirectShow 関数をフックするため、**絶対に同一プロセスへ重ねてロードしないこと**。切り替える際は必ず Music Center を完全終了して再起動する。

1. `inject.exe` と試験用 DLL をビルドする：
```powershell
cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_DIAGNOSTICS=ON -DMC_BRIDGE_BUILD_EXPERIMENTS=ON
cmake --build build-Win32 --config Release --target inject flac24_eos_experiment
```

2. `MusicCenter.exe` を単体で起動し、再生前に対象 PID へロードする：
```powershell
$targetPid = (Get-Process MusicCenter).Id
$dll = (Resolve-Path .\build-Win32\Release\flac24_eos_experiment.dll).Path
& .\build-Win32\Release\inject.exe $targetPid $dll
```

3. Music Center の画面右上に `HR-BR RDY` が表示され、`%TEMP%\Flac24Override-<PID>.tsv` に `READY` が出力されたことを確認してから再生を開始する。
4. 以下の再生パターンの遷移音声をスピーカーまたはループバック録音で確認する：
   - `48kHz/24bit -> 48kHz/16bit`（末尾約 2.9 秒および 0.30 秒が途切れず自然に次曲へ遷移すること）
   - `96kHz/24bit -> 96kHz/24bit`（シームレス先読みが維持されギャップレスに再生されること）
   - 再生中のシーク操作、一時停止、手動曲送りでノイズやフリーズが発生しないこと
   - ログには通常の `SET_24` / `PASS_THROUGH` / `STATUS_ON` に加え、`EOS_NEW_SEGMENT`、`EOS_UPSTREAM`、`EOS_PREREAD_24` / `EOS_PREREAD_DIFF`、`EOS_DELIVERED_DOWNSTREAM`（または `EOS_DELIVERED_IMMEDIATE`）、`EOS_RESET_*` が記録される。
5. 試験終了時は Music Center を完全終了して再起動すると解除される。
