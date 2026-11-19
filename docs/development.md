# 開発・診断資料

通常の使い方は[利用ガイド](package-readme.md)を参照してください。従来方式の説明と測定条件は各調査時点の記録です。

## ソースの配置

| ディレクトリ | 用途 |
| --- | --- |
| `src/bridge/` | 配布するブリッジDLLと状態表示 |
| `src/launcher/` | 配布するランチャーEXE・リソース |
| `src/common/` | DLLとランチャーで共有する起動通知 |
| `tests/` | 通常版の状態表示・設定解析の確認 |
| `diagnostics/` | API・形式・PCM・曲末の観測、手動ロード、解析補助 |
| `diagnostics/tests/` | 診断ツールの動作確認 |
| `experiments/legacy/` | 従来の16bit迂回方式の接続試験 |
| `experiments/wav192/` | WAV 192kHz試験用処理と独立検証 |
| `assets/`, `tools/` | アイコン素材とアイコン生成スクリプト |
| `docs/`, `vendor/` | 調査記録・説明書、第三者ライブラリー |

通常のビルドは配布用DLLとランチャーだけを生成します。診断・実験用は下記のオプションで明示的に有効化します。WAV試験DLLは通常版FLAC処理のソースを再利用し、実験用マクロを指定した別ターゲットとしてビルドします。旧迂回DLLも診断用DirectShowソースを再利用します。

## 従来の16bit迂回方式

確認した環境では、96kHz FLACの再生中にSonyのDSEEフィルターの出力が44.1kHzとなっていました。DSEE、Seamless、EQを完全に外すと96kHz再生はできましたが、経過時間表示が停止しました。

修正版は次の2経路を作ります。

```text
FLAC source (96kHz/16bit) → Infinite Pin Tee
  ├→ DirectSound renderer (96kHz/16bit) → 音声出力
  └→ DSEE → Seamless → EQ (44.1kHz/16bit) → Null Renderer
```

Sonyの位置管理用経路を無音で残します。可聴音声にはDSEE・EQが適用されません。ネイティブの位置取得に必要な内部関数は未特定です。

## ビルド

Windows 11、Visual StudioのC++デスクトップ開発環境、Windows SDK、CMake 3.20以降が必要です。Visual Studioの開発者PowerShellで実行してください。

```powershell
cmake -S . -B build-Win32 -A Win32
cmake --build build-Win32 --config Release
```

Music Center for PCは検証したバージョンでは32bitなので、Win32版DLLとインジェクターを使用します。x64対象の診断には別ビルドを使用します。

```powershell
cmake -S . -B build-x64 -A x64 -DMC_BRIDGE_BUILD_DIAGNOSTICS=ON
cmake --build build-x64 --config Release
```

診断ツールと実験ターゲットも使う場合は次を実行します。EXE・DLLの出力先とターゲット名は従来どおりです。配布ZIPには通常版だけが入ります。

```powershell
cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_DIAGNOSTICS=ON -DMC_BRIDGE_BUILD_EXPERIMENTS=ON
cmake --build build-Win32 --config Release
# 通常版の確認は必要なときだけ実行
cmake --build build-Win32 --config Release --target bridge_status_tests track_tail_tests
./build-Win32/Release/bridge_status_tests.exe
./build-Win32/Release/track_tail_tests.exe
```

WAV試験用ターゲットはオプションを有効にしたうえで個別指定します。[試験版のビルド手順](wav192-experiment.md)を参照してください。

## 試験適用

1. Music Centerを完全終了して再起動し、再生を始める前に対象PIDを確認します。
2. 次を実行します。PIDは実際の値に置き換えてください。

```powershell
$targetPid = 1234
$dll = (Resolve-Path .\build-Win32\Release\dshow_bypass_timing.dll).Path
& .\build-Win32\Release\inject.exe $targetPid $dll
```

3. `%TEMP%\DirectShowTimingBypass-PID.tsv`で初期化結果を確認し、96kHz音源を再生します。`TIMING_BRANCH_APPLIED`と実際の接続形式を確認してください。DLLのロード成功だけでは迂回成功を意味しません。
4. Music Centerを完全終了して再起動すると解除されます。自動ロードは実装していません。

`dshow_probe.dll`、`dshow_bypass.dll`、`dshow_bypass_timing.dll`は同じ関数をフックします。同一プロセスへ重ねてロードせず、切り替える際はアプリを再起動してください。

## 検証済みの範囲

2026-11-01、Windows 11 / Music Center for PC 2.7.4 x86で確認。

- DirectSoundの96kHz/16bit/stereo出力、経過時間更新、一時停止、シーク後の再開、自動次曲。
- 先頭20秒のPCMが旧迂回版とバイト単位で一致し、FFTで24–40kHzの成分を確認。
- 最終ソースのフィルター種類確認ガードはビルド検証済み。実再生試験はガード追加直前の同じ接続処理で実施。

従来の`dshow_bypass_timing`単体は24bit出力への改善ではありません。全形式、ギャップレス、長時間安定性、出力デバイス変更、枝間の厳密な同期は未検証です。無音のSony経路も動作するため追加負荷があります。この従来方式はランチャーの自動適用対象ではありません。

## 診断用ソース

| ターゲット / ファイル | 役割 |
| --- | --- |
| `dshow_bypass_timing` | 位置管理を維持する修正版 |
| `flac24_override` | 確認済み48・96・192kHz/24bit FLACの事前16bit指定を24へ変更（x86のみ） |
| `dshow_bypass` | 旧迂回版。経過時間が停止する比較用 |
| `dshow_probe` | DirectShowグラフ・接続形式の観測 |
| `probe` | DirectSound / WASAPIの形式観測 |
| `position_probe` | 標準位置取得APIの観測 |
| `negotiation_probe` | 接続前の形式交渉とSony FLACの出力設定の観測（検証済みx86用） |
| `pcm_tap` | DirectSound PCMをストリームごと最大20秒取得 |
| `wasapi_pcm_probe` | WASAPI形式の観測と出力直前PCMの取得（最大20秒、通常の`probe`とは併用不可） |
| `inject` | 同じアーキテクチャのDLLを対象プロセスへロード |
| `smoke`, `dshow_smoke`, `bypass_test` | 診断・接続試験 |
| `diagnostics/analyze-pcm.mjs` | PCMのFFT解析（Node.js） |

`diagnostics/observe.ps1`は形式観測用の補助スクリプトです。既定では`build-Win32/Release`または`build-x64/Release`のinjectとprobeを使い、`-BinPath`で別の出力先も指定できます。診断ツールのビルドには`MC_BRIDGE_BUILD_DIAGNOSTICS=ON`が必要です。

24bit音源が16bitになる箇所については、Music Centerの`OpcFlac.dll`が接続前にSony FLACフィルターへ16を指定する呼び出しを捕捉しました。[形式交渉の調査結果](bit-depth-investigation.md)を参照してください。これは原因の切り分けであり、24bit出力への修正ではありません。`negotiation_probe.dll`も他の観測版・迂回版と同時にロードしないでください。

ログにはファイルパスや音源名が含まれる場合があります。PCM取得ツールは音声データを保存します。Issueへ添付する前に個人情報と共有範囲を確認してください。このリポジトリには個人の実測ログや音声データを含めていません。

## 開発・Issue

不具合、改善案、検証結果は[このリポジトリのIssues](https://github.com/yshdsnd/mc-hires-bridge/issues)で管理します。OS・アプリのバージョン、対象形式、操作手順、期待した結果と実際の結果を記載してください。

本プロジェクト独自部分は[BSD 2-Clause License](../LICENSE)で公開しています。

MinHook v1.3.4（commit `c3fcafdc10146beb5919319d0683e44e3c30d537`）を`vendor/minhook`に同梱しています。第三者ライセンスは同ディレクトリの`LICENSE.txt`を参照してください。
