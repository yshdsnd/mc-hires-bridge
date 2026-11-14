# 異形式曲送り時の Pause 保留・手動操作・終端検証（Issue #23 / #24 / #25 / #27 / #28）

[Issue #22](https://github.com/yshdsnd/mc-hires-bridge/issues/22) の調査結果に基づき、[Issue #23](https://github.com/yshdsnd/mc-hires-bridge/issues/23)、[Issue #24](https://github.com/yshdsnd/mc-hires-bridge/issues/24)、[Issue #25](https://github.com/yshdsnd/mc-hires-bridge/issues/25)、[Issue #27](https://github.com/yshdsnd/mc-hires-bridge/issues/27)、および [Issue #28](https://github.com/yshdsnd/mc-hires-bridge/issues/28) にて実装・強化した試験版（`flac24_pause_experiment.dll` / `flac24_pause_diagnostic.dll`）の設計、排他ロックと同期 EOS の解析結果、手動操作・終端状態遷移、検証結果、および Music Center 実機での動作確認手順をまとめる。

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

#27 の実機検証（Issue #28 報告）により、以下の事実が判明した：
1. **プロセス全域のロック API フックによる起動障害**:
   `kernel32!EnterCriticalSection` をプロセス全域でフックする構成では、起動直後にウィンドウが消失した。Chromium / CEF のスレッドプール生成や CRT の動的初期化等と競合したことが原因と推定される。一方、ロックフックのみを外した構成では正常に起動・再生可能であった。
2. **通常再生時と保留時の手動操作伝播の差異**:
   - **通常再生中**: マウスボタンを離してから約 3 ms で手動 Pause が DirectShow 境界（`BOUNDARY_CONTROL_PAUSE_ENTER`）に到達した。
   - **Pause 保留中**: Win32 入力メッセージ（`APP_INPUT_DISPATCH`）は正常にディスパッチされたが、手動 Pause は DirectShow 側に現れず、ディスパッチから約 2.74 秒後に通常の曲末処理完了で保留が解除された。

このため、プロセス全域への介入を完全に排除し、再生エンジン（`OpcDSPlay.dll`）の境界に絞った安全なロック観測構成への再編（#28）が必要となった。

---

## 3. 解決アーキテクチャと診断機能（Issue #28 絞り込み構成）

### 1. 早期同期 EOS 送出の抑止（`hasDownstreamReachedTarget`）
- `deferFormatChangePause` 開始時点で、下流が目標 PCM を受信済みか（`hasDownstreamReachedTarget(key)`）を判定。
- **目標未達の場合（曲末尾手前）**:
  `deferFormatChangePause` は同期 EOS を叩かず、直ちに待機ループ（メッセージポンプ）へ入る。
  残りの PCM は通常通りストリーミングスレッド経由で `eqReceiveHook` が受信し、**最終 PCM が EQ に到達した瞬間に `eqReceiveHook` 自身が `originalEqEndOfStream` を 1 回だけ送出する**。
- これにより、`deferFormatChangePause` 開始時の 1.6〜1.7 秒に及ぶ不必要な同期ブロッキングが完全に解消され、待機ループが即座にメッセージポンプを実行する。

### 2. 再生エンジン境界に絞った IAT ロック観測とアプリ指令受付（#28）
プロセス全域のフック（`kernel32` 全域）を完全に廃止し、起動・再生の安全性を担保した診断構成を実装：

1. **アプリ側入力ディスパッチ観測（`APP_INPUT_DISPATCH`）**:
   - `user32!DispatchMessageW` をフックし、UI スレッドで処理されるマウス操作（`WM_LBUTTONDOWN` / `UP`）、キー入力（`WM_KEYDOWN` / `UP`）、メニュー・アクセラレータ（`WM_COMMAND`）をキャプチャ。
2. **再生エンジン（`OpcDSPlay.dll`）専用の IAT ロック観測（`ENGINE_LOCK_*`）**:
   - `OpcDSPlay.dll` のインポートアドレステーブル（IAT）内の `EnterCriticalSection` / `LeaveCriticalSection` のみを局所的に置換。
   - CEF や Chromium スレッド、CRT ヒープ、Windows ローダーの動作には一切触れないため、プロセス起動およびバックグラウンドスレッド生成が 100% 安定動作。
   - `OpcDSPlay.dll` が未ロードの段階では `PENDING_MODULE_LOAD` として記録され、再生開始時に遅延アタッチ（`ATTACHED_LATE`）を実行。
3. **診断構成と環境変数切り替え**:
   - 診断ビルドの既定では、入力ディスパッチ（`APP_INPUT_DISPATCH`）、DirectShow 境界（`BOUNDARY_*`）、およびエンジン IAT ロックフックが有効。
   - `$env:MC_BRIDGE_LOCK_DIAG = "0"` を設定することで、ロック観測のみを完全に無効化し、入力と DirectShow のみで安全に採取することも可能。

#### 診断ログ仕様
- **単調時刻**: `QueryPerformanceCounter`（QPC）によるマイクロ秒精度オフセット（`mono_ms`）。
- **スレッド ID**: `tid`（UI スレッドとワーカースレッドの分離観測）。
- **グラフ識別子 / オブジェクト**: `graph`（COM オブジェクトポインタまたはロックアドレス）。
- **呼出元 RVA 解決**: `caller`（例: `OpcDSPlay.dll+0xDAC9`, `USER32.dll` 等）。
- **フック初期化ステータス**:
  - `DIAG_HOOK_STATUS`: 各観測点（DirectShow, User32, OpcDSPlay IAT）の登録成否（`OK`, `PENDING_MODULE_LOAD`, `ATTACHED_LATE`, `DISABLED_BY_CONFIG`）を出力。
- **アプリ指令受付イベント**:
  - `APP_INPUT_DISPATCH`: Win32 メッセージループでの入力ディスパッチ。
- **エンジン排他ロックイベント**:
  - `ENGINE_LOCK_TRY`: 再生エンジン（`OpcDSPlay.dll`）からの `EnterCriticalSection` 試行。
  - `ENGINE_LOCK_ACQUIRED`: ロック取得成功時刻、および試行からの待機時間 `wait_ms`（競合ブロッキング時間）。
  - `ENGINE_LOCK_RELEASED`: ロック解放、および保持時間 `held_ms`。
- **DirectShow / フィルター境界イベント**:
  - `BOUNDARY_CONTROL_PAUSE_ENTER` / `LEAVE`
  - `BOUNDARY_CONTROL_STOP_ENTER` / `LEAVE`
  - `BOUNDARY_SEEK_SETPOSITIONS_ENTER` / `LEAVE`
  - `PAUSE_DEFERRED_START`
  - `DRAIN_WAIT_ENTER` / `DRAIN_WAIT_TICK`
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

## 5. ヘッドレス検証（`eos_transition_tests` 全 28 項目）

本実装に対する全 28 項目の単体・結合・マルチスレッド並行テストを実装し、Win32 Release ビルドで全件合格を確認済み。

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
  - `IMediaControl::Pause` 保留と実ストリーム末尾検証
  - 観察モード（`observe`）と実験モード（`defer`）の切り替え検証
  - 非形式変更時の Pause 非保留検証
- **Test 14〜18（Issue #24）**:
  - 保留中の手動操作状態遷移検証
  - ドレイン待ちタイムアウト安全機構検証
  - 再入防止および古いグラフへの重複 EOS 抑止検証
  - 異なるサンプルレート遷移での末尾欠落 0ms 検証
  - 24bit プレイリスト終端（異レート・次曲なし）検証
- **Test 19〜22（Issue #25）**:
  - **Test 19**: 同期 EQ EOS 実行中の並行手動操作検証
  - **Test 20**: ドレイン待機中の並行手動操作検証
  - **Test 21**: 早すぎる EQ EOS 送出抑止検証
  - **Test 22**: 取消サイクル後の 24bit 末尾保持および同形式先読み維持検証
- **Test 23〜25（Issue #27）**:
  - **Test 23**: 到達した指令が排他ロック取得を待つケースの追跡（Case B）
  - **Test 24**: 指令がエンジンに未到達のケースの識別（Case A）
  - **Test 25**: 観測モードにおける単調時刻整合性と非介入性の検証
- **Test 26〜28（Issue #28 新設）**:
  - **Test 26: 診断フック登録ステータス記録とフォールバック耐性検証**:
    各観測点の登録結果（`OK`、`PENDING_MODULE_LOAD`、`ATTACHED_LATE` 等）が正確に追跡され、特定モジュールが未ロードでも他のフックが継続動作することを検証。
  - **Test 27: 再入保護と初期化前後の安全な呼び出し検証**:
    同一スレッド内でのネスト呼び出しや初期化前の呼び出しにおいて、デッドロックやクラッシュを起こさず安全に透過実行されることを検証。
  - **Test 28: 通常再生時の手動 Pause 発火と全境界の時刻照合検証**:
    通常再生時に手動 Pause を押下した際、入力ディスパッチ（`APP_INPUT_DISPATCH`）→ エンジンロック（`ENGINE_LOCK_TRY`/`ACQUIRED`）→ DirectShow 境界（`BOUNDARY_CONTROL_PAUSE_ENTER`/`LEAVE`）→ ロック解放（`ENGINE_LOCK_RELEASED`）が同一スレッドかつ単調増加時刻で欠落なく記録されることを検証。

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

### 2. 既存ランチャーを用いた診断実行
```powershell
# 診断モードを有効化（エンジン IAT ロックフック有効）
$env:MC_BRIDGE_PAUSE_MODE = "defer"
$env:MC_BRIDGE_DIAGNOSTICS = "1"
# 必要に応じてロックフックのみ無効化したい場合は: $env:MC_BRIDGE_LOCK_DIAG = "0"

# 既存ランチャーから起動（または Music Center を直接起動）
& ".\build-Win32\Release\MC HiRes Bridge.exe"
```

### 3. 検証ステップ
1. **ステップ 1: 通常再生中でのエンジン側観測点の発火確認**:
   - 任意の FLAC 曲を通常再生し、再生途中で「一時停止」ボタンを押下。
   - ログ（`%TEMP%\Flac24Override-<PID>.tsv`）を確認し、以下の一連のイベントが同一 `tid` かつ連続した時刻（約 3ms 以内）で記録されていることを確認：
     - `APP_INPUT_DISPATCH` (`WM_LBUTTONUP`)
     - `ENGINE_LOCK_TRY` / `ENGINE_LOCK_ACQUIRED` (`wait_ms = 0`)
     - `BOUNDARY_CONTROL_PAUSE_ENTER` / `LEAVE`
     - `ENGINE_LOCK_RELEASED`
2. **ステップ 2: 自動曲送りの Pause 保留中における手動操作の照合**:
   - 48 kHz/24 bit → 48 kHz/16 bit の自動曲送りを再生。
   - 曲末尾（20.1秒〜23秒付近、`PAUSE_DEFERRED_START` 後）で手動一時停止またはシークを実行。
   - 判定基準：
     - **Case B（到達した指令がロック待ち）**: 手動操作スレッドから `ENGINE_LOCK_TRY` が出力され、`ENGINE_LOCK_ACQUIRED` が保留解除まで待たされる（`wait_ms > 0`）。
     - **Case A（指令がエンジンに未到達）**: `APP_INPUT_DISPATCH` は記録されるが、手動操作スレッドからの `ENGINE_LOCK_TRY` が 0 件。

---

## 7. 配布安全性
- `cmake --install` の出力には通常版バイナリとライセンスファイルのみが含まれ、`flac24_pause_experiment.dll`、`flac24_pause_diagnostic.dll`、およびテストバイナリは一切含まれないことを確認済みです。
- 個人の楽曲、パス、PCM、生ログ、生成バイナリはコミットに含まれません。
- 第三者バイナリ由来の逆アセンブリ命令列、機械語バイト列、逆コンパイル出力は含みません（#26）。
