# 自動曲送り待機の非同期境界設計書（Issue #21 工程2 / Issue #29）

## 1. 背景と課題整理

### 1.1 現象と課題の所在
[Issue #28](https://github.com/yshdsnd/mc-hires-bridge/issues/28) の実機検証（Music Center for PC 2.7.4 x86, DirectSound 共有出力）において、以下の挙動が観測された：
1. **通常再生時**: マウスボタンを離してから約 3 ms で手動 Pause が DirectShow 境界（`BOUNDARY_CONTROL_PAUSE_ENTER`）に到達する。
2. **異形式曲送り（48 kHz/24 bit → 48 kHz/16 bit FLAC）の Pause 保留中**:
   - Win32 入力メッセージ（`APP_INPUT_DISPATCH`）はマウス押下時に即時ディスパッチされた。
   - しかし、その後約 2.74 秒間、再生エンジン（`OpcDSPlay.dll`）の排他ロック取得試行（`ENGINE_LOCK_TRY`）および DirectShow 操作が **1 件も記録されなかった**。
   - 2.74 秒後に前曲の曲末ドレイン処理が完了して保留が解除された直後に、初めてエンジン側の処理が進んだ。

### 1.2 根本原因の構造
自動曲送り時、再生エンジン（`OpcDSPlay.dll`）は内部の排他制御（CriticalSection）を取得した状態で `IMediaControl::Pause()` を呼び出す。
現行の試験実装（`deferFormatChangePause`）は、この `IMediaControl::Pause()` のフック内部で曲末 PCM の出力完了（ドレイン）を同期的ループ（`while` ループ + `Sleep(15)`）で待機している。
このため：
- `IMediaControl::Pause()` が復帰するまでの約 2.7 秒間、エンジンの排他制御が保持され続ける。
- ユーザーが手動で一時停止・シーク・停止を操作しても、UI スレッドからのエンジン呼び出しが排他制御の取得待ちでブロックされ、指令がエンジン内部および DirectShow 境界に到達できない。

### 1.3 解決に向けた設計要求と制約
- **中核要求**: 自動曲送りの待機処理を `IMediaControl::Pause()` 呼び出しの外へ移し、再生エンジンおよび UI スレッドへ制御を即座に戻す。
- **必須制約（除外条件）**:
  単に `IMediaControl::Pause()` を即時 `S_OK` で復帰させただけでは、後続処理（DirectSound 停止、位置変更、グラフ再構築）が直ちに実行され、前曲の残り 2.7 秒の出力が即座に打ち切られてしまう。したがって、**「Pause を先に返しただけで後続処理が前曲を打ち切る案」は明示的に除外**し、前曲の出力を確実に継続しつつ制御を戻せる境界を設計する必要がある。

---

## 2. 候補境界の比較検討

前曲の音声出力を継続したまま自動曲送り処理から制御を戻すための境界として、以下の 4 案を比較検討した。

```
[UI 層 / Win32 メッセージキュー]
       |
[Music Center アプリケーション制御層]
       |
[再生エンジン (OpcDSPlay.dll)]  <--- 境界案 1: 再生エンジン COM / 指令キュー境界
       | (内部排他制御 CriticalSection)
       |
       |---> IMediaControl::Pause()  <--- 現行境界 (同期的待機)
       |
       |---> IMediaSeeking::SetPositions()  <--- 境界案 2: DirectShow 下流制御境界 (Virtual Pause + 操作遅延)
       |
       |---> IDirectSoundBuffer::Stop()
       |
[DirectShow フィルタグラフ]
   [OmgFlacSrc] ---> [SeamlessFilter] ---> [OmgEqxlFilter] ---> [DSoundRender] ---> [サウンド出力]
                                                                        |
                                                           境界案 3: DirectSound 出力層境界
```

### 境界案 1: 再生エンジン COM / 指令キュー境界（`OpcDSPlay.dll` エントリ）
- **概要**: `OpcDSPlay.dll` の上位インターフェースまたは曲送り通知（EC_COMPLETE / 通知3841等）の受付時点で自動曲送りフローを一時停止（キューイング）し、前曲のドレイン完了通知を受けてから曲送りを再開する。
- **次曲要求の保留・再開**: エンジン上位で次曲の初期化要求を保留。
- **後続 SetPositions / グラフ破棄**: 曲送りフロー自体が進まないため、後続処理も自然に遅延される。
- **手動操作による取消**: エンジン排他制御が取得されていないため、手動操作が即時反映可能。
- **実行スレッドと COM 寿命**: アプリケーション内部のプライベートスレッドに依存。
- **評価と課題**:
  `OpcDSPlay.dll` の内部インターフェースおよび通知コードは完全な非公開仕様であり、バージョン間の互換性維持が極めて困難。リバースエンジニアリング依存度が高く、フォールバックの信頼性に課題が残る。

### 境界案 2: DirectShow 下流制御境界（Virtual Pause + 後続操作ゲーティング）★推奨・採用
- **概要**:
  `IMediaControl::Pause()` が呼ばれた際、グラフを実際には停止させず **Virtual Pause（仮想一時停止）** 状態として直ちに `S_OK` を呼び出し元（`OpcDSPlay.dll`）へ返す。
  これにより `OpcDSPlay.dll` は直ちに排他制御を解放する。
  直後に `OpcDSPlay.dll` が発行する後続の破壊的操作（`IDirectSoundBuffer::Stop()`、`IMediaSeeking::SetPositions()`、次曲の `Run()`）をフック層で検知・保留（ゲーティング）し、前曲のドレイン完了まで実行を遅延させる。
- **次曲要求の保留・再開**:
  次曲に向けた `SetPositions` やフォーマット設定のパラメータをフック層のキューに退避し、ドレイン完了時に本来の順序で再生エンジン/グラフへ適用する。
- **後続 SetPositions / グラフ破棄**:
  `dsStopHook` および `setPositionsHook` で即時実行を抑止し、前曲の再生継続を保証。
- **手動操作による取消**:
  排他制御が即時解放されているため、手動操作（Pause, Seek, Stop, TrackChange）は < 15ms でエンジンおよび DirectShow フックに到達。Virtual Pause 状態を直ちに破棄し、手動操作を最優先実行。
- **実行スレッドと COM 寿命**:
  同一 STA / COM アパートメント内で完結し、スレッド間マーシャリング不要。
- **評価と課題**:
  既存のフック基盤（`controlPauseHook`, `dsStopHook`, `setPositionsHook`, `controlStopHook`）をそのまま活用でき、非公開 API に依存しない。最も確実で安全なアプローチ。

### 境界案 3: DirectSound 出力層境界（シャドウバッファ分離）
- **概要**:
  DirectShow グラフ側は `Pause`、`Stop`、`SetPositions` を即座に完了させるが、前曲の残り 2.7 秒の PCM を DirectSound の独立したセカンダリバッファ（シャドウバッファ）へ退避し、DirectShow とは独立して最後まで再生させる。
- **評価と課題**:
  DirectSound 共有モードにおいて、48 kHz/24 bit から 48 kHz/16 bit（または 96 kHz 等）へのサンプリングレート・ビット深度変更を伴う場合、同一オーディオエンドポイントで異なるフォーマットのバッファを並行再生するとハードウェア競合やリサンプラーの不整合が発生するリスクが高い。

### 境界案 4: STA 非同期ワーカースレッド分離（COM 非同期化）
- **概要**:
  `IMediaControl::Pause()` の待機処理をバックグラウンドスレッドへオフロードする。
- **評価と課題**:
  `IMediaControl::Pause()` は同期型 COM インターフェース（`STDMETHOD(Pause)(void)`）であり、呼び出し元スレッドは戻り値 `HRESULT` を同期的に待つ。ワーカースレッドに処理を投げても呼び出し元へ戻り値を返さなければスレッドはブロックされ続け、戻り値を返せば後続処理が前曲を打ち切る。したがって、後続処理のゲーティング（境界案2）を伴わない非同期化は成立しない。

---

### 境界案の比較一覧表

| 評価項目 | 境界案 1: 再生エンジンAPI層 | 境界案 2: DirectShow 下流制御（Virtual Pause） | 境界案 3: DirectSound 出力層 | 境界案 4: STA ワーカースレッド |
| :--- | :--- | :--- | :--- | :--- |
| **排他制御の即時解放** | ○（未取得段階で保留） | **◎（1 ms 以内に解放）** | ◎（即時解放） | ×（COM 同期呼び出しが待機） |
| **前曲音声の末尾保持** | ○（曲送り自体を待機） | **◎（下流遮断により 100% 保持）** | △（フォーマット切替競合） | ×（即時復帰なら打ち切り） |
| **手動操作の即時応答** | ○（即時受付） | **◎（< 15 ms で反映・取消）** | ○（DirectShow 側は即時） | ×（排他制御待ちが残存） |
| **後続処理の整合性** | △（状態不整合リスク） | **◎（パラメータ保持・完了後再生）** | ×（二重再生・順序制御難） | △（非同期完了通知が必要） |
| **同形式先読みへの影響** | なし（形式変更時のみ発火）| **なし（既存判定ロジックを流用）** | なし | なし |
| **COM 寿命・スレッド安全性** | △（未公開内部スレッド） | **◎（同一アパートメント内で安全）** | △（デバイスリソース競合） | ×（STA マーシャリング課題） |
| **実現性と保守性** | 低（バイナリ依存大） | **高（標準 DirectShow フックで完結）**| 中（オーディオ制御が複雑） | 低（同期 COM の制約） |

---

## 3. 試作に採用する境界とその根拠

### 3.1 採用境界
**境界案 2: DirectShow 下流制御境界（Virtual Pause + 後続操作ゲーティング）** を試作対象として選定する。

### 3.2 選定根拠
1. **排他制御の確実な即時解放**:
   `IMediaControl::Pause()` に対して即座に `S_OK` を返すため、`OpcDSPlay.dll` は `[esi+78h]` の排他制御を < 1 ms で解放する。これにより、UI スレッドからの手動操作がロック競合で待たされる根本原因が完全に排除される。
2. **前曲音声の完全な出力継続**:
   `OpcDSPlay.dll` が直後に発行する `dsStopHook`（DirectSound 停止）および `setPositionsHook`（位置リセット）を Virtual Pause フラグによって透過的に抑止・キューイングするため、前曲の残り 2.7 秒は DirectSound バッファからスピーカーへ途切れなく出力され続ける。
3. **手動操作の優先割り込み（キャンセル）が極めて容易**:
   ドレイン待機中に手動操作（Pause, Seek, Stop）が到達した場合、保留していた自動曲送りパラメータを破棄し、手動操作の DirectShow API を直ちに実実行できる。
4. **既存基盤の最大活用と安全性**:
   プロジェクトで既に実績のある `eos_transition_hooks.inl` の COM フック（MinHook）とトラッカー機構をそのまま活用でき、第三者非公開 API への依存や不安定なメモリ書き換えを一切含まない。

---

## 4. 詳細シーケンスとタイムライン

### 4.1 正常自動曲送り（手動操作なし）
```
時刻 (相対)    OpcDSPlay.dll (エンジン)               フック層 (Virtual Pause 制御)           DirectShow / DirectSound
-------------------------------------------------------------------------------------------------------------------------
t = 0 ms       [排他制御 EnterCriticalSection]
t = 0.5 ms     IMediaControl::Pause() 呼出  ------> [controlPauseHook]
                                                    Virtual Pause 開始 (virtualPause = true)
                                                    EOS トラッカーにドレイン完了監視を設定
t = 1.0 ms     Pause() が S_OK で即時復帰    <------ [S_OK を即時返却]
t = 1.2 ms     [排他制御 LeaveCriticalSection] ★排他ロック解放！
               (エンジンは曲送り処理を継続)
t = 2.0 ms     IDirectSoundBuffer::Stop()   ------> [dsStopHook]
                                                    Virtual Pause 中を検知し実実行を抑止
                                             <------ [S_OK を即時返却] (音声は継続再生)
t = 3.0 ms     IMediaSeeking::SetPositions() ------> [setPositionsHook]
                                                    Virtual Pause 中を検知しパラメータ退避
                                             <------ [S_OK を即時返却] (位置変更は保留)
...            (前曲の残り 2.7 秒の PCM が DirectSound から出力され続ける)
t = 2700 ms                                          [ドレイン完了検知 (curPos >= stopPos)]
                                                    1. 退避していた SetPositions を実実行 -> [originalSetPositions]
                                                    2. 必要に応じ Pause/Stop を実実行      -> [originalControlPause]
                                                    3. virtualPause = false
                                                    次曲再生がスムーズに開始
```

### 4.2 保留区間中の手動一時停止（ユーザーが Pause をクリック）
```
時刻 (相対)    UI スレッド (ユーザー操作)             OpcDSPlay.dll (エンジン)              フック層 (Virtual Pause 制御)
-------------------------------------------------------------------------------------------------------------------------
t = 0 ms       (自動曲送り開始により Virtual Pause 突入、排他制御は既に解放済み)
t = 120 ms     一時停止ボタンをクリック (WM_LBUTTONUP)
t = 122 ms     UI コントローラがエンジン呼出 ------> [排他制御 EnterCriticalSection] ★即時獲得可能 (wait_ms = 0)!
t = 123 ms                                          IMediaControl::Pause() 呼出 ------> [controlPauseHook]
                                                                                        Virtual Pause を検知
                                                                                        保留中の次曲パラメータを破棄
                                                                                        virtualPause = false
                                                                                        本物の Pause を実行 -> [originalControlPause]
t = 125 ms                                          Pause() 完了               <------ [S_OK]
t = 126 ms                                          [排他制御 LeaveCriticalSection]
t = 127 ms     UI 画面が一時停止状態に更新 (< 10 ms で完全反映！)
```

### 4.3 保留区間中の手動シーク
```
時刻 (相対)    UI スレッド (シークバー操作)           OpcDSPlay.dll (エンジン)              フック層 (Virtual Pause 制御)
-------------------------------------------------------------------------------------------------------------------------
t = 150 ms     シークバーをクリック/ドラッグ
t = 152 ms     UI コントローラがエンジン呼出 ------> [排他制御 EnterCriticalSection] ★即時獲得可能 (wait_ms = 0)!
t = 153 ms                                          IMediaSeeking::SetPositions() ----> [setPositionsHook]
                                                                                        Virtual Pause を検知
                                                                                        自動曲送りの保留パラメータを破棄
                                                                                        virtualPause = false
                                                                                        手動シークを実実行 -> [originalSetPositions]
t = 156 ms                                          SetPositions() 完了        <------ [S_OK]
t = 157 ms                                          [排他制御 LeaveCriticalSection]
               (シーク先から即座に再生が継続)
```

### 4.4 同形式先読み（同一フォーマット）時
- 前曲と次曲が同一フォーマット（例: 48 kHz/24 bit → 48 kHz/24 bit）の場合：
  既存の `eosTracker.isFormatChangePending(key)` が `false` であるため、Virtual Pause 機構は完全にバイパスされ、従来のシームレス連続再生が 100% 維持される。

---

## 5. 失敗時・異常系の復帰条件

| 異常事象 | 検知方法 | 復帰動作 | 最終状態 |
| :--- | :--- | :--- | :--- |
| **DirectSound ドレイン停止（ハードウェアストール）** | タイムアウト監視（上限 4.5 秒超過） | Virtual Pause を強制終了し、退避済みの `SetPositions` / `Pause` を実実行 | ハングアップを防止し、次曲へ強制遷移 |
| **ドレイン中の急激なグラフ破棄（アプリ終了）** | `WM_QUIT` 受信または FilterGraph の Release | 保留キューを即時破棄し、DirectSound バッファを解放 | メモリリークや COM 例外なく安全にプロセス終了 |
| **手動曲送り（スキップボタン連打）** | 新たなトラックオープン（`EOS_TRACK_OPEN` / 外部呼び出し） | 前曲の Virtual Pause 状態および保留操作を即座に破棄 | 最新の手動選曲トラックを最優先で再生開始 |
| **Virtual Pause 実装自体の不具合時の切り戻し** | 環境変数 `$env:MC_BRIDGE_PAUSE_MODE = "sync_defer"` | 従来の同期保留モード（#25 版）で動作 | 旧安定動作への即時フォールバックが可能 |

---

## 6. 次工程（実装・検証）への引き渡し項目

1. **実装対象ターゲット**:
   `experiments/eos_transition/` 配下の試験ターゲット（`flac24_pause_experiment.dll` / `flac24_pause_diagnostic.dll`）において、上記 Virtual Pause 状態機械および下流ゲーティングキュー（`pendingSetPositions` 等）を試作実装する。
2. **ヘッドレス検証シナリオ**:
   - Virtual Pause 突入から `IMediaControl::Pause` が即時復帰することの検証。
   - `dsStopHook` および `setPositionsHook` がドレイン完了まで正しく遅延されることの検証。
   - ドレイン完了時に遅延されたパラメータが正確に適用されることの検証。
   - 待機区間中に手動 Pause / Seek / Stop が入った際、遅延パラメータが破棄され手動操作が即時適用されることの検証。
3. **実機検証（Codex 担当）**:
   Music Center 実機にて、通常再生および曲間保留中に手動 Pause を押下し、`APP_INPUT_DISPATCH` から DirectShow 反映までの遅延が 10 ms 以内に収まることを診断ログで確認する。
