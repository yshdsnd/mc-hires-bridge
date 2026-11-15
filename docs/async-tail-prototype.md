# 非ブロッキング仮想 Pause 試作実装および検証仕様書（Issue #33）

## 1. 概要とアーキテクチャ

### 1.1 背景
[Issue #28](https://github.com/yshdsnd/mc-hires-bridge/issues/28) および [Issue #29](https://github.com/yshdsnd/mc-hires-bridge/issues/29)（`docs/pause-deferral-design.md`）の分析により、異形式自動曲送り（例: 24bit/96kHz → 16bit/44.1kHz）時に手動操作が最大約 2.7 秒間ブロックされる問題の原因が判明しました。
再生エンジン（`OpcDSPlay.dll`）は内部排他制御（CriticalSection）を保持したまま DirectShow の `IMediaControl::Pause()` / `IMediaFilter::Pause()` を呼び出します。従前の同期型待機実装では、このフック内部で前曲の PCM 出力完了を `Sleep` ループで同期待機していたため、エンジン排他制御が 2.7 秒間拘束され、UI スレッドからの手動操作（Pause, Seek, Stop, TrackChange）がブロックされていました。

### 1.2 非ブロッキング仮想 Pause（Virtual Pause）のアーキテクチャ
Issue #33 では、この排他制御拘束を解消するため「非ブロッキング仮想 Pause（Virtual Pause）」方式を採用・試作実装しました（`experiments/eos_transition/` および `flac24_async_experiment`）。

```
[UI スレッド / ユーザー手動操作]
       |
       | < 1 ms で到達可能（排他制御フリー）
       v
[再生エンジン (OpcDSPlay.dll)]
       |
       | 1. 自動曲送り時 Pause 呼出
       v
[IMediaControl::Pause / IMediaFilter::Pause フック]
       |-- 仮想 Pause 状態（Virtual Pause）を設定
       |-- 即座に S_OK を返却 (< 2 ms)
       v
[再生エンジン (OpcDSPlay.dll)]
       |-- 即座に内部排他制御 (CriticalSection) を解放！
       |
       | 2. 後続の自動制御呼出
       |---> IDirectSoundBuffer::Stop() --------> [フック層で抑止 (DS_OK 返却、再生継続)]
       |---> IMediaSeeking::SetPositions() -----> [フック層で自動 SetPositions を退避・保留]
       |
       +=========================================+
       | バックグラウンド監視 (asyncDrainWorker)  |
       | 前曲の残り PCM を自然ドレイン (約2.7秒)  |
       +=========================================+
                       |
        +--------------+--------------+
        |                             |
  [自然完了ルート]              [手動割り込みルート]
        |                             |
  前曲 PCM ドレイン完了         ユーザー操作 (Pause/Seek/Stop等) 到着
        |                             |
  下流 EOS を 1 回送出          Virtual Pause 即時取消 (< 1 ms)
        |                             |
  保留 SetPositions を自動実行  保留 SetPositions を破棄 (PURGE)
        |                             |
  次曲へ円滑遷移                ユーザー操作を最優先実行
  (PAUSE_DEFERRED_COMPLETED)    遅延 EOS は破棄 (stale_eos == 0)
                                (PAUSE_DEFERRED_CANCELLED)
```

### 1.3 状態遷移と構成要素
- **`virtualPauseActive` (原子フラグ)**: 仮想 Pause 状態が有効であることを示します。
- **`deferredSeek` (退避構造体)**: エンジンから自動発行された `SetPositions`（次曲の初期再生位置等）のパラメータを安全に保持します。
- **`dsStopHook` 抑止制御**: 仮想 Pause 中の自動 `IDirectSoundBuffer::Stop()` 呼出を抑止し、ハードウェア/DirectSound バッファの再生を中断させません。
- **`asyncDrainWorkerThread` (非同期監視スレッド)**:
  - 15 ms 間隔で前曲の出力進捗（DirectSound 再生カーソルおよびサンプル残量）を監視します。
  - 手動操作が割り込んだ場合は直ちに監視ループを抜け、キャンセル処理を行います。
  - 自然完了時は、下流フィルタ（イコライザ等）へ `EndOfStream` を 1 回だけ送出し、退避されていた `SetPositions` を DirectShow グラフへ適用します。

---

## 2. ログ仕様

本実装では、実行時の遷移結果および動作モードを明確に識別・追跡できる詳細ログを出力します。

### 2.1 動作モードログ
フック初期化時に設定されたモードが出力されます。
- `PAUSE_MODE_ASYNC`: 非ブロッキング仮想 Pause モード（デフォルト）
- `PAUSE_MODE_DEFER`: 従来の同期保留モード
- `PAUSE_MODE_OBSERVE`: 介入を行わない観測モード

### 2.2 自然完了時ログシーケンス
手動操作の割り込みがなく、前曲の曲末がすべて出力され次曲へ自動遷移した場合：
1. `PAUSE_ASYNC_VIRTUAL_ENTER` (rate, bits, hr=S_OK): 仮想 Pause に突入し、エンジンへ即座に `S_OK` を返却したことを記録。
2. `PAUSE_DEFERRED_PENDING` (remainingMs, 0, hr=S_OK): 非同期ドレイン監視が開始されたことを記録。
3. `DSOUND_STOP_SUPPRESSED` (0, 0, hr=DS_OK): エンジンからの自動 DirectSound 停止呼出が抑止されたことを記録。
4. `SEEK_SETPOSITIONS_DEFERRED` (flags, 0, hr=S_OK): エンジンからの自動 `SetPositions` が退避・保留されたことを記録。
5. `EOS_DOWNSTREAM_DELIVER` (rate, bits, hr=S_OK): 前曲ドレイン完了に伴い下流 EOS を正常送出したことを記録。
6. `PAUSE_DEFERRED_COMPLETED` (status=CompletedDrain, hr=S_OK): 自然ドレイン完了。
7. `DEFERRED_SETPOSITIONS_EXECUTED` (flags, 0, hr=S_OK): 保留されていた `SetPositions` が正常に適用されたことを記録。

### 2.3 手動割り込み時ログシーケンス
仮想 Pause 期間中にユーザーが操作（Pause, Seek, Stop, TrackChange, Exit）を行った場合：
1. `PAUSE_ASYNC_VIRTUAL_ENTER`: 仮想 Pause 突入。
2. `PAUSE_DEFERRED_PENDING`: 非同期監視中。
3. `PAUSE_DEFERRED_CANCELLED` (reason, elapsedMs, hr=S_OK): 手動操作を検知し仮想 Pause を即時取消（キャンセル理由は以下のいずれか）：
   - `CancelledManualPause`: ユーザー手動一時停止
   - `CancelledSeek`: ユーザー手動シーク
   - `CancelledStop`: ユーザー手動停止
   - `CancelledTrackChange`: ユーザー手動曲送り/戻し
   - `CancelledExit`: アプリケーション終了（WM_QUIT）
4. `DEFERRED_SETPOSITIONS_PURGED` (count, 0, hr=S_OK): 保留されていた自動曲送り用 `SetPositions` を破棄したことを記録。
5. ユーザー要求の DirectShow / DirectSound 操作（`BOUNDARY_CONTROL_PAUSE_ENTER`, `BOUNDARY_SEEK_SETPOSITIONS_ENTER` 等）が遅延なく実行。
6. 遅延して到着した前曲の EOS やコールバックは無効化され、次曲への誤送出（Stale EOS）は 0 件として記録。

---

## 3. ヘッドレス競合テストスイート（Tests 40〜50）検証結果

Issue #30 の競合テストハーネス（`AsyncControlHarness`）を拡張し、仮想 Pause の非同期特性と境界競合を検証する Tests 40〜50 を実装・全件合格を確認しました。

| テスト番号 | テスト名称 | 検証項目 | 結果 |
|:---:|:---|:---|:---:|
| **TEST 40** | Non-blocking Virtual Pause: Immediate S_OK | `IMediaControl::Pause` が即座に復帰し（実測 < 2 ms）、排他制御拘束時間を 0 にすることを確認 | **PASS** |
| **TEST 41** | Automatic SetPositions gating / deferral | 仮想 Pause 中の自動 `SetPositions` がグラフへ即時反映されず `deferredSeek` に退避されることを確認 | **PASS** |
| **TEST 42** | Natural drain completion | 手動操作がない場合、前曲 PCM が 100% ドレインされ、欠落 0 ms で `PAUSE_DEFERRED_COMPLETED` となり保留シークが実行されることを確認 | **PASS** |
| **TEST 43** | Manual Pause priority | 仮想 Pause 中の手動 Pause が < 1 ms で仮想 Pause を取り消し、保留操作をパージして手動 Pause を最優先実行することを確認 | **PASS** |
| **TEST 44** | Manual Seek priority | 手動シークが仮想 Pause を即時取り消し、自動保留シークをパージしてユーザー指定位置へシークされることを確認 | **PASS** |
| **TEST 45** | Manual Stop priority | 手動停止が仮想 Pause を即時取り消し、グラフおよび DirectSound を完全に停止させることを確認 | **PASS** |
| **TEST 46** | Manual TrackChange priority | 手動曲変更が仮想 Pause を即時取り消し、前曲グラフ状態を完全リセットすることを確認 | **PASS** |
| **TEST 47** | Application Exit priority | アプリケーション終了シグナルが仮想 Pause を安全かつ即時に中止し、リソースリークなく終了することを確認 | **PASS** |
| **TEST 48** | Stale EOS prevention | 手動操作でキャンセルされた後、遅延して到着した旧トラックの EOS が確実に破棄され、誤送出（Stale EOS）が 0 件であることを確認 | **PASS** |
| **TEST 49** | Same-format pre-reading continuity | 同一形式（24b/96k → 24b/96k 等）の先行読み込み時は仮想 Pause に入らず、ギャップレス再生が維持されることを確認 | **PASS** |
| **TEST 50** | Watchdog timeout safety | ハードウェアクロック停止等の異常時、安全タイムアウト上限（3.5 秒）で自動復帰しデッドロックを防止することを確認 | **PASS** |

> 合計 50 件（基本テスト 1〜28、Issue #30 競合テスト 29〜39、Issue #33 非同期テスト 40〜50）すべてがエラーなしで通過しました。

---

## 4. Music Center for PC 2.7.4 x86 実機検証プロトコル

Codex が実機環境で検証を行うための手順および確認事項です。

### 4.1 試作バイナリのビルドと配置
1. **ビルドターゲット**: `flac24_async_experiment`
   ```powershell
   cmake --build build-Win32 --config Release --target flac24_async_experiment
   ```
   生成される DLL: `build-Win32/Release/flac24_async_experiment.dll`
2. **Music Center への導入**:
   Music Center for PC 2.7.4 インストールディレクトリ（`C:\Program Files (x86)\Sony\Music Center\` 等）内の配置対象 DLL（通常はブリッジ DLL）として本試作バイナリを配置します。

### 4.2 環境変数の設定
- `MC_BRIDGE_PAUSE_MODE=async`（デフォルトで有効ですが、明示指定推奨）
- `MC_BRIDGE_DIAGNOSTICS=1`（高精度タイムスタンプ付き診断ログの出力）

### 4.3 検証シナリオと確認ポイント

#### シナリオ A: 自動曲送りの自然遷移（異形式）
- **操作**: 96 kHz/24 bit FLAC から 44.1 kHz/16 bit FLAC への自動曲送りを再生。手動操作は行わない。
- **期待結果**:
  - 曲末の約 2.7 秒間の残響・フェードアウトが一切途切れることなく再生される。
  - ドレイン完了と同時に次曲の再生がシームレスに開始される。
  - ログに `PAUSE_ASYNC_VIRTUAL_ENTER` → `PAUSE_DEFERRED_COMPLETED` → `DEFERRED_SETPOSITIONS_EXECUTED` が記録される。

#### シナリオ B: 曲末保留区間中の手動一時停止（Manual Pause）
- **操作**: 96 kHz/24 bit FLAC の曲末約 2 秒前（次曲先行読み込み〜Pause 発生区間）で、UI の一時停止ボタンをクリック。
- **期待結果**:
  - UI がフリーズせず、クリックとほぼ同時（< 20 ms 以内）に再生が一時停止する。
  - 2.7 秒間の無応答（ロック競合）が発生しない。
  - ログに `PAUSE_DEFERRED_CANCELLED (CancelledManualPause)` および `DEFERRED_SETPOSITIONS_PURGED` が記録される。

#### シナリオ C: 曲末保留区間中の手動シーク（Manual Seek）
- **操作**: シナリオ B と同様の曲末区間で、シークバーをクリックまたはドラッグ。
- **期待結果**:
  - 即座にシーク指定位置へ移動し、再生または待機状態になる。
  - ログに `PAUSE_DEFERRED_CANCELLED (CancelledSeek)` が記録される。

#### シナリオ D: 曲末保留区間中の手動曲送り・停止（Manual TrackChange / Stop）
- **操作**: 曲末区間で「次の曲」ボタンまたは「停止」ボタンをクリック。
- **期待結果**:
  - 即座に前曲の出力が停止し、指定曲の再生または停止状態に遷移する。
  - ログに `PAUSE_DEFERRED_CANCELLED` が記録される。

#### シナリオ E: 同一形式先行読み込み（ギャップレス確認）
- **操作**: 96 kHz/24 bit FLAC → 96 kHz/24 bit FLAC の連続再生。
- **期待結果**:
  - 仮想 Pause に入らず、音切れなく連続再生される。
