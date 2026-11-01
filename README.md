# MC HiRes Bridge

[Wune](https://github.com/yshdsnd/wune)の開発によって再生中の音声の周波数成分を確認できるようになり、Music Center for PCのDirectSound出力でハイレゾ音源を再生しても、約22kHzを上限として、それ以上の周波数成分が出力されていないことに気付きました。同じ音源をfoobar2000で再生すると高域まで確認できたことが、このツールを開発するきっかけです。

MC HiRes Bridgeは、Music Center for PC本体のファイルを改変することなく、Music Center使用時に音源の高域を保ったハイレゾ出力ができるようにすることを目的としています。

Windows上のMusic Center for PCの再生経路を調査し、経過時間表示を維持しながら96kHz音声の44.1kHz変換を迂回する、実験段階のC++ツールです。

Sonyとは関係のない非公式プロジェクトです。対象アプリのインストール済みバイナリや音源ファイルは変更しません。実行中のプロセスにDLLをロードし、一時的にDirectShowの接続を変更します。

その後の調査では、標準のWASAPI排他出力なら迂回なしで96kHz/有効24bitと高域を確認できました。本ツールの迂回機能は、共有出力を使いながら高域を保ちたい場合を対象とします。[排他モードの検証結果](docs/wasapi-exclusive-investigation.md)も参照してください。

96kHz/24bit FLACについては、sourceへの16bit指定だけを24bitへ変更すると、Music Center自身がSeamless・EQを通る96kHz/24bit共有出力を構築することも確認できました。[共有24bit化の試験結果](docs/shared-24bit-investigation.md)と実験用`flac24_override`を追加しています。対応範囲は限定されています。

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
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Release
```

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

従来の`dshow_bypass_timing`単体は24bit出力への改善ではありません。全形式、ギャップレス、長時間安定性、出力デバイス変更、枝間の厳密な同期は未検証です。無音のSony経路も動作するため追加負荷があります。一般利用向けのGUIや自動適用機能はまだありません。

## 診断用ソース

| ターゲット / ファイル | 役割 |
| --- | --- |
| `dshow_bypass_timing` | 位置管理を維持する修正版 |
| `flac24_override` | 確認済み96kHz/24bit FLACの事前16bit指定を24へ変更する実験版（x86のみ） |
| `dshow_bypass` | 旧迂回版。経過時間が停止する比較用 |
| `dshow_probe` | DirectShowグラフ・接続形式の観測 |
| `probe` | DirectSound / WASAPIの形式観測 |
| `position_probe` | 標準位置取得APIの観測 |
| `negotiation_probe` | 接続前の形式交渉とSony FLACの出力設定の観測（検証済みx86用） |
| `pcm_tap` | DirectSound PCMをストリームごと最大20秒取得 |
| `wasapi_pcm_probe` | WASAPI形式の観測と出力直前PCMの取得（最大20秒、通常の`probe`とは併用不可） |
| `inject` | 同じアーキテクチャのDLLを対象プロセスへロード |
| `smoke`, `dshow_smoke`, `bypass_test` | 診断・接続試験 |
| `analyze-pcm.mjs` | PCMのFFT解析（Node.js） |

`observe.ps1`は従来の`bin/x86`・`bin/x64`配置を前提とする形式観測用補助スクリプトです。通常のCMakeビルドでは、生成されたReleaseディレクトリ内のinjectとprobeを直接指定できます。

24bit音源が16bitになる箇所については、Music Centerの`OpcFlac.dll`が接続前にSony FLACフィルターへ16を指定する呼び出しを捕捉しました。[形式交渉の調査結果](docs/bit-depth-investigation.md)を参照してください。これは原因の切り分けであり、24bit出力への修正ではありません。`negotiation_probe.dll`も他の観測版・迂回版と同時にロードしないでください。

ログにはファイルパスや音源名が含まれる場合があります。PCM取得ツールは音声データを保存します。Issueへ添付する前に個人情報と共有範囲を確認してください。このリポジトリには個人の実測ログや音声データを含めていません。

## 開発・Issue

不具合、改善案、検証結果は[このリポジトリのIssues](https://github.com/yshdsnd/mc-hires-bridge/issues)で管理します。OS・アプリのバージョン、対象形式、操作手順、期待した結果と実際の結果を記載してください。

本プロジェクト独自部分は[BSD 2-Clause License](LICENSE)で公開しています。

MinHook v1.3.4（commit `c3fcafdc10146beb5919319d0683e44e3c30d537`）を`vendor/minhook`に同梱しています。第三者ライセンスは同ディレクトリの`LICENSE.txt`を参照してください。
