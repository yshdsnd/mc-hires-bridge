# 異形式曲送り時の Pause 保留・手動操作・終端検証（Issue #23 / #24 / #25 / #27）

[Issue #22](https://github.com/yshdsnd/mc-hires-bridge/issues/22) の調査結果に基づき、[Issue #23](https://github.com/yshdsnd/mc-hires-bridge/issues/23)、[Issue #24](https://github.com/yshdsnd/mc-hires-bridge/issues/24)、[Issue #25](https://github.com/yshdsnd/mc-hires-bridge/issues/25)、および [Issue #27](https://github.com/yshdsnd/mc-hires-bridge/issues/27) にて実装・強化した試験版（`flac24_pause_experiment.dll` / `flac24_pause_diagnostic.dll`）の設計、排他ロックと同期 EOS の解析結果、手動操作・終端状態遷移、検証結果、および Music Center 実機での動作確認手順をまとめる。

本試験ターゲットは、実再生での総合検証が完了するまで通常の配布パッケージ（`cmake --install` およびリリース ZIP）からは除外される。

---

## 1. 曲末の早期停止で観測した挙動

Music Center for PC 2.7.4 x86 では、48 kHz/24 bit → 48 kHz/16 bit FLAC の自動曲送りで、23秒の前曲が約20.1秒で途切れた。#22 の診断では、通知3841、Pause、DirectSound停止、位置変更の順を観測した。#23 の24 bit観測モードでも同じ停止順を確認した。

調査時の仮説は、先読み後の早期Pauseと後続の位置変更が、旧経路に残る音声の出力を中断するというもの。#23 の保留モードでは、前曲は23秒の末尾まで出力され、約160 ms後に次曲が始まった。

---

## 2. 保留中の手動操作で観測した挙動と排他制御の課題

#24 の実機測定では、Pause保留中に一時停止とシークを入力しても、取消は記録されなかった。EQへの同期EOS呼び出しの復帰まで約1.6〜1.7秒かかった。

#25 の試験版では早期の同期 EOS 呼び出しを回避し、下流未達時は待機へ即時進み別スレッドから EOS を送出することで 48 kHz/24 bit → 48 kHz/16 bit FLAC の曲末を保持できた。
しかし実機では、保留中に発行した一時停止・シーク・終了の取消が DirectShow 側に記録されず、入力から約1〜2.7秒後に通常の曲末処理が進む事象が残存した。

この要因として、以下の2つの経路が考えられる（#27）：
1. **指令がエンジンに未到達**: アプリ側の UI 層（CEF / イベント処理）でボタン押下やシーク操作が拾われないか、保留待機中にエンジン呼び出しまでディスパッチされていない。
2. **到達した指令がロック取得を待つ**: 指令は再生エンジンに到達しているが、Pause 保留を実行中のスレッドがエンジンの排他制御（CriticalSection）を保持し続けており、手動操作側スレッドがロック取得待ちで停止している。

---

## 3. 解決アーキテクチャと診断機能（Issue #25 / #27 対策）

### 1. 早期同期 EOS 送出の抑止（`hasDownstreamReachedTarget`）
- `deferFormatChangePause` 開始時点で、下流が目標 PCM を受信済みか（`hasDownstreamReachedTarget(key)`）を判定。
- **目標未達の場合（曲末尾手前）**:
  `deferFormatChangePause` は同期 EOS を叩かず、直ちに待機ループ（メッセージポンプ）へ入る。
  残りの PCM は通常通りストリーミングスレッド経由で `eqReceiveHook` が受信し、**最終 PCM が EQ に到達した瞬間に `eqReceiveHook` 自身が `originalEqEndOfStream` を 1 回だけ送出する**。
- これにより、`deferFormatChangePause` 開始時の 1.6〜1.7 秒に及ぶ不必要な同期ブロッキングが完全に解消され、112 ms や 144 ms 時点で待機ループが既にアクティブかつメッセージポンプ実行中となる。

### 2. アプリ指令受付・排他ロック追跡・高精度単調時刻（`flac24_pause_diagnostic`）
手動操作の指令がどこで止まっているかを追跡するため、専用診断ターゲット `flac24_pause_diagnostic` および環境変数切り替え（`MC_BRIDGE_PAUSE_MODE=diag` / `MC_BRIDGE_DIAGNOSTICS=1`）を拡張。

#### 診断ログ仕様
- **単調時刻**: `QueryPerformanceCounter`（QPC）によるマイクロ秒精度オフセット（`mono_ms`）。
- **スレッド ID**: `tid`（UI スレッドとワーカースレッドの分離観測）。
- **グラフ識別子 / オブジェクト**: `graph`（COM オブジェクトポインタまたはロックアドレス）。
- **呼出元 RVA 解決**: `caller`（例: `OpcDSPlay.dll+0xDAC9`, `USER32.dll` 等）。
- **アプリ指令受付イベント**:
  - `APP_INPUT_DISPATCH`: Win32 メッセージループでのマウス操作（`WM_LBUTTONDOWN` / `UP`）、キー入力（`WM_KEYDOWN` / `UP`）、メニュー・アクセラレータ（`WM_COMMAND`）のディスパッチ時刻・対象 HWND・パラメータ。
- **エンジン排他ロックイベント**:
  - `ENGINE_LOCK_TRY`: 再生エンジン（`OpcDSPlay.dll` 等）からの `EnterCriticalSection` 呼び出し試行。
  - `ENGINE_LOCK_ACQUIRED`: ロック取得成功。`p2`（`wait_ms`）にロック待ち時間をミリ秒で記録。
  - `ENGINE_LOCK_RELEASED`: ロック解放（`LeaveCriticalSection`）。`p2`（`held_ms`）にロック保持時間をミリ秒で記録。
  - `ENGINE_LOCK_TRY_SUCCEEDED` / `FAILED`: `TryEnterCriticalSection` 実行結果。
- **DirectShow / フィルター境界イベント**:
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

#### 観測可能な境界と観測できない境界
- **観測可能な境界**:
  1. Win32 メッセージキューにおけるユーザー入力の到着とディスパッチ（`APP_INPUT_DISPATCH`）。
  2. 再生エンジン（`OpcDSPlay.dll` 等）が排他ロックを取得・待機・解放する全ライフサイクル（`ENGINE_LOCK_*`）。
  3. DirectShow API（`IMediaControl`, `IMediaSeeking`）への到達（`BOUNDARY_*`）。
- **観測できない境界とその理由**:
  1. Chromium Embedded Framework（CEF）内部の JavaScript DOM イベント処理（`onclick`）および内部 IPC:
     CEF の内部シンボルは非公開かつプロセス内で静的/動的に隠蔽されているため、DOM 内部の JavaScript 実行行を直接フックすることはできません。
     しかし、Win32 メッセージの投入点（`APP_INPUT_DISPATCH`）とネイティブエンジン呼び出し点（`ENGINE_LOCK_TRY`）を両端から記録することで、CEF 内部での脱落や遅延を完全に挟み込んで判別可能です。

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

## 5. ヘッドレス検証（`eos_transition_tests` 全 25 項目）

本実装に対する全 25 項目の単体・結合・マルチスレッド並行テストを実装し、Win32 Release ビルドで全件合格を確認済み。

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
- **Test 19〜22（Issue #25）**:
  - **Test 19**: 同期 EQ EOS 実行中の別スレッド（UI スレッド）からの並行手動操作検証
  - **Test 20**: ドレイン待機中の並行手動操作検証
  - **Test 21**: 早すぎる EQ EOS 送出抑止（`hasDownstreamReachedTarget`）検証
  - **Test 22**: 取消サイクル後の 24bit 末尾保持および同形式先読み維持検証
- **Test 23〜25（Issue #27 新設）**:
  - **Test 23: 到達した指令が排他ロック取得を待つケースの追跡（Case B）**:
    Thread A が保留待機中にエンジン CriticalSection を保持し、Thread B（UI 操作）が `EnterCriticalSection` を呼んだ際、`ENGINE_LOCK_TRY` が直ちに記録され、Thread A の解放後に `ENGINE_LOCK_ACQUIRED` が `wait_ms > 0` で記録されることを検証。
  - **Test 24: 指令がエンジンに未到達のケースの識別（Case A）**:
    UI 層で `APP_INPUT_DISPATCH` が発生したが、エンジン側への呼び出しが発生しなかった場合、ログに `ENGINE_LOCK_TRY` が 0 件となることで「指令がエンジンに未到達」と明確に判定できることを検証。
  - **Test 25: 観測モードにおける単調時刻整合性と非介入性の検証**:
    観測モード（`PauseMode::Observe`）において、再生制御やロックの取得順序を一切変更せず、すべてのイベント（UI 入力、ロック、DirectShow 呼び出し、EOS）が単調増加時刻（`mono_ms`）とスレッド ID（`tid`）に沿って記録されることを検証。

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
# 観測モード（既存動作の排他競合観測）
$env:MC_BRIDGE_PAUSE_MODE = "observe"
$env:MC_BRIDGE_DIAGNOSTICS = "1"
& "C:\Program Files (x86)\Sony\Music Center\MusicCenter.exe"

# または保留モード（手動操作追跡）
$env:MC_BRIDGE_PAUSE_MODE = "defer"
$env:MC_BRIDGE_DIAGNOSTICS = "1"
& "C:\Program Files (x86)\Sony\Music Center\MusicCenter.exe"
```

### 3. 保留区間（20.1秒〜23.0秒）でのログ判定方法

ログ出力（`%TEMP%\Flac24Override-<PID>.tsv`）を時系列（`mono_ms`）順に確認し、以下の判定基準に照合します：

1. **「到達した指令がロック取得を待つ」場合（Case B）**:
   - UI 操作時刻に `ENGINE_LOCK_TRY`（操作側の `tid`）が記録されている。
   - しかし `ENGINE_LOCK_ACQUIRED` がすぐには出力されず、保留解除スレッドの `ENGINE_LOCK_RELEASED` 直後に出力される。
   - `ENGINE_LOCK_ACQUIRED` の `p2`（`wait_ms`）に 1000〜2500ms などの大きな待機時間が記録されている。
   -> **判定**: 指令はエンジンに到達しているが、Pause 保留スレッドが CriticalSection を解放するまでブロックされていた。

2. **「指令がエンジンに未到達」の場合（Case A）**:
   - UI 操作時刻に `APP_INPUT_DISPATCH`（マウス押下等）が記録されている。
   - しかし、その後の保留待機区間（1〜2.7秒間）に、手動操作側の `tid` から `ENGINE_LOCK_TRY` が 1 度も記録されない。
   -> **判定**: アプリ UI（CEF / UI コントローラー）側で指令が破棄・保留されており、再生エンジン自体が呼ばれていなかった。

---

## 7. 配布安全性
- `cmake --install` の出力には通常版バイナリとライセンスファイルのみが含まれ、`flac24_pause_experiment.dll`、`flac24_pause_diagnostic.dll`、およびテストバイナリは一切含まれないことを確認済みです。
- 個人の楽曲、パス、PCM、生ログ、生成バイナリはコミットに含まれません。
- 第三者バイナリ由来の逆アセンブリ命令列、機械語バイト列、逆コンパイル出力は含みません（#26）。
