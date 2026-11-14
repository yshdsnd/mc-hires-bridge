# 異形式自動曲送りにおける出力切断フローの観測手順（Issue #22）

[Issue #22](https://github.com/yshdsnd/mc-hires-bridge/issues/22) に対応する観測用ツール（`flow_observer.dll`）の設計・ログ仕様および実行手順。

## 背景と観測の目的

Issue #21 の試験版（`a4979df`）の実機検証（Music Center for PC 2.7.4 x86, DirectSound 共有出力, WASAPI ループバック観測）において、以下の事象が確認された：

- **48 kHz/24 bit → 48 kHz/16 bit FLAC**:
  23.0 秒の前曲が約 20.1 秒で途切れ、次曲が 20.200 秒で開始した。
  実測ログには `EOS_UPSTREAM → EOS_RESET_SEEK (20.088s) → EOS_NEW_SEGMENT → 16 bit の EOS_TRACK_OPEN` の順で記録され、既存の `IMediaControl::Stop` フックや停止保留、EQ への EOS 送出ログは一切出力されなかった。
- **48 kHz/24 bit → 48 kHz/24 bit FLAC（対照試験）**:
  先読みが正常に機能し、前曲末尾まで原音通り再生され、次曲開始は 23.000 秒だった。

この結果から、「既存の Stop フックが呼ばれなかった理由」および「20.088 秒の位置変更（`SetPositions`）が出力打ち切りの直接原因か、先行する処理の結果か」を切り分ける必要がある。

`flow_observer.dll` は、再生制御を一切改変しない（EOS 補送や停止保留を行わない）純粋な観測プローブとして、通知 3841、先読み開始と成否、`SetPositions`、フラッシュ、グラフ停止・切断・再構築、レンダラー停止の全イベントを単調時刻・スレッド・グラフ・呼び出し元モジュール＋RVA 付きで相関記録する。

---

## 観測アーキテクチャとフック一覧

| 観測対象 | インターフェース / 関数 | Vtable Index / RVA | 記録内容 |
| :--- | :--- | :--- | :--- |
| **DirectShow イベント** | `IMediaEventSink::Notify` | Index 3 | `code`（3841, 0x01 等）、`p1`, `p2`、呼び出し元バックトレース |
| **先読み / オープン** | `COpcFlacPlayer::Open` (`OpcFlac.dll`) | RVA `0x33290` | `flags`（1=先読み, 0=通常）、ファイル名、呼び出し元、戻り値 HRESULT、所要時間 |
| **クローズ** | `COpcFlacPlayer::Close` (`OpcFlac.dll`) | RVA `0x319E0` | 呼び出し元モジュール＋RVA、戻り値 HRESULT |
| **位置変更（シーク）** | `IMediaSeeking::SetPositions` | Index 14 | `currFlags`（シンボル展開）、`currPos`（100ns & 秒）、`stopFlags`、`stopPos`、呼び出し元バックトレース（4フレーム） |
| **位置取得** | `IMediaSeeking::GetStopPosition` / `GetCurrentPosition` / `GetPositions` | Index 11, 12, 15 | 取得位置（秒）、呼び出し元モジュール＋RVA |
| **グラフ制御（Stop/Pause/Run）**| `IMediaControl::Stop` / `Pause` / `Run` | Index 9, 8, 7 | 呼び出し元バックトレース、対象グラフ識別子 |
| **フィルター制御** | `IMediaFilter::Stop` / `Pause` / `Run` | Index 4, 5, 6 | 呼び出し元バックトレース、対象グラフ識別子 |
| **トポロジ変更** | `IFilterGraph::RemoveFilter`<br>`IGraphBuilder::Disconnect`<br>`IGraphBuilder::Reconnect` | Index 5<br>Index 16<br>Index 17 | フィルター名、CLSID、ピン名、接続方向、呼び出し元モジュール＋RVA |
| **個別フィルター停止** | `IBaseFilter::Stop` | Index 6 | フィルター名、CLSID（DSoundRender, Seamless, EQ 等）、呼び出し元 |
| **フラッシュ** | `IPin::BeginFlush` / `IPin::EndFlush` | Index 15, 16 | ピン名、ピン方向、フィルター名、呼び出し元モジュール＋RVA |
| **ストリーム終端・区間** | `IPin::EndOfStream`<br>`IPin::NewSegment` | Index 14<br>Index 17 | `tStart`, `tStop`, `rate`、ピン名、フィルター名、呼び出し元 |
| **DirectSound バッファー** | `IDirectSoundBuffer::Stop`<br>`IDirectSoundBuffer::Play`<br>`IDirectSoundBuffer::SetCurrentPosition` | Index 18<br>Index 12<br>Index 13 | 再生カーソル、書込カーソル、バッファー長、フォーマット（レート/bit/ch）、呼び出し元バックトレース |

---

## ログ形式仕様 (`%TEMP%\FlowObserver-<PID>.tsv`)

タブ区切りテキスト（TSV）形式。ヘッダー行：
```tsv
seq	time_us	tick_ms	tid	graph_id	obj_ptr	event	caller	details
```

| カラム名 | 型 | 説明 |
| :--- | :--- | :--- |
| `seq` | 整数 | 1 から始まるプロセス内完全連番（原子的にインクリメント） |
| `time_us` | 整数 | `QueryPerformanceCounter` に基づく単調増加マイクロ秒時刻 |
| `tick_ms` | 整数 | `GetTickCount64()` のミリ秒時刻 |
| `tid` | 整数 | 実行スレッド ID |
| `graph_id` | ポインタ | 関連する `IFilterGraph*`（特定不能な場合は `00000000`） |
| `obj_ptr` | ポインタ | 呼び出されたインターフェース／オブジェクトの `this` ポインタ |
| `event` | 文字列 | イベント種別識別子（例: `SEEK_SETPOSITIONS_ENTER`, `PIN_BEGIN_FLUSH` 等） |
| `caller` | 文字列 | 呼び出し元のモジュール名＋RVA（例: `OpcDSPlay.dll+0x000213A0`） |
| `details` | 文字列 | イベント固有のキー＝値ペア（位置、フラグ、バックトレース等） |

---

## 合成サンプルログによる対照例

### 1. 形式変更時の自動曲送り（24 bit → 16 bit）の合成イベント列
```tsv
seq	time_us	tick_ms	tid	graph_id	obj_ptr	event	caller	details
1	1002500	254100	4120	015A4200	01683210	DS_EVENT_3841	SeamlessFilter.ax+0x00004520	code=3841(0x0F01) p1=0x0 p2=0x0 backtrace=SeamlessFilter.ax+0x00004520 -> OpcDSPlay.dll+0x00012400
2	1003100	254101	1844	015A4200	0210A880	OPC_FLAC_OPEN_ENTER	OmgPcMan.dll+0x00018520	flags=1(SeamlessPreRead) file=Track2_16bit.flac
3	1004340	254102	1844	015A4200	0210A880	OPC_FLAC_OPEN_LEAVE	OmgPcMan.dll+0x00018520	flags=1 hr=0x80040200 elapsedUs=1240
4	1088450	254188	1844	015A4200	015A4220	SEEK_SETPOSITIONS_ENTER	OpcDSPlay.dll+0x000213A0	currFlags=0x01(Absolute) currPos=200880000(20.0880s) stopFlags=0x00(NoPositioning) stopPos=-1(-1.0000s) backtrace=OpcDSPlay.dll+0x000213A0 -> OmgPcMan.dll+0x00019200
5	1088600	254188	1844	015A4200	01689100	PIN_BEGIN_FLUSH	quartz.dll+0x00041200	pin_name=Input dir=input clsid={5C968488-5E35-4CFE-98A1-A51C838797DB} name=OmgEqxlFilter
6	1088720	254188	1844	015A4200	01701200	PIN_BEGIN_FLUSH	quartz.dll+0x00041200	pin_name=Input dir=input clsid={E30629D1-27E5-11CE-875D-00608CB78066} name=DirectSound Audio Renderer
7	1088850	254188	1844	015A4200	01701200	PIN_END_FLUSH	quartz.dll+0x00041200	pin_name=Input dir=input name=DirectSound Audio Renderer
8	1088900	254188	1844	015A4200	01689100	PIN_END_FLUSH	quartz.dll+0x00041200	pin_name=Input dir=input name=OmgEqxlFilter
9	1089100	254189	1844	015A4200	015A4220	SEEK_SETPOSITIONS_LEAVE	OpcDSPlay.dll+0x000213A0	hr=0x00000000
10	1090200	254190	1844	00000000	02485120	DS_STOP	dsound.dll+0x0001A400	playCursor=115200 writeCursor=120000 bufBytes=144000 rate=48000 bits=24 ch=2 backtrace=dsound.dll+0x0001A400 -> quartz.dll+0x00052100
11	1100000	254200	1844	015A4200	0210A880	OPC_FLAC_OPEN_ENTER	OmgPcMan.dll+0x00018600	flags=0(NormalTrackOpen) file=Track2_16bit.flac
```
> **解析ポイント**:
> - `OPC_FLAC_OPEN_LEAVE` で先読みが `0x80040200` で失敗した後、`CONTROL_STOP` は呼ばれず、`OpcDSPlay.dll+0x000213A0` から `SEEK_SETPOSITIONS_ENTER`（位置 20.088 秒）が発行されている。
> - `SetPositions` の直後に EQ および DirectSound Renderer ピンへの `PIN_BEGIN_FLUSH` が発生し、レンダラー内部の未再生 PCM が破棄された後に `DS_STOP` が呼ばれていることが確認できる。

### 2. 同形式（24 bit → 24 bit）の対照試験の合成イベント列
```tsv
seq	time_us	tick_ms	tid	graph_id	obj_ptr	event	caller	details
1	1002500	254100	4120	015A4200	01683210	DS_EVENT_3841	SeamlessFilter.ax+0x00004520	code=3841(0x0F01) p1=0x0 p2=0x0
2	1003100	254101	1844	015A4200	0210A880	OPC_FLAC_OPEN_ENTER	OmgPcMan.dll+0x00018520	flags=1(SeamlessPreRead) file=Track2_24bit.flac
3	1004100	254102	1844	015A4200	0210A880	OPC_FLAC_OPEN_LEAVE	OmgPcMan.dll+0x00018520	flags=1 hr=0x00000000 elapsedUs=1000
4	1150000	254250	3012	015A4200	01683210	PIN_NEW_SEGMENT	SeamlessFilter.ax+0x00006200	start=0(0.0000s) stop=230000000(23.0000s) rate=1.000 pin_name=Output dir=output
```
> **解析ポイント**:
> - 先読みが成功（`hr=0x00000000`）した場合、`SetPositions` や `PIN_BEGIN_FLUSH`、`DS_STOP` は一切発生せず、次曲の `PIN_NEW_SEGMENT` がそのまま連続して供給される。

### 3. 手動シーク時の合成イベント列
```tsv
seq	time_us	tick_ms	tid	graph_id	obj_ptr	event	caller	details
1	5001200	258100	1200	015A4200	015A4220	SEEK_SETPOSITIONS_ENTER	MusicCenter.exe+0x000F4120	currFlags=0x01(Absolute) currPos=100000000(10.0000s) stopFlags=0x00(NoPositioning) stopPos=-1(-1.0000s) backtrace=MusicCenter.exe+0x000F4120
2	5001400	258100	1200	015A4200	01689100	PIN_BEGIN_FLUSH	quartz.dll+0x00041200	pin_name=Input dir=input clsid={5C968488-5E35-4CFE-98A1-A51C838797DB}
3	5001600	258100	1200	015A4200	01701200	PIN_BEGIN_FLUSH	quartz.dll+0x00041200	pin_name=Input dir=input clsid={E30629D1-27E5-11CE-875D-00608CB78066}
4	5001700	258100	1200	015A4200	01701200	PIN_END_FLUSH	quartz.dll+0x00041200	pin_name=Input dir=input
5	5001750	258100	1200	015A4200	01689100	PIN_END_FLUSH	quartz.dll+0x00041200	pin_name=Input dir=input
6	5001900	258101	1200	015A4200	015A4220	SEEK_SETPOSITIONS_LEAVE	MusicCenter.exe+0x000F4120	hr=0x00000000
```
> **解析ポイント**:
> - 手動シークでは `3841` や `OPC_FLAC_OPEN` は先行せず、呼び出し元が GUI スレッドの `MusicCenter.exe` から直接発行される。

---

## Codex 担当者向け実機実行手順

> [!CAUTION]
> **重要: 二重ロードの禁止**
> `flow_observer.dll` は `flac24_override.dll` および `flac24_eos_experiment.dll` と同一の COM 関数をフックします。**絶対に同じプロセスへ同時にロードしないでください**。
> `MC HiRes Bridge.exe` は起動せず、必ず `MusicCenter.exe` を単体で起動してインジェクトしてください。

### 1. ビルド
```powershell
cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_DIAGNOSTICS=ON
cmake --build build-Win32 --config Release --target inject flow_observer
```

### 2. インジェクションと観測の開始
1. Music Center を起動（まだ再生は開始しない）。
2. PowerShell で対象 PID を取得し、`flow_observer.dll` をインジェクト：
   ```powershell
   $targetPid = (Get-Process MusicCenter).Id
   $dll = (Resolve-Path .\build-Win32\Release\flow_observer.dll).Path
   & .\build-Win32\Release\inject.exe $targetPid $dll
   ```
3. `%TEMP%\FlowObserver-<PID>.tsv` が生成され、先頭に `OBSERVER_READY` が出力されたことを確認。

### 3. 実機検証シナリオ
以下の3パターンを順に実施し、各シナリオのログを採取：
1. **異形式自動曲送り**:
   `48kHz/24bit FLAC` → `48kHz/16bit FLAC` をプレイリストに並べ、自然に自動曲送りさせる。
2. **同形式対照試験**:
   `48kHz/24bit FLAC` → `48kHz/24bit FLAC` を連続再生させる。
3. **手動シーク試験**:
   `48kHz/24bit FLAC` 再生中にシークバーをクリックして任意位置（例: 10秒付近）へ手動シークする。

### 4. ログの確認と回収
再生完了後、Music Center を終了し、`%TEMP%\FlowObserver-<PID>.tsv` を回収して確認します：
- 20.088 秒付近の `SEEK_SETPOSITIONS_ENTER` の `caller`（モジュール名＋RVA）と `backtrace`
- `CONTROL_STOP` または `FILTER_STOP` が呼ばれていたか、あるいは呼ばれなかったか
- `PIN_BEGIN_FLUSH` と `DS_STOP` が発生した時刻と呼び出し元
- `OPC_FLAC_OPEN_LEAVE` の HRESULT

---

## 観測限界と留意事項

- **ユーザーモード COM 境界**:
  本プローブは DirectShow / DirectSound の COM vtable および `OpcFlac.dll` の関数エントリーを観測します。Windows カーネルモードドライバ（`portcls.sys`）内部のバッファー消費や、ハードウェアオーディオエンドポイント自体の物理発音遅延はユーザーモードフックからは直接観測できません（これは WASAPI ループバック録音のタイムスタンプと本プローブの `DS_STOP` / `PIN_BEGIN_FLUSH` 時刻を突き合わせることで特定します）。
- **個人情報の保護**:
  生成される TSV ログやファイルパス、PCM データは Git リポジトリへコミットしないでください。Issue への報告時はファイル名や個人パスを伏せ、イベントシーケンスと RVA 情報のみを共有してください。
