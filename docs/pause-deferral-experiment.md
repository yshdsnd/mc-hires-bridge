# 異形式曲送り時の Pause 保留・末尾欠落防止試験（Issue #23 / #24）

[Issue #22](https://github.com/yshdsnd/mc-hires-bridge/issues/22) の調査結果に基づき、[Issue #23](https://github.com/yshdsnd/mc-hires-bridge/issues/23) および [Issue #24](https://github.com/yshdsnd/mc-hires-bridge/issues/24) にて実装・強化した試験版（`flac24_pause_experiment.dll`）の設計、手動操作・終端状態遷移、検証結果、および Music Center 実機での動作確認手順をまとめる。

本試験ターゲットは、実再生での総合検証が完了するまで通常の配布パッケージ（`cmake --install` およびリリース ZIP）からは除外される。

---

## 1. 曲末の早期停止で観測した挙動

Music Center for PC 2.7.4 x86 では、48 kHz/24 bit → 48 kHz/16 bit FLAC の自動曲送りで、23秒の前曲が約20.1秒で途切れた。#22 の診断では、通知3841、Pause、DirectSound停止、位置変更の順を観測した。#23 の24 bit観測モードでも同じ停止順を確認した。

調査時の仮説は、先読み後の早期Pauseと後続の位置変更が、旧経路に残る音声の出力を中断するというもの。#23 の保留モードでは、前曲は23秒の末尾まで出力され、約160 ms後に次曲が始まった。

---


## 2. 試験版アーキテクチャと機能

本試験版（`flac24_pause_experiment.dll`）は、24bit FLAC 再生能力を完全に維持したまま、環境変数による動作モード切り替えと安全な Pause 保留制御を提供する。

### モード切り替え（`MC_BRIDGE_PAUSE_MODE`）

| モード名 | 設定値 | 挙動 |
| :--- | :--- | :--- |
| **観察モード** | `MC_BRIDGE_PAUSE_MODE=observe` | 24bit FLAC 出力を維持し、全イベント（`PAUSE`, `STOP`, `SEEK_SETPOSITIONS`, `DS_EVENT_3841`, `DOWNSTREAM_PCM`, `DOWNSTREAM_EOS`, `DS_STOP`）をログ出力するが、Pause 保留および合成 EOS 送出は行わない。 |
| **実験モード**（デフォルト） | `MC_BRIDGE_PAUSE_MODE=defer` または未設定 | 形式変更曲送り時の `OpcDSPlay.dll+0xDAC9` からの `Pause()` を保留し、EQ への EOS 送出と DirectSound ドレインを待機して末尾欠落を完全に解消する。 |

### 制御シーケンス（実験モード）

```
[上流デコーダ]      [OmgEqxlFilter (EQ)]      [DirectSound]          [OpcDSPlay.dll]
      |                      |                      |                       |
      |-- (最終PCM出力) ---->|                      |                       |
      |-- EndOfStream ------>| (内部保持)           |                       |
      |                      |                      |                       |
   (イベント 3841 発火) -------------------------------------------------->|
      |                      |                      |                       |
      |                      |                      |   [RVA 0xDAC9]        |
      |                      |                      |   IMediaControl::Pause()
      |                      |                      |           |           |
      |               === フックが Pause をインターセプト ===   |           |
      |                      |                      |           |           |
      |                      |<- IPin::EndOfStream() (1回送出)  |           |
      |                      |-- (保持PCMフラッシュ)-->|           |           |
      |                      |                      | (PCM再生) |           |
      |                      |                      |           |           |
      |          === メッセージポンプ待機 (curPos >= stopPos) ==|           |
      |                      |                      |           |           |
      |                      |                      | (全PCM完了)|          |
      |                      |                      |<-- DS_STOP|           |
      |                      |                      |           |           |
      |               === 保留解除: originalControlPause() 実行 ===         |
      |                      |                      |           |           |
      |                      |                      |           |<-- S_OK --|
      |                      |                      |                       |
      |                      |                      |   IMediaSeeking::     |
      |                      |                      |   SetPositions()      |
      |                      |                      |   (Flush & 次曲準備)  |
```

---

## 3. 保留中の手動操作・終端・再入状態遷移（Issue #24 強化）

保留待機中（約 2.9 秒のドレイン区間）にメッセージポンプを経由してユーザー操作やウィンドウイベントが発生した場合、状態機械（`eosTransition::Tracker`）とフック群が連携して直ちに安全な状態遷移を行います。

### 終了・解除ステータス（`PauseDeferralStatus`）

| ステータス名 | 値 | 発生契機 | 保留待機ループの挙動 | グラフ状態への作用 |
| :--- | :--- | :--- | :--- | :--- |
| `CompletedDrain` | 2 | 全 PCM が DirectSound から出力完了（`curPos >= stopPos`） | 正常脱出 | 保留解除、本来の `Pause()` を実行し、次曲の Flush/SetPositions へ引き渡し |
| `CancelledStop` | 3 | 待機中にユーザーが停止ボタン押下、または `Stop` 発行 | 即座に待機脱出 | 保留破棄、多重保留を行わず即座に `originalControlStop` を実行して停止 |
| `CancelledSeek` | 4 | 待機中に再生バーで位置変更（シーク）操作 | 即座に待機脱出 | 保留破棄、`originalControlPause` をバイパスしてシーク先からの再生を継続 |
| `CancelledTrackChange` | 5 | 待機中に次曲/前曲スキップボタン押下（手動曲送り） | 即座に待機脱出 | 保留破棄、即座に次曲の Open/Play 処理へ移行 |
| `CancelledManualPause` | 6 | 待機中に手動で一時停止ボタン押下（外部 caller からの Pause） | 即座に待機脱出 | 保留破棄、即座に `originalControlPause` を実行して一時停止状態へ移行 |
| `CancelledQuit` | 7 | `WM_QUIT` 受信（ウィンドウ終了・アプリ終了） | 即座に待機脱出 | `PostQuitMessage` を再送出し、UI メインループへ終了通知 |
| `TimedOut` | 8 | DirectSound ストール等により上限時間（最大 4.5 秒）超過 | 安全脱出 | 安全上限でループを抜け、`originalControlPause` を実行してハングを防止 |

### 再入防止と重複 EOS 抑止
1. **多重保留の抑止**:
   Pause 保留中に Stop や Seek が発生した場合、二重に停止保留（`EOS_STOP_DEFERRED_START`）が走るのを完全に防止。`isPauseDeferralActive()` がアクティブな場合は直ちに解除シグナルを発行して通常処理にフォールバックします。
2. **古いグラフへの EOS / Pause 再送防止**:
   保留完了後（`formatChangePauseCompleted == true`）、同一グラフに対して再度 `Pause` や `Stop`、`EndOfStream` が呼ばれても、`isFormatChangePending == false` および `eosDelivered == true` により一切の合成 EOS や保留が再発動しません。
3. **異なるサンプルレートへの遷移（`96kHz/24bit -> 44.1kHz/16bit` 等）**:
   サンプリングレートが変化する場合、バイト数や時刻の計算が対象レート（96kHz: 1サンプル6バイト）に基づいて正確に算出され、曲末尾まで 100% の PCM が出力された後に次曲へ遷移します。
4. **24bit プレイリスト終端（次曲なし）**:
   次曲が存在しない場合、Pause 保留は起動せず、最終 PCM 受信時に EQ へ EOS を 1 回送出。EQ 内部保持フレームが DirectSound にドレインされ、DirectShow の `EC_COMPLETE` によって正常終了します。

---

## 4. ヘッドレス検証（`eos_transition_tests` 全 18 項目）

本実装に対する全 18 項目の単体・結合テストを実装し、Win32 Release ビルドで全件合格を確認済み。

### ビルドとテスト実行手順
```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_EXPERIMENTS=ON
& $cmake --build build-Win32 --config Release --target eos_transition_tests flac24_pause_experiment
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
- **Test 14（Issue #24）: 保留中の手動操作（Stop, Seek, TrackChange, ManualPause, Exit）状態遷移検証**:
  - 保留待機中の手動停止で即座に待機脱出・`CancelledStop` 記録・二重保留抑止
  - 保留待機中のシークで即座に待機脱出・`CancelledSeek` 記録・Pause バイパス
  - 保留待機中の手動曲送りで即座に待機脱出・`CancelledTrackChange` 記録
  - 保留待機中の手動一時停止で即座に待機脱出・`CancelledManualPause` 記録
  - 保留待機中の `WM_QUIT` 受信で即座に待機脱出・`CancelledQuit` 記録・終了通知再送
- **Test 15（Issue #24）: ドレイン待ちタイムアウト安全機構検証**:
  - 位置停止・ストール時にも最大 4.5 秒の上限で `TimedOut` 脱出し、ハングを回避
- **Test 16（Issue #24）: 再入防止および古いグラフへの重複 EOS 抑止検証**:
  - 保留完了後の同一グラフに対する追加 EOS / Pause / Stop が重複実行されないことを確認
- **Test 17（Issue #24）: 異なるサンプルレート遷移（`96kHz/24bit -> 44.1kHz/16bit` & `96kHz/24bit -> 48kHz/24bit`）**:
  - 96kHz 24bit 音源から異レートへの遷移時、末尾欠落 0ms（`tailComplete == true`, `missingTailFrames == 0`）を達成
- **Test 18（Issue #24）: 24bit プレイリスト終端（異レート・次曲なし）検証**:
  - 96kHz 24bit プレイリスト最終曲が EOS 1 回送出を経て正常ドレイン・停止することを検証

---

## 5. Music Center 実機での動作確認手順（Codex 用）

実機検証を行う際は、以下の手順に従って保留区間（終端前約 2.9 秒）を狙った手動操作とログ採取を実施してください。

### 1. 試験 DLL の配置
ビルドされた `build-Win32\Release\flac24_pause_experiment.dll` を Music Center の実行環境（またはランチャー環境）へ配置する。

### 2. 異レート自動曲送りの検証（`48kHz/24bit -> 48kHz/16bit`、`96kHz/24bit -> 44.1kHz/16bit`）
```powershell
$env:MC_BRIDGE_PAUSE_MODE = "defer"
$env:MC_BRIDGE_LOG_PATH = "C:\path\to\transition.log"
# Music Center を起動し、異形式曲送りを再生
```
**確認ポイント**:
- `PAUSE_DEFERRED_START` が約 20.1 秒時点で記録されること。
- `EOS_DELIVERED_DEFERRED_PAUSE` により EQ から残余フレームが出力されること。
- `PAUSE_DEFERRED_RELEASE` でステータス `2`（`CompletedDrain`）が記録されること。
- WASAPI ループバック等で前曲が末尾まで聴こえ、次曲がスムーズに開始すること。

### 3. 保留区間（末尾約 2.9 秒前）を狙った手動操作の検証

| 操作 | タイミング | 期待されるログ出力 | 期待される本体の挙動 |
| :--- | :--- | :--- | :--- |
| **手動停止** | 20.5 秒〜22.5 秒の間に「停止」ボタンを押す | `PAUSE_DEFERRED_ABORT_BY_STOP`<br>`PAUSE_DEFERRED_RELEASE (status=3: CancelledStop)` | 待たされずに即座に再生が停止すること。 |
| **手動シーク** | 20.5 秒〜22.5 秒の間に曲の中盤（例: 10 秒）をクリック | `PAUSE_DEFERRED_ABORT_BY_SEEK`<br>`PAUSE_DEFERRED_RELEASE (status=4: CancelledSeek)`<br>`CONTROL_PAUSE_BYPASS_CANCEL` | Pause せずに即座にシーク先から再生が継続すること。 |
| **手動曲送り** | 20.5 秒〜22.5 秒の間に「次へ」ボタンを押す | `PAUSE_DEFERRED_ABORT_BY_TRACK_CHANGE`<br>`PAUSE_DEFERRED_RELEASE (status=5: CancelledTrackChange)` | 待たされずに即座に次の曲が再生開始すること。 |
| **手動一時停止** | 20.5 秒〜22.5 秒の間に「一時停止」ボタンを押す | `PAUSE_DEFERRED_ABORT_BY_MANUAL_PAUSE`<br>`PAUSE_DEFERRED_RELEASE (status=6: CancelledManualPause)` | 即座に一時停止状態になり、再生ボタンで再開できること。 |
| **アプリ終了** | 20.5 秒〜22.5 秒の間にウィンドウの「×」ボタンを押す | `PAUSE_DEFERRED_RELEASE (status=7: CancelledQuit)` | アプリがハングせずスムーズに終了すること。 |

### 4. 24bit プレイリスト最終曲の停止検証
- 24bit 音源をプレイリストの最後の曲として配置し、末尾まで自然再生。
- 終端でノイズや末尾欠落なく再生が完了し、本体が「停止」状態になることを確認。
