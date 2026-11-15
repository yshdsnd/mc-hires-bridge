# 非ブロッキング仮想 Pause 試作実装および検証仕様書（Issue #33 / #34 / #35 / #36 / #37 / #38 / #39 / #40 / #41）

## 1. 概要とアーキテクチャ

### 1.1 背景
[Issue #28](https://github.com/yshdsnd/mc-hires-bridge/issues/28) および [Issue #29](https://github.com/yshdsnd/mc-hires-bridge/issues/29)（`docs/pause-deferral-design.md`）の分析により、異形式自動曲送り（例: 24bit/96kHz → 16bit/44.1kHz、または 24bit/48kHz → 16bit/48kHz）時に手動操作が最大約 2.7 秒間ブロックされる問題の原因が判明しました。
再生エンジン（`OpcDSPlay.dll`）は内部排他制御（CriticalSection）を保持したまま DirectShow の `IMediaControl::Pause()` / `IMediaFilter::Pause()` を呼び出します。従前の同期型待機実装では、このフック内部で前曲の PCM 出力完了を `Sleep` ループで同期待機していたため、エンジン排他制御が 2.7 秒間拘束され、UI スレッドからの手動操作（Pause, Seek, Stop, TrackChange）がブロックされていました。

### 1.2 非ブロッキング仮想 Pause（Virtual Pause）のアーキテクチャ
Issue #33、#34、#35、および #36 では、この排他制御拘束を解消するため「非ブロッキング仮想 Pause（Virtual Pause）」方式を採用・改善実装しました（`experiments/eos_transition/` および `flac24_async_experiment`）。

```
[UI スレッド / ユーザー手動操作]
       |
       | < 1 ms で到達可能（排他制御フリー）
       v
[再生エンジン (OpcDSPlay.dll)]
       |
       | 1. 自動曲送り時 Pause 呼出 (RVA 0xDAC9) -> 旧グラフ
       v
[IMediaControl::Pause / IMediaFilter::Pause フック]
       |-- 遷移コンテキスト（transitionId, generation, enterTick）を生成
       |-- 仮想 Pause 状態（Virtual Pause）を設定
       |-- 即座に S_OK を返却 (< 2 ms)
       v
[再生エンジン (OpcDSPlay.dll)]
       |-- 即座に内部排他制御 (CriticalSection) を解放！
       |
       | 2. 後続の自動制御呼出 (遷移世代・対象グラフ・UI入力履歴に基づく峻別)
       |---> IDirectSoundBuffer::Stop() --------> [仮想Pause中の自動停止を抑止 (DS_OK 返却、再生継続)]
       |---> 旧グラフへの SetPositions(~20s) ---> [旧グラフ内部シークを退避・吸収 (OLD_GRAPH_PARKED)]
       |                                          (ドレイン中断・旧グラフのフラッシュを防止)
       |---> 新グラフへの次曲一元準備 (Issue #38)
       |     ・SetPositions(0) -----------------> [頭出しシークを退避・保留 (DEFERRED_SETPOSITIONS_QUEUED)]
       |     ・IMediaControl::Run() ------------> [再生開始を保留・抑止 (DEFERRED_CONTROL_RUN_QUEUED)]
       |     ・DirectSoundBuffer::Play() -------> [バッファ再生を保留・抑止 (DS_PLAY_DEFERRED_FOR_VIRTUAL_PAUSE)]
       |     (※ 旧グラフ待機解除時に シーク(0) → Run() → Play() の順に厳密に一度だけ確定適用！)
       | (※ ユーザーの手動非ゼロシーク・手動停止・手動再生は UI 入力検知により即時最優先実行)
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
  新グラフに保留シークを適用    ユーザー操作を最優先実行
  (PAUSE_DEFERRED_COMPLETED)    遅延 EOS は破棄 (stale_eos == 0)
                                (PAUSE_DEFERRED_CANCELLED)
```

### 1.3 状態遷移と構成要素（Issue #36 改善点を含む）
- **旧グラフへの非ゼロシークと新グラフへのゼロシークの完全分離**:
  - Issue #35 では内部シーク判定に `seekPos > 0` での手動シーク判定を用いていましたが、実機観測により、仮想 Pause 突入の約 6ms 後に旧グラフに対して絶対位置約 20.066 秒の内部シーク（旧グラフの境界位置決め/パーキング呼出）が発行されていることが判明しました（Issue #36）。
  - この旧グラフへのシークは仮説として扱い、位置の数値や特定モジュールだけに依存せず、**「仮想 Pause 中の旧グラフ宛ての自動呼出（ユーザー入力なし）」** として分類（`OldGraphAutomatedPark`）し、ドレインをキャンセルせず、また旧グラフをフラッシュして残存音声を破壊しないよう安全に吸収（`S_OK` 返却）します。
  - 約 40ms 後に開く次曲の**新グラフ（`newGraph`）** に対して発行される頭出しシーク（`SetPositions(0)`）は、保留シーク（`AutomatedDeferred`）として新グラフのインターフェース情報とともに退避され、旧グラフのドレイン完了時に新グラフに対して**一度だけ適用**されます。
- **手動非ゼロシークおよび手動停止の即時優先保証**:
  - 遷移中であっても、ユーザーの UI 操作（マウス、キーボード、シークバークリック）が観測された場合は `ManualCancelled` として即座に仮想 Pause を取り消し、保留操作をパージした上でユーザー操作を直ちに反映します。
- **DirectSound レンダラー停止の抑止**:
  - 仮想 Pause 中の自動 `IDirectSoundBuffer::Stop()` 呼出をフック層で確実に抑止（`DS_OK` 返却）し、ハードウェアバッファ内の残存 PCM（約 2.9 秒）の再生を維持します。
- **2段階ドレイン完了判定（PCM 到達 vs レンダラー最終出力完了）**:
  - **Stage 1（下流への PCM 到達 + 下流 EOS 送出）** と **Stage 2（DirectSound レンダラーの最終出力完了）** の双方が満たされて初めて `PAUSE_DEFERRED_COMPLETED` とし、新グラフに対して保留シークを安全に 1 度だけ適用します。

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
1. `PAUSE_ASYNC_VIRTUAL_ENTER` (rate, bits, hr=S_OK): 旧グラフで仮想 Pause に突入し、エンジンへ即座に `S_OK` を返却したことを記録。
2. `PAUSE_DEFERRED_PENDING` (remainingMs, 0, hr=S_OK): 非同期ドレイン監視が開始されたことを記録。
3. `DS_STOP_SUPPRESSED_FOR_VIRTUAL_PAUSE` (0, 0, hr=DS_OK): 旧グラフ DirectSound バッファの自動停止呼出が抑止されたことを記録。
4. `OLD_GRAPH_SETPOSITIONS_PARKED` (pos, flags, hr=S_OK): 旧グラフに対するエンジン内部シーク（約 20.066 秒等）が安全に吸収・パーキングされ、ドレインが維持されたことを記録。
5. `DEFERRED_SETPOSITIONS_QUEUED` (newPos, flags, hr=S_OK): 次曲の新グラフに対する頭出しシーク（位置 0）が安全に退避・保留されたことを記録。
6. `EOS_DOWNSTREAM_DELIVER` (rate, bits, hr=S_OK): 前曲ドレイン完了（Stage 1）に伴い下流 EOS を正常送出したことを記録。
7. `PAUSE_DEFERRED_COMPLETED` (status=CompletedDrain, hr=S_OK): Stage 1（PCM到達）および Stage 2（レンダラー最終出力）が完了。
8. `DEFERRED_SETPOSITIONS_EXECUTED` (flags, 0, hr=S_OK): 新グラフに対して保留されていた頭出しシークが 1 度だけ適用されたことを記録。

### 2.3 手動割り込み時ログシーケンス
仮想 Pause 期間中にユーザーが操作（Pause, Seek, Stop, TrackChange, Exit）を行った場合：
1. `PAUSE_ASYNC_VIRTUAL_ENTER`: 仮想 Pause 突入。
2. `PAUSE_DEFERRED_PENDING`: 非同期監視中。
3. `PAUSE_DEFERRED_CANCELLED` (reason, elapsedMs, hr=S_OK): 手動操作（手動非ゼロシーク、手動停止等）を UI 入力検知により即時認識し仮想 Pause を即時取消（キャンセル理由は以下のいずれか）：
   - `CancelledManualPause`: ユーザー手動一時停止
   - `CancelledSeek`: ユーザー手動シーク
   - `CancelledStop`: ユーザー手動停止
   - `CancelledTrackChange`: ユーザー手動曲送り/戻し
   - `CancelledExit`: アプリケーション終了（WM_QUIT）
4. `DEFERRED_SETPOSITIONS_PURGED` (count, 0, hr=S_OK): 保留されていた自動曲送り用 `SetPositions` を破棄したことを記録。
5. ユーザー要求の DirectShow / DirectSound 操作（`BOUNDARY_CONTROL_PAUSE_ENTER`, `BOUNDARY_SEEK_SETPOSITIONS_ENTER` 等）が遅延なく実行。
6. 遅延して到着した前曲の EOS やコールバックは無効化され、次曲への誤送出（Stale EOS）は 0 件として記録。

### 2.4 再生開始・ドレイン進捗・タイムアウト診断イベント（Issue #37）
次曲の再生開始経路および旧グラフドレイン未達要因を特定するため、以下のイベントを診断ログに追加：
1. `CONTROL_RUN` / `CONTROL_RUN_NEW_GRAPH_DURING_VIRTUAL_PAUSE`: `IMediaControl::Run` 呼出をグラフ世代とともに記録。仮想 Pause 中に新グラフで Run が呼ばれた場合を検出。
2. `FILTER_RUN` / `FILTER_RUN_NEW_GRAPH_DURING_VIRTUAL_PAUSE`: `IMediaFilter::Run` 呼出をグラフ世代とともに記録。
3. `DS_PLAY` / `DS_PLAY_NEW_BUFFER_DURING_VIRTUAL_PAUSE`: `IDirectSoundBuffer::Play` 呼出を記録。
4. `SEAMLESS_BEGIN_FLUSH` / `SEAMLESS_END_FLUSH` / `EQ_BEGIN_FLUSH` / `EQ_END_FLUSH`: ピンフラッシュの発生を記録。
5. `DRAIN_PROGRESS_TICK`: ドレイン監視スレッドから約 500ms ごとに進捗ハートビートを出力（`downstreamBytes`, `targetBytes`, `upstreamBytes`, `curPos`, `stopPos`）。
6. `PAUSE_DRAIN_TIMEOUT_STATE`: タイムアウト（4.5 秒上限）到達時の未達条件を特定（`pcmDelivered`, `rendererFinished`, `targetBytes - downstreamBytes`, `stopPos - curPos`）。

### 2.5 次曲準備の一元確定ログシーケンス（Issue #38）
次曲の頭出しシークと再生開始（`Run`/`Play`）を同一グラフ・遷移世代で一元管理し、旧グラフの待機解除後に順序立てて適用：
1. `DEFERRED_SETPOSITIONS_QUEUED`: 仮想 Pause 中の新グラフへの `SetPositions(0)` を保留。
2. `DEFERRED_CONTROL_RUN_QUEUED`: 仮想 Pause 中の新グラフへの `IMediaControl::Run()` を保留（新グラフの早回り再生を抑止）。
3. `DS_PLAY_DEFERRED_FOR_VIRTUAL_PAUSE`: 仮想 Pause 中の新バッファへの `IDirectSoundBuffer::Play()` を保留。
4. 旧グラフの待機解除時（自然完了またはタイムアウト）に、厳密に以下の順序で一度だけ確定適用：
   - `DEFERRED_SETPOSITIONS_EXECUTED`: 新グラフ位置 0 へシーク適用。
   - `DEFERRED_CONTROL_RUN_EXECUTED`: 新グラフ `IMediaControl::Run()` を適用（新曲再生開始）。
   - `DEFERRED_DS_PLAY_EXECUTED`: 新バッファ `IDirectSoundBuffer::Play()` を適用。
5. 手動操作（手動 Pause, Seek, Stop, TrackChange, Exit, ManualPlay）発生時：
   - `DEFERRED_OPERATIONS_PURGED`: 保留されていた次曲のシーク・Run・Play をすべて安全に破棄。

### 2.6 PCM 配送不足とレンダラー進捗の切り分け診断イベント（Issue #39）
旧グラフのドレイン待機中に PCM 配送が停止する要因と DirectShow レンダラー位置が 0ms のままとなる要因を別々に切り分けるため、以下の詳細診断イベントを拡充：
1. `ASYNC_DRAIN_CLOCK_CHECK` (hasClock, 0, hrClock): 仮想 Pause 監視開始時に旧グラフの `IMediaFilter::GetSyncSource` を確認し、リファレンスクロックの有無を記録。
2. `ASYNC_DRAIN_SEEKING_INIT` (isMediaTime, timeFormat.Data1, hrTf): 旧グラフ `IMediaSeeking` の時間フォーマットが `TIME_FORMAT_MEDIA_TIME` であるかを記録。
3. `DRAIN_HEARTBEAT_POS` (curPos_ms, stopPos_ms, hrCurPos): ドレイン監視スレッドから約 500ms ごとに再生位置と `GetCurrentPosition` の HRESULT を記録。
4. `DRAIN_HEARTBEAT_PCM` (downstreamBytes, targetBytes, transitionGeneration): 世代別の下流到達 PCM バイト数と目標バイト数を記録。
5. `DRAIN_HEARTBEAT_BYTES` (upstreamBytes, downstreamBytes, completionFlags): 上流送出バイト数と下流到達バイト数を比較記録。
6. `DRAIN_HEARTBEAT_DSOUND` ((playCursor << 32) | writeCursor, dsStatus, hrDsPos): 旧 DirectSound バッファの実際のハードウェア再生・書込カーソルおよび再生状態（`DSBSTATUS_PLAYING` 等）を直接取得して記録。
7. `PAUSE_DRAIN_TIMEOUT_UPSTREAM_DIFF` (upstreamBytes, targetBytes, generation): タイムアウト時の上流送出完了状態を記録。
8. `PAUSE_DRAIN_TIMEOUT_DSOUND` ((playCursor << 32) | writeCursor, dsStatus, hrDsPos): タイムアウト時の DirectSound ハードウェアカーソル位置を記録。

---

## 3. ヘッドレス競合テストスイート（Tests 40〜67）検証結果

Issue #30、#33、#34、#35、#36、#37、#38、#39 の競合テストハーネス（`AsyncControlHarness`）を再利用・拡張し、仮想 Pause の非同期特性、48kHz 実測回帰（Issue #35 / #36）、旧グラフ非ゼロシークのパーキング、新グラフゼロシークと再生開始（Run/Play）の一元確定適用、世代別ハートビート進捗、DirectSound ハードウェアカーソル追跡、および PCM 供給不足と位置計測の独立切り分けを検証する Tests 40〜67 を実装・全件合格を確認しました。

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
| **TEST 55** | 48kHz auto track change event sequence regression (Issue #35 / #36) | 仮想 Pause 開始 → DirectSound `Stop()` 抑止 → 旧グラフ非ゼロシーク（約 20.066 秒）パーキング → 新グラフ頭出しシーク（位置 0）保留 → 残存 PCM（約 2.9 秒）完全排出 → 新グラフへ 1 度だけシーク適用を完全検証 | **PASS** |
| **TEST 56** | Manual seek not misclassified from same caller | 仮想 Pause 保留中に同一呼び出し元から手動シーク（位置 > 0 またはユーザー入力後）が発生した場合、手動シークと判定して即時反映することを確認 | **PASS** |
| **TEST 57** | Manual stop regression during Virtual Pause | 仮想 Pause 保留中に手動停止が発行された場合、直ちに仮想 Pause を取消してグラフおよび DirectSound を完全停止することを確認 | **PASS** |
| **TEST 58** | Next track zero seek applied to new graph exactly once (Issue #36) | 重複したエンジン頭出し呼出が発生した場合でも、新グラフへの頭出しシークがドレイン完了時に厳密に 1 回だけ適用されることを確認 | **PASS** |
| **TEST 59** | New graph playback precedes deferred seek & causes timeout duplicate (Issue #37) | 新グラフ再生開始（`Run`/`Play`）が保留シーク適用に先行し、旧グラフ未達によるタイムアウト（約 4.5 秒後）で保留シークが実行されて次曲が頭出しリセットされ重複波形が生じる実機現象を完全モデル化して検証 | **PASS** |
| **TEST 60** | Unified seek (0) and playback start (Run/Play) on natural drain (Issue #38) | 仮想 Pause 中の新グラフ位置 0 シーク、Run、Play を一元保留し、自然完了時に シーク(0) → Run → Play の順序で厳密に一度だけ確定適用されることを検証 | **PASS** |
| **TEST 61** | Unified seek (0) and playback start on timeout: no duplicate waveform (Issue #38) | 旧グラフ未達タイムアウト時にも次曲が先行再生されず、タイムアウト時に初めて シーク(0) → Run → Play が適用され重複波形を完全解消することを検証 | **PASS** |
| **TEST 62** | Manual cancellation purges unified seek and playback start (Issue #38) | ユーザー手動操作（Stop等）発生時に一元保留されたシーク・Run・Play が即座に全破棄され、古い再生命令が一切発生しないことを検証 | **PASS** |
| **TEST 63** | Transition generation update discards stale preparation (Issue #38) | 遷移世代が更新（新世代突入）された場合、旧世代の一元保留命令が無効化され新世代へ誤適用されないことを検証 | **PASS** |
| **TEST 64** | COM thread boundary marshaling and execution across apartments (Issue #38) | `CoMarshalInterThreadInterfaceInStream` による `IMediaSeeking` および `IMediaControl` のアパートメント間ストリームマーシャリング・復元実行が正常動作することを検証 | **PASS** |
| **TEST 65** | Heartbeat generation, upstream/downstream/target byte counters, and HRESULT (Issue #39) | ハートビートにおいて世代番号、上流バイト数、下流バイト数、目標バイト数、および `GetCurrentPosition` の HRESULT が正確に記録されることを検証 | **PASS** |
| **TEST 66** | DirectSound buffer hardware cursor and status tracking across Virtual Pause (Issue #39) | 仮想 Pause 保留中に旧 DirectSound バッファが正常に関連付けられ、ハードウェア再生カーソルおよび `DSBSTATUS_PLAYING` 状態が監視可能であることを検証 | **PASS** |
| **TEST 67** | Independent isolation of PCM delivery deficit vs renderer position (Issue #39) | 下流 PCM 配送不足（差分 228,960 bytes）と DirectShow 再生位置 0ms（クロック停止・API 成否）を独立して判定・切り分けられることを検証 | **PASS** |
| **TEST 68** | Old buffer identification, reference lifetime preservation, and continuous cursor reporting (Issue #40) | レンダラー `IAMDirectSound` クエリによる旧バッファ特定、`AddRef`/`Release` による監視スレッド参照寿命保護、ハードウェアカーソル継続取得を検証 | **PASS** |
| **TEST 69** | DirectSound SetCurrentPosition, SetVolume, and filter Stop suppression (Issue #40) | 仮想 Pause 中の自動 `SetCurrentPosition(0)`、`SetVolume(-10000)`、フィルター停止・一時停止命令の抑止と、手動停止時の即時キャンセル優先を検証 | **PASS** |
| **TEST 70** | Clean natural EOS propagation without loop injection (Issue #41) | ループ内での直接 EOS 送出を完全廃止し、自然な下流 EOS 到達・重複送出抑止・取消後 Stale EOS 破棄・実 HRESULT 記録を検証 | **PASS** |
| **TEST 71** | Symmetrical diagnostic telemetry in sync (#28) vs async (#33-#40) (Issue #41) | 同期方式（#28）と非同期方式（#33〜#40）で同一の診断項目（最終 PCM、EOS、停止・位置操作、カーソル取得）を対称記録・比較可能であることを検証 | **PASS** |
| **TEST 72** | DirectSound buffer cursor diagnostics and failure reporting (Issue #41) | `safeQueryDirectSoundBuffer` における Null ポインタ（E_POINTER）、バッファ喪失（DSERR_BUFFERLOST）、例外（ERROR_INVALID_ADDRESS）、および AddRef/Release 寿命管理を検証 | **PASS** |

> 合計 72 件（基本テスト 1〜28、Issue #30 競合テスト 29〜39、Issue #33 / #34 / #35 / #36 / #37 / #38 / #39 / #40 / #41 非同期テスト 40〜72）すべてがエラーなしで通過しました。

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

#### シナリオ A: 48kHz 自動曲送りの末尾欠落解消確認（参照 PCM と共有ループバック照合）
- **対象**: 48 kHz/24 bit FLAC から 48 kHz/16 bit FLAC への自動曲送り（ユーザー操作なし）。
- **確認事項**:
  - 先行曲（約 23 秒）の PCM が約 20.2 秒で打ち切られず、末尾約 2.9 秒まで欠落なく再生されること（末尾欠落 0ms）。
  - 旧グラフへのシーク（約 20.066 秒）でドレインがキャンセルされず `OLD_GRAPH_SETPOSITIONS_PARKED` として処理されること。
  - レンダラー出力完了と同時に次曲が新グラフで**一度だけ**頭出し・開始されること。
  - ログに `DS_STOP_SUPPRESSED_FOR_VIRTUAL_PAUSE`, `OLD_GRAPH_SETPOSITIONS_PARKED`, `DEFERRED_SETPOSITIONS_QUEUED` が記録され、`PAUSE_DEFERRED_COMPLETED` で自然終了すること。

#### シナリオ B: 保留区間中の手動一時停止（Manual Pause）の即時反映
- **対象**: 先行曲末尾約 2 秒前の仮想 Pause 保留区間で、一時停止ボタンをクリック。
- **確認事項**:
  - クリック直後（< 50 ms）に音声が一時停止し、UI 応答遅延（2.7 秒間の排他ロック拘束）が発生しないこと。
  - ログに `PAUSE_DEFERRED_CANCELLED (CancelledManualPause)` および `DEFERRED_SETPOSITIONS_PURGED` が記録されること。

#### シナリオ C: 保留区間中の手動非ゼロシーク（Manual Seek）の即時反映
- **対象**: 保留区間でシークバーをクリックまたはドラッグ（例: 50 秒へシーク）。
- **確認事項**:
  - 旧グラフの内部シークと同一スレッド・呼出元であっても、UI 入力検知により手動シークと判定され、ユーザー指定位置へ即座にジャンプすること。
  - ログに `PAUSE_DEFERRED_CANCELLED (CancelledSeek)` が記録されること。

#### シナリオ D: 保留区間中の手動停止（Manual Stop）の即時反映
- **対象**: 保留区間で停止ボタンをクリック。
- **確認事項**:
  - 直ちに音声出力が停止し、停止状態へ遷移すること。
  - ログに `PAUSE_DEFERRED_CANCELLED (CancelledStop)` が記録されること。

#### シナリオ E: Issue #37 再生開始経路・タイムアウト要因の特定（診断ログ採取）
- **対象**: 48 kHz/24 bit FLAC から 48 kHz/16 bit FLAC への自動曲送り。
- **確認事項**:
  - 先行曲の約 20.2 秒付近で次曲波形が出現する際、どの経路（`CONTROL_RUN`, `FILTER_RUN`, `DS_PLAY` 等）が新グラフ・バッファで呼ばれているか確認する。
  - 約 24.6 秒付近でタイムアウト（`PAUSE_DEFERRED_COMPLETED (status=TimedOut)`）が発生する際、直前に出力される `PAUSE_DRAIN_TIMEOUT_STATE` から旧グラフが未達となった原因（`pcmDelivered` が偽か、`rendererFinished` が偽か、未到達 PCM バイト数、またはレンダラー進行度）を特定する。
  - ピンフラッシュ（`SEAMLESS_BEGIN_FLUSH`, `EQ_BEGIN_FLUSH` 等）がどのタイミングで発生しているか確認する。

#### シナリオ F: 次曲再生開始と保留シークの一元確定・重複波形解消確認（Issue #38 実機検証）
- **対象**: 48 kHz/24 bit FLAC から 48 kHz/16 bit FLAC への自動曲送り。
- **確認事項**:
  - **重複波形の完全解消**: 先行曲再生中（仮想 Pause 中、約 20.2 秒付近）に次曲の音声波形が早期出現しないこと。次曲の音声波形は先行曲のドレイン完了（または安全タイムアウト時）に**一度だけ**出現すること（Issue #37 で観測された 20.220 秒と 24.621 秒の二重波形が解消されていること）。
  - **先行開始抑止ログの確認**:
    - 仮想 Pause 突入後（約 20.2 秒〜20.3 秒）、新グラフに対する `IMediaControl::Run` が `DEFERRED_CONTROL_RUN_QUEUED` として抑止・保留されること。
    - 新バッファに対する `IDirectSoundBuffer::Play` が `DS_PLAY_DEFERRED_FOR_VIRTUAL_PAUSE` として抑止・保留されること。
  - **一元確定順序の検証**:
    - ドレイン完了時（またはタイムアウト時）に、厳密に以下の順序で確定・実行されること:
      1. `DEFERRED_SETPOSITIONS_EXECUTED` (位置 0 へのシーク)
      2. `DEFERRED_CONTROL_RUN_EXECUTED` (新グラフの Run)
      3. `DEFERRED_DS_PLAY_EXECUTED` (新バッファの Play)
  - **手動操作時のキャンセル安全性**:
    - 仮想 Pause 保留区間中に手動停止・一時停止・シーク・手動再生を行った場合、一元保留されたシーク・Run・Play が全て即座に破棄され、古い再生命令や新グラフの誤再生が一切発生しないこと。

#### シナリオ G: 旧グラフの PCM 配送不足とレンダラー進捗停止の切り分け（Issue #39 実機検証）
- **対象**: 48 kHz/24 bit FLAC から 48 kHz/16 bit FLAC への自動曲送り。
- **目的**: タイムアウト時に `pcmDelivered == 0` および `rendererFinished == 0` となった旧グラフの停止要因を、PCM 供給停止（上流・中間フィルター）とレンダラー位置計測（DirectShow クロック / DirectSound ハードウェアカーソル）に分離して特定する。
- **確認事項と判定基準**:
  1. **リファレンスクロックと時間単位の確認**:
     - `ASYNC_DRAIN_CLOCK_CHECK`: 旧グラフに有効なリファレンスクロックが設定されているか（`hasClock == 1` か `0` か）。
     - `ASYNC_DRAIN_SEEKING_INIT`: `IMediaSeeking` の時間単位が `TIME_FORMAT_MEDIA_TIME`（`isMediaTime == 1`）か。
  2. **PCM 配送停止の切り分け判定**:
     - `DRAIN_HEARTBEAT_PCM` / `DRAIN_HEARTBEAT_BYTES`:
       - `upstreamBytes == targetBytes`（約 6,624,000 bytes）に達しているか確認。
       - `downstreamBytes` が約 6,395,040 bytes で固定され増加していない場合、不足分 228,960 bytes（約 0.795 秒分）が `OmgSeamlessFilter` / `OmgDseeXlFilter` 中間キューに滞留しているか、下流レンダラーのブロックによって送出停止したことが確定。
  3. **DirectShow 再生位置 0ms の切り分け判定**:
     - `DRAIN_HEARTBEAT_POS`: `hrCurPos` が `S_OK` かエラーコード（`VFW_E_NO_CLOCK` 等）かを確認。
       - `hrCurPos != S_OK`: API 取得失敗に起因して値が 0ms のままとなったと判定。
       - `hrCurPos == S_OK` かつ `curPos == 0`: DirectShow グラフ内部のリファレンスクロックが停止またはストリームタイムがリセットされたと判定。
  4. **DirectSound ハードウェア再生カーソルの照合**:
     - `DRAIN_HEARTBEAT_DSOUND`:
       - `dsStatus & DSBSTATUS_PLAYING` が 1（再生中）か 0（停止）かを確認。
       - `dsPlayCursor` が約 20.2 秒相当（約 5,817,600 bytes 付近）で進捗しているか停止しているかを確認。
       - **判定**: `dsPlayCursor` が実時間通りに進捗していれば、DirectShow グラフ側の位置通知が 0ms であっても音声ハードウェア自体は正常に鳴動していることが特定できる。


#### シナリオ H: 旧グラフ PCM 供給とレンダラー末尾維持の実機検証（Issue #40 実機検証）
- **対象**: 48 kHz/24 bit FLAC から 48 kHz/16 bit FLAC への自動曲送り（実機共有出力、Music Center for PC 2.7.4 x86）。
- **目的**: 仮想 Pause 突入後の旧バッファ特定・参照保護、消音/停止/位置リセット抑止、および上流完了時 EOS 送出による中間遅延キュー（228,960 bytes）のフラッシュにより、先行曲末尾（約 23 秒）まで音声が維持されることを検証する。
- **確認事項と判定基準**:
  1. **旧バッファ特定と参照寿命保護**:
     - `DRAIN_DSOUND_ATTACHED`: Virtual Pause 突入時に `CLSID_DSoundRender`（`IAMDirectSound::GetSecondaryBufferInterface`）から旧バッファが捕捉・登録されたか。
     - `DRAIN_HEARTBEAT_DSOUND`: 500ms ごとに旧バッファの再生カーソル（`dsPlayCursor`）と再生中ステータス（`DSBSTATUS_PLAYING`）が継続出力されるか。
  2. **エンジン側の消音・停止・位置リセット抑止**:
     - `DS_SETCURRENTPOS_SUPPRESSED_FOR_VIRTUAL_PAUSE`: `IDirectSoundBuffer::SetCurrentPosition(0)` 等の位置リセットが抑止され、ハードウェア再生カーソルがジャンプ・リセットされないこと。
     - `DS_SETVOLUME_SUPPRESSED_FOR_VIRTUAL_PAUSE`: `IDirectSoundBuffer::SetVolume(-10000)` 等の消音命令が抑止され、ボリュームが保持されること。
     - `CONTROL_STOP_SUPPRESSED_FOR_VIRTUAL_PAUSE` / `FILTER_STOP_SUPPRESSED_FOR_VIRTUAL_PAUSE` / `FILTER_PAUSE_SUPPRESSED_FOR_VIRTUAL_PAUSE`: 旧グラフおよびフィルターに対する自動停止・一時停止命令が抑止され、ストリーミングスレッドが末尾まで維持されること。
  3. **中間遅延キュー（228,960 bytes）の供給維持**:
     - `EOS_DELIVERED_ASYNC_DRAIN`: 上流完了（`upstreamBytes == 6,624,000`）に伴い EOS が EQ 入力ピンに送出されること。
     - `DRAIN_HEARTBEAT_PCM`: `downstreamBytes` が 6,395,040 bytes から 6,624,000 bytes まで完全に進行すること。
  4. **実機ループバック波形の末尾確認**:
     - 約 20.1 秒付近での波形途絶・不連続フラグが解消し、先行曲が末尾（約 23 秒）まで正常に鳴動すること。
     - ドレイン完了時に `PAUSE_DEFERRED_COMPLETED` が出力され、次曲へ自然に引き継がれること（Issue #38 の単一起動が維持され、重複波形が発生しないこと）。

#### シナリオ I: 同期方式（#28）と非同期方式（#33〜#40）の対称診断比較と仮説検証（Issue #41 実機検証）
- **対象**: 48 kHz/24 bit FLAC から 48 kHz/16 bit FLAC への自動曲送り（共有出力、Music Center for PC 2.7.4 x86）。
- **目的**: ループ内での直接 EOS 送出（427 回発生）を廃止し、実機で曲末が鳴り切っていた同期方式（`MC_BRIDGE_PAUSE_MODE=defer`）と非同期方式（`MC_BRIDGE_PAUSE_MODE=async`）を同一の診断項目で対称採取・比較し、末尾欠落要因を切り分ける。
- **検証する 4 つの仮説**:
  1. **仮説 1（バッファオブジェクトの有効性と寿命）**:
     - `CLSID_DSoundRender`（`IAMDirectSound::GetSecondaryBufferInterface`）経由で取得したセカンダリバッファは同期方式・非同期方式の双方で有効か？
     - `GetCurrentPosition` / `GetStatus` が `S_OK` を返しているか、あるいは `E_POINTER`、`DSERR_BUFFERLOST`（`0x88780096`）、`ERROR_INVALID_ADDRESS` が発生しているか？
     - `DSOUND_BUFFER_ADDREF` / `DSOUND_BUFFER_RELEASE` による参照カウント推移から、バッファが非同期ドレイン中に解放（0）されていないか？
  2. **仮説 2（下流 PCM 総バイト数と実曲長）**:
     - 曲末まで正常再生された同期方式（#28）において、下流 PCM 総バイト数（`downstreamBytes`）は 6,624,000 bytes まで到達しているか、それとも同期方式でも 6,395,040 bytes で終了しているか？
     - もし同期方式でも 6,395,040 bytes で曲末まで正常に鳴り切っているならば、6,395,040 bytes こそが先行曲の真のデコード音声総量（残りはゼロパディングまたはメタデータ領域）である。
  3. **仮説 3（Pause 後のエンジン後続操作）**:
     - 同期方式では `OpcDSPlay.dll` が `Pause()` 内部でブロックされるため、後続の停止・リセット呼出が 0 件となる。
     - 非同期方式では即座に制御が戻るため、エンジンが発行する後続呼出（`SetPositions(0)`、`Stop()`、`Pause()`、`SetVolume(-10000)`、`SetCurrentPosition(0)`）がすべて観測される。これらが完全に抑止されているか、あるいは未捕捉の操作が存在するか？
  4. **仮説 4（EOS の到達タイミングと自然伝播）**:
     - ループ内直接送出を行わないクリーンな状態で、上流（`SeamlessFilter`）から下流（`EQ`）へ自然に EOS が伝播しているか？
     - `EOS_ENTER_SEAMLESS`, `EOS_LEAVE_SEAMLESS`, `EOS_UPSTREAM_PROCESSED`, `EOS_ENTER_EQ_PROPAGATED`, `EOS_LEAVE_EQ_PROPAGATED` の呼び出し箇所、遷移世代、および実際の HRESULT を対称記録し、不自然な遅延や脱落がないか確認する。
- **確認手順**:
  1. `MC_BRIDGE_PAUSE_MODE=defer` で同一曲送りを実行し、同期ベースラインログを採取。
  2. `MC_BRIDGE_PAUSE_MODE=async` で同一曲送りを実行し、非同期ログを採取。
  3. 両ログの `DRAIN_HEARTBEAT_PCM`, `DRAIN_HEARTBEAT_BYTES`, `DRAIN_HEARTBEAT_DSOUND`, `EOS_*` を直接対比し、仮説 1〜4 を検証する。
