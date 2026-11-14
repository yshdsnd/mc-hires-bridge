# 異形式曲送り時の Pause 保留・手動操作・終端検証（Issue #23 / #24 / #25）

[Issue #22](https://github.com/yshdsnd/mc-hires-bridge/issues/22) の調査結果に基づき、[Issue #23](https://github.com/yshdsnd/mc-hires-bridge/issues/23)、[Issue #24](https://github.com/yshdsnd/mc-hires-bridge/issues/24)、および [Issue #25](https://github.com/yshdsnd/mc-hires-bridge/issues/25) にて実装・強化した試験版（`flac24_pause_experiment.dll` / `flac24_pause_diagnostic.dll`）の設計、排他ロックと同期 EOS の解析結果、手動操作・終端状態遷移、検証結果、および Music Center 実機での動作確認手順をまとめる。

本試験ターゲットは、実再生での総合検証が完了するまで通常の配布パッケージ（`cmake --install` およびリリース ZIP）からは除外される。

---

## 1. 曲末の早期停止で観測した挙動

Music Center for PC 2.7.4 x86 では、48 kHz/24 bit → 48 kHz/16 bit FLAC の自動曲送りで、23秒の前曲が約20.1秒で途切れた。#22 の診断では、通知3841、Pause、DirectSound停止、位置変更の順を観測した。#23 の24 bit観測モードでも同じ停止順を確認した。

調査時の仮説は、先読み後の早期Pauseと後続の位置変更が、旧経路に残る音声の出力を中断するというもの。#23 の保留モードでは、前曲は23秒の末尾まで出力され、約160 ms後に次曲が始まった。

---


## 2. 保留中の手動操作で観測した挙動

#24 の実機測定では、Pause保留中に一時停止とシークを入力しても、取消は記録されなかった。EQへの同期EOS呼び出しの復帰まで約1.6〜1.7秒かかった。

#25 の試験版では、保留開始から約1 msで待機へ進み、別スレッドからEOSが送られた。曲末は保持できたが、保留中の一時停止・シーク・終了は即時には反映されなかった。共有される内部の排他制御が指令を待たせている可能性がある。指令受付とロック取得前後の時刻は未計測。

---


## 3. 解決アーキテクチャと診断機能（Issue #25 対策）

### 1. 早期同期 EOS 送出の抑止（`hasDownstreamReachedTarget`）
- `deferFormatChangePause` 開始時点で、下流が目標 PCM を受信済みか（`hasDownstreamReachedTarget(key)`）を判定。
- **目標未達の場合（曲末尾手前）**:
  `deferFormatChangePause` は同期 EOS を叩かず、直ちに待機ループ（メッセージポンプ）へ入る。
  残りの PCM は通常通りストリーミングスレッド経由で `eqReceiveHook` が受信し、**最終 PCM が EQ に到達した瞬間に `eqReceiveHook` 自身が `originalEqEndOfStream` を 1 回だけ送出する**。
- これにより、`deferFormatChangePause` 開始時の 1.6〜1.7 秒に及ぶ不必要な同期ブロッキングが完全に解消され、112 ms や 144 ms 時点で待機ループが既にアクティブかつメッセージポンプ実行中となる。

### 2. 高精度単調時刻・スレッド追跡・任意診断ビルド（`flac24_pause_diagnostic`）
手動操作の指令がどこまで届いているかを正確に検証するため、専用診断ターゲット `flac24_pause_diagnostic` および環境変数切り替え（`MC_BRIDGE_PAUSE_MODE=diag` / `MC_BRIDGE_DIAGNOSTICS=1`）を実装。

#### 診断ログ仕様
- **単調時刻**: `QueryPerformanceCounter`（QPC）によるマイクロ秒精度オフセット（`mono_ms`）。
- **スレッド ID**: `tid`（UI スレッドとワーカースレッドの分離観測）。
- **グラフ識別子**: `graph`（COM オブジェクトポインタ）。
- **呼出元 RVA 解決**: `caller`（例: `OpcDSPlay.dll+0xDAC9`, `USER32.dll` 等）。
- **境界イベント**:
  - `BOUNDARY_CONTROL_PAUSE_ENTER` / `LEAVE`
  - `BOUNDARY_FILTER_PAUSE_ENTER` / `LEAVE`
  - `BOUNDARY_CONTROL_STOP_ENTER` / `LEAVE`
  - `BOUNDARY_FILTER_STOP_ENTER` / `LEAVE`
  - `BOUNDARY_SEEK_SETPOSITIONS_ENTER` / `LEAVE`
  - `BOUNDARY_TRACK_OPEN`
  - `PAUSE_DEFERRED_START`
  - `EQ_EOS_CALL_ENTER` / `LEAVE`（所要時間をミリ秒で記録）
  - `DRAIN_WAIT_ENTER` / `DRAIN_WAIT_TICK`（250ms ごとの現在位置・目標進捗）
  - `DRAIN_WAIT_CANCEL_DETECTED`（取消検知時の経過時間・ステータス）
  - `PAUSE_DEFERRED_RELEASE`

---

## 4. 保留中の手動操作・終端状態遷移（`PauseDeferralStatus`）

保留待機中（約 2.9 秒のドレイン区間）にメッセージポンプを経由してユーザー操作やウィンドウイベントが発生した場合、状態機械（`eosTransition::Tracker`）とフック群が連携して直ちに安全な状態遷移を行います。

| 解除ステータス | 値 | 発生契機 | 保留待機ループの挙動 | グラフ状態への作用 |
| :--- | :--- | :--- | :--- | :--- |
| `CompletedDrain` | 2 | 全 PCM が DirectSound から出力完了（`curPos >= stopPos`） | 正常脱出 | 保留解除、本来の `Pause()` を実行し、次曲の Flush/SetPositions へ引き渡し（末尾欠落 0ms） |
| `CancelledStop` | 3 | 待機中にユーザーが停止ボタン押下、または `Stop` 発行 | 即座に待機脱出 | 多重保留を行わず即座に `originalControlStop` を実行して停止 |
| `CancelledSeek` | 4 | 待機中に再生バーで位置変更（シーク）操作 | 即座に待機脱出 | 保留破棄、`originalControlPause` をバイパス（`CONTROL_PAUSE_BYPASS_CANCEL`）してシーク先からの再生を継続 |
| `CancelledTrackChange` | 5 | 待機中に次曲/前曲スキップボタン押下（手動曲送り） | 即座に待機脱出 | 保留破棄、直ちに次曲の Open/Play 処理へ移行 |
| `CancelledManualPause` | 6 | 待機中に手動で一時停止ボタン押下（外部 caller からの Pause） | 即座に待機脱出 | 保留破棄、直ちに `originalControlPause` を実行して一時停止状態へ移行 |
| `CancelledQuit` | 7 | `WM_QUIT` 受信（ウィンドウ終了・アプリ終了） | 即座に待機脱出 | `PostQuitMessage` を再送出し、UI メインループへ終了通知 |
| `TimedOut` | 8 | DirectSound ストール等により上限時間（最大 4.5 秒）超過 | 安全脱出 | 安全上限でループを抜け、`originalControlPause` を実行してハングを防止 |

---

## 5. ヘッドレス検証（`eos_transition_tests` 全 22 項目）

本実装に対する全 22 項目の単体・結合・マルチスレッド並行テストを実装し、Win32 Release ビルドで全件合格を確認済み。

### ビルドとテスト実行手順
```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_EXPERIMENTS=ON
& $cmake --build build-Win32 --config Release --target eos_transition_tests flac24_pause_experiment flac24_pause_diagnostic
.\build-Win32\Release\eos_transition_tests.exe
```

### テスト項目一覧
- **Test 1〜10**:
  - 非 23 秒任意長・時刻なしサンプル
  - 重複 EOS 抑止保証
  - 同形式 24bit 先読み維持
  - 形式変更・未確定時の EOS 送出
  - Seek / Flush / Stop / TrackChange による状態クリア
  - 複数 DirectShow グラフの同時追跡
  - DirectShow 実 EQ フィルター内部保持フレーム出力
  - COM vtable フックおよび測位フラグ安全性
  - Stop 保留時の `trackTail::analyzeStream` 検証
  - プレイリスト終端ドレイン検証
- **Test 11〜13（Issue #23）**:
  - `IMediaControl::Pause` 保留と実ストリーム末尾検証（`tailComplete == true`, `missingTailFrames == 0`）
  - 観察モード（`observe`）と実験モード（`defer`）の切り替え検証
  - 非形式変更時の Pause 非保留検証
- **Test 14〜18（Issue #24）**:
  - 保留中の手動操作（Stop, Seek, TrackChange, ManualPause, Exit）状態遷移検証
  - ドレイン待ちタイムアウト安全機構検証（デッドロック回避）
  - 再入防止および古いグラフへの重複 EOS 抑止検証
  - 異なるサンプルレート遷移（`96kHz/24bit -> 44.1kHz/16bit` 等）での末尾欠落 0ms 検証
  - 24bit プレイリスト終端（異レート・次曲なし）検証
- **Test 19〜22（Issue #25 新設）**:
  - **Test 19: 同期 EQ EOS 実行中の別スレッド（UI スレッド）からの並行手動操作検証**:
    同期呼び出しブロック中に UI スレッドが Stop / Seek を呼んだ際、排他ロックのデッドロックを起こさず取消ステータスがセットされ、復帰直後に待機ループが即座に中止されることを検証。
  - **Test 20: ドレイン待機中の並行手動操作検証**:
    待機ループ中に UI スレッドが一時停止を呼んだ際、15ms 以内に待機ループが検知・脱出することを検証。
  - **Test 21: 早すぎる EQ EOS 送出抑止（`hasDownstreamReachedTarget`）検証**:
    下流未達時点で保留が開始されても EOS を叩かず、ストリーミング完了時に初めて EOS が送出されることを検証。
  - **Test 22: 取消サイクル後の 24bit 末尾保持および同形式先読み維持検証**:
    シーク取消が発生した後の次曲再生において、同形式（48kHz/24bit）先読みが維持され、中間 EOS 送出なく末尾まで完全出力されることを `#19` 末尾解析器で検証。

---

## 6. Music Center 実機での動作確認手順（Codex 用）

実機検証を行う際は、以下の手順に従って診断モードログ採取と手動操作の反映確認を実施してください。

### 1. 診断版 DLL のビルドと配置
```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_EXPERIMENTS=ON
& $cmake --build build-Win32 --config Release --target flac24_pause_diagnostic
```
ビルドされた `build-Win32\Release\flac24_pause_diagnostic.dll` を配置します（または `flac24_pause_experiment.dll` で `$env:MC_BRIDGE_DIAGNOSTICS = "1"` を指定）。

### 2. 診断ログを有効にしての検証実行
```powershell
$env:MC_BRIDGE_PAUSE_MODE = "defer"
$env:MC_BRIDGE_DIAGNOSTICS = "1"
$env:MC_BRIDGE_LOG_PATH = "C:\path\to\diag.log"
# Music Center を起動し、48kHz/24bit -> 48kHz/16bit の自動曲送りを再生
```

### 3. 保留区間（20.1秒〜23.0秒）での手動操作テスト

| 操作 | タイミング | 確認すべき診断ログ | 期待される挙動 |
| :--- | :--- | :--- | :--- |
| **手動一時停止** | 保留開始直後（100〜200ms）に「一時停止」ボタン押下 | `BOUNDARY_CONTROL_PAUSE_ENTER`<br>`PAUSE_DEFERRED_ABORT_BY_MANUAL_PAUSE`<br>`DRAIN_WAIT_CANCEL_DETECTED (status=6)` | 1.7秒待たされずに即座に一時停止すること。`tid` と `mono_ms` で UI スレッドの到達時刻を確認。 |
| **手動シーク** | 保留開始直後（100〜200ms）にシークバーをクリック | `BOUNDARY_SEEK_SETPOSITIONS_ENTER`<br>`PAUSE_DEFERRED_ABORT_BY_SEEK`<br>`CONTROL_PAUSE_BYPASS_CANCEL` | Pause せずに即座にシーク先から再生が継続すること。 |
| **通常曲送り（操作なし）** | 操作せずに自動曲送りを完了 | `EQ_EOS_CALL_ENTER`（終端付近で実行）<br>`PAUSE_DEFERRED_RELEASE (status=2: CompletedDrain)` | 前曲が末尾まで 100% 再生され、次曲がスムーズに開始すること。 |

---

## 7. 配布安全性
- `cmake --install` の出力には通常版バイナリとライセンスファイルのみが含まれ、`flac24_pause_experiment.dll`、`flac24_pause_diagnostic.dll`、およびテストバイナリは一切含まれないことを確認済みです。
- 個人の楽曲、パス、PCM、生ログ、生成バイナリはコミットに含まれません。
