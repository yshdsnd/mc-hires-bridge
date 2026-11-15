# 非ブロッキング仮想 Pause 試作実装および検証仕様書（Issue #33 / #34 / #35）

## 1. 概要とアーキテクチャ

### 1.1 背景
[Issue #28](https://github.com/yshdsnd/mc-hires-bridge/issues/28) および [Issue #29](https://github.com/yshdsnd/mc-hires-bridge/issues/29)（`docs/pause-deferral-design.md`）の分析により、異形式自動曲送り（例: 24bit/96kHz → 16bit/44.1kHz、または 24bit/48kHz → 16bit/48kHz）時に手動操作が最大約 2.7 秒間ブロックされる問題の原因が判明しました。
再生エンジン（`OpcDSPlay.dll`）は内部排他制御（CriticalSection）を保持したまま DirectShow の `IMediaControl::Pause()` / `IMediaFilter::Pause()` を呼び出します。従前の同期型待機実装では、このフック内部で前曲の PCM 出力完了を `Sleep` ループで同期待機していたため、エンジン排他制御が 2.7 秒間拘束され、UI スレッドからの手動操作（Pause, Seek, Stop, TrackChange）がブロックされていました。

### 1.2 非ブロッキング仮想 Pause（Virtual Pause）のアーキテクチャ
Issue #33、#34、および #35 では、この排他制御拘束を解消するため「非ブロッキング仮想 Pause（Virtual Pause）」方式を採用・改善実装しました（`experiments/eos_transition/` および `flac24_async_experiment`）。

```
[UI スレッド / ユーザー手動操作]
       |
       | < 1 ms で到達可能（排他制御フリー）
       v
[再生エンジン (OpcDSPlay.dll)]
       |
       | 1. 自動曲送り時 Pause 呼出 (RVA 0xDAC9)
       v
[IMediaControl::Pause / IMediaFilter::Pause フック]
       |-- 遷移コンテキスト（transitionId, generation, enterTick）を生成
       |-- 仮想 Pause 状態（Virtual Pause）を設定
       |-- 即座に S_OK を返却 (< 2 ms)
       v
[再生エンジン (OpcDSPlay.dll)]
       |-- 即座に内部排他制御 (CriticalSection) を解放！
       |
       | 2. 後続の自動制御呼出 (遷移世代 transitionGeneration に基づく照合)
       |---> IDirectSoundBuffer::Stop() --------> [仮想Pause中の自動停止を抑止 (DS_OK 返却、再生継続)]
       |---> IMediaSeeking::SetPositions() -----> [内部シーク (seekPos=0, 世代内初回) を退避・保留]
       |                                          (ユーザー手動シークは世代判定・入力検知で即時優先実行)
       |
       +=======================================================+
       | バックグラウンド監視スレッド (asyncDrainWorkerThread) |
       | COM インターフェースはストリームマーシャリング経由で安全アクセス |
       | スレッド作成失敗時も呼出元をブロックせず即時安全終了  |
       | 前曲の残り PCM を 2 段階で監視                        |
       |  ・Stage 1: 下流 PCM 到達 (targetBytes到達 + EOS送出) |
       |  ・Stage 2: レンダラー最終出力完了 (デバイス再生完了) |
       +=======================================================+
                       |
        +--------------+--------------+
        |                             |
  [自然完了ルート]              [手動割り込みルート]
        |                             |
  Stage 1 (PCM到達)             ユーザー操作 (Pause/Seek/Stop等) 到着
        +                             |
  Stage 2 (レンダラー出力完了)  Virtual Pause 即時取消 (< 1 ms)
        |                             |
  有効な遷移を 1 度だけ完了     保留 SetPositions を破棄 (PURGE)
  保留 SetPositions を自動実行  ユーザー操作を最優先実行
  (PAUSE_DEFERRED_COMPLETED)    遅延 EOS は破棄 (stale_eos == 0)
                                (PAUSE_DEFERRED_CANCELLED)
```

### 1.3 状態遷移と構成要素（Issue #35 改善点を含む）
- **自動曲送り状態と世代管理（`transitionGeneration`）による内部シーク判定**:
  - Issue #34 実測において、仮想 Pause 突入の約 6ms 後に別スレッドから発行された内部シーク（`SetPositions(0)`）が手動シークと誤判定されて保留がキャンセルされ、曲末が約 2.8 秒早く打ち切られる回帰が発生しました。
  - Issue #35 では、固定的なスレッド ID や呼出元アドレスによる判定を廃止し、**グラフの世代（`transitionGeneration`）**、**自動曲送り状態（`autoTrackChangeInProgress`）**、および **ユーザー入力イベント（`g_lastUserInputTick`）** を統合した判定ロジック（`classifySeekOperation`）を導入しました。
  - 当該世代でユーザー入力がなく、最初に発行された内部初期化シーク（`seekPos == 0`）を自動操作として安全に保留（`AutomatedDeferred`）します。一方、ユーザー入力（マウス・キー・シークバー操作）を伴うシークや位置ジャンプ（`seekPos > 0`）は即座に手動操作（`ManualCancelled`）として識別され、同一呼び出し元からの手動シークも誤分類しません。
- **DirectSound レンダラー停止の確実な抑止（残存音声排出の維持）**:
  - 仮想 Pause 中の自動 `IDirectSoundBuffer::Stop()` 呼出をフック層で確実に抑止（`DS_OK` 返却）し、ハードウェア/DirectSound リングバッファ内の残存 PCM（約 2.8 秒）の再生を継続させます。
  - ユーザーが明示的に停止を操作した場合（`IMediaControl::Stop()` または入力イベント直後の停止）のみ手動停止として Virtual Pause をキャンセルし、直ちに DirectSound を停止させます。
- **2段階ドレイン完了判定（PCM 到達 vs レンダラー最終出力完了）**:
  - **Stage 1（下流への PCM 到達 + 下流 EOS 送出）** と **Stage 2（DirectSound レンダラーの最終出力完了）** の双方が満たされて初めて `PAUSE_DEFERRED_COMPLETED` とし、保留されていた `SetPositions` を 1 度だけ安全に適用します。
  - 完了直前の手動操作取消競合に対しても排他的保護を維持します。

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
3. `DS_STOP_SUPPRESSED_FOR_VIRTUAL_PAUSE` (0, 0, hr=DS_OK): エンジンからの自動 DirectSound 停止呼出が抑止されたことを記録。
4. `DEFERRED_SETPOSITIONS_QUEUED` (newPos, flags, hr=S_OK): 当該世代の内部自動 `SetPositions` が安全に退避・保留されたことを記録。
5. `EOS_DOWNSTREAM_DELIVER` (rate, bits, hr=S_OK): 前曲ドレイン完了（Stage 1）に伴い下流 EOS を正常送出したことを記録。
6. `PAUSE_DEFERRED_COMPLETED` (status=CompletedDrain, hr=S_OK): Stage 1（PCM到達）および Stage 2（レンダラー最終出力）が完了。
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

## 3. ヘッドレス競合テストスイート（Tests 40〜57）検証結果

Issue #30、#33、#34 の競合テストハーネス（`AsyncControlHarness`）を再利用・拡張し、仮想 Pause の非同期特性、48kHz 実測回帰（Issue #35）、世代・状態判定、および手動操作誤分類防止を検証する Tests 40〜57 を実装・全件合格を確認しました。

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
| **TEST 51** | Automated SetPositions caller matching & unrelated buffer isolation | 自動 Pause と異なる RVA からの後続 `SetPositions` を同一スレッド・遷移コンテキストで自動と照合して保留。無関係な別バッファの `Stop()` を通常処理へ素通しすることを確認 | **PASS** |
| **TEST 52** | Two-stage drain verification (PCM delivery vs renderer output) | 下流への PCM 到達（Stage 1）だけでは完了とせず、レンダラー出力完了（Stage 2）を経て初めて `COMPLETED` となることを確認 | **PASS** |
| **TEST 53** | Pre-completion cancellation race prevention | PCM 到達・EOS 送出完了後、レンダラー最終出力直前に手動操作が到着した場合、取消が確実に優先され、保留操作がパージされ遅延完了処理が古い操作を実行しないことを確認 | **PASS** |
| **TEST 54** | Monitor thread creation failure non-blocking safety | スレッド作成失敗時にも呼出元スレッドが曲末までブロックされず、即座に（< 2 ms）安全復帰することを確認 | **PASS** |
| **TEST 55** | 48kHz auto track change event sequence regression (Issue #35) | 仮想 Pause 直後の DirectSound `Stop()` 抑止、約 6ms 後の内部 `SetPositions(0)` 保留、および残存 PCM（約 2.8 秒）完全排出を経て次曲へ遷移することを確認 | **PASS** |
| **TEST 56** | Manual seek not misclassified from same caller | 仮想 Pause 保留中に同一呼び出し元から手動シーク（位置 > 0 またはユーザー入力後）が発生した場合、手動シークと判定して即時反映することを確認 | **PASS** |
| **TEST 57** | Manual stop regression during Virtual Pause | 仮想 Pause 保留中に手動停止が発行された場合、直ちに仮想 Pause を取消してグラフおよび DirectSound を完全停止することを確認 | **PASS** |

> 合計 57 件（基本テスト 1〜28、Issue #30 競合テスト 29〜39、Issue #33 / #34 / #35 非同期テスト 40〜57）すべてがエラーなしで通過しました。

---

## 4. Music Center for PC 2.7.4 x86 実機検証プロトコル（Codex 引き渡し用）

Codex が Music Center for PC 2.7.4 x86 実機環境で動作検証を行うための手順および確認事項です。

### 4.1 試作バイナリのビルドと配置
1. **ビルドターゲット**: `flac24_async_experiment`
   ```powershell
   cmake --build build-Win32 --config Release --target flac24_async_experiment
   ```
   生成される DLL: `build-Win32/Release/flac24_async_experiment.dll`
2. **配置手順**:
   Music Center for PC 2.7.4 インストールディレクトリ（`C:\Program Files (x86)\Sony\Music Center\` 等）内の配置対象 DLL（通常はブリッジ DLL）として本試作バイナリを配置します。

### 4.2 環境変数の設定
- `MC_BRIDGE_PAUSE_MODE=async`（デフォルトで有効ですが、明示指定を推奨）
- `MC_BRIDGE_DIAGNOSTICS=1`（高精度タイムスタンプ付き診断ログの出力）

### 4.3 実機検証シナリオと確認ポイント

#### シナリオ A: 48kHz 自動曲送りの末尾欠落回帰解消確認（参照 PCM と共有ループバック照合）
- **対象**: 48 kHz/24 bit FLAC から 48 kHz/16 bit FLAC への自動曲送り（ユーザー操作なし）。
- **確認事項**:
  - 先行曲（約 23 秒）の PCM が約 20.2 秒で打ち切られず、末尾まで欠落なく再生されること。
  - レンダラー出力完了と同時に次曲が 1 度だけ開始されること。
  - ログに `DS_STOP_SUPPRESSED_FOR_VIRTUAL_PAUSE` および `DEFERRED_SETPOSITIONS_QUEUED` が記録され、`PAUSE_DEFERRED_COMPLETED` で自然終了すること。

#### シナリオ B: 保留区間中の手動一時停止（Manual Pause）の即時反映
- **対象**: 先行曲末尾約 2 秒前の仮想 Pause 保留区間で、一時停止ボタンをクリック。
- **確認事項**:
  - クリック直後（< 50 ms）に音声が一時停止し、UI 応答遅延（2.7 秒間の排他ロック拘束）が発生しないこと。
  - ログに `PAUSE_DEFERRED_CANCELLED (CancelledManualPause)` および `DEFERRED_SETPOSITIONS_PURGED` が記録されること。

#### シナリオ C: 保留区間中の手動シーク（Manual Seek）の即時反映
- **対象**: 保留区間でシークバーをクリックまたはドラッグ。
- **確認事項**:
  - 内部シークと誤分類されることなく、ユーザー指定位置へ即座にジャンプすること。
  - ログに `PAUSE_DEFERRED_CANCELLED (CancelledSeek)` が記録されること。

#### シナリオ D: 保留区間中の手動停止（Manual Stop）の即時反映
- **対象**: 保留区間で停止ボタンをクリック。
- **確認事項**:
  - 直ちに音声出力が停止し、停止状態へ遷移すること。
  - ログに `PAUSE_DEFERRED_CANCELLED (CancelledStop)` が記録されること。
