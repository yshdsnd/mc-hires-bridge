# WASAPI排他での再生動作

調査日：2026-11-01。対象：Windows 11、Music Center for PC 2.7.4 x86、デジタル出力（High Definition Audio Device）。[Issue #2](https://github.com/yshdsnd/mc-hires-bridge/issues/2)。

## 結果

今回の96kHz/24bit FLACは、Music Centerの標準のWASAPI排他出力で高域を保って再生できた。迂回フックは使用していない。音声形式・設定の監視と、アプリがWASAPIへ渡す直前のPCM取得のみを行った。

| 項目 | DirectSound（前回の実測） | 排他・手動96kHz/24bit | 排他・おまかせ |
| --- | --- | --- | --- |
| OpcFlacがsourceの独自slot 7へ指定する値 | 16 | 24 | 32 |
| source→DSEE | 96kHz/16bit | 96kHz/24bit | 96kHz/32bit |
| DSEE→Seamless | 44.1kHz/16bit | 96kHz/24bit | 96kHz/32bit |
| レンダラー | DirectSound | Sony OmgAudioRenderer | Sony OmgAudioRenderer |
| 最終音声APIへ渡す形式 | DirectSound 44.1kHz/16bit | WASAPI排他 96kHz/有効24bit | WASAPI排他 96kHz/有効24bit |
| 高域 | 約22kHzで切れる | 取得PCMで24–40kHz成分あり | 取得PCMで24–40kHz成分あり |

おまかせでsourceが32bit形式になっても、元の24bit音源の情報量が32bitへ増えるという意味ではない。

## WASAPIの実際の初期化

`OmgAudioRenderer.ax`を呼び出し元とする`IAudioClient::Initialize`を捕捉。手動・おまかせとも以下でS_OKだった。

- share mode: `1`（AUDCLNT_SHAREMODE_EXCLUSIVE）
- sample rate: 96,000Hz、2ch
- tag: WAVE_FORMAT_EXTENSIBLE、SubFormat: 整数PCM
- container bits: 32、valid bits: 24、block align: 8

「32bit出力」の表示だけで32bitの有効精度と解釈せず、**32bitの格納幅に有効24bitを収めた形式**として扱う。取得PCMでも下位8bitは全サンプル0。手動設定の3,840,000サンプル中3,755,484サンプルではbit 8–15が非ゼロで、単に16bitをゼロ埋めしたデータではなかった。

## 音声内容の観測

通常のループバックではなく、Music Center内の`IAudioRenderClient::GetBuffer`と`ReleaseBuffer`の間でPCMをコピーした。音声バッファーの内容は変更せず、ReleaseBufferが成功した分のみ保存した。

手動・おまかせのそれぞれで20秒、1,920,000フレーム、15,360,000バイトを取得。解析は整数32bit PCMとして読み、左ch、65,536点Hann窓、32,768点hop、先頭0.8秒を除外、55窓の平均スペクトル。

以下は20Hz–20kHz帯の電力に対する相対値。

| 帯域 | 手動 | おまかせ |
| --- | --- | --- |
| 22.05–24kHz | −58.0618dB | −58.0617dB |
| 24–40kHz | −47.6122dB | −47.6122dB |
| 40–46kHz | −58.2764dB | −58.2763dB |

両条件で約22kHz以上の成分が残った。取得PCM同士はそのままではバイト一致しない。開始位置の小さなずれもあるため、今回の結果からビットパーフェクトとは判定しない。また、DAC内部処理やアナログ出力の測定ではない。

## DSEE HX・再生操作

- DSEE HXのUI設定はオンのまま実施し、変更していない。96kHz音源に対して内部DSPがどの処理を実行したかは、接続形式だけでは判定していない。
- 排他の経路にはDSEE、Seamless、Sony Audio Rendererがあり、今回捕捉した接続にEQはなかった。
- 再生中の経過時間更新と一時停止を確認。排他の全操作、全形式、長時間安定性、他の出力機器、ASIOは未検証。

## 実装と再現

`wasapi_pcm_probe`を追加。従来の`probe.cpp`に`ENABLE_WASAPI_PCM_CAPTURE`を指定した診断用ターゲットで、`wasapi_capture.h`によるPCM取得を追加する。通常の`probe`はPCMを取得しない。

```powershell
cmake -S . -B build-Win32 -A Win32
cmake --build build-Win32 --config Release --target wasapi_pcm_probe negotiation_probe inject
# Music Centerを完全終了して再起動。再生前に実際のPIDを指定する。
$targetPid = 1234
& .\build-Win32\Release\inject.exe $targetPid (Resolve-Path .\build-Win32\Release\wasapi_pcm_probe.dll).Path
& .\build-Win32\Release\inject.exe $targetPid (Resolve-Path .\build-Win32\Release\negotiation_probe.dll).Path
```

この2つのDLLは今回併用して検証した。通常の`probe.dll`と`wasapi_pcm_probe.dll`は同じAPIをフックするため併用しない。グラフ観測版・迂回版も追加しない。

- `%TEMP%\AudioFormatProbe-PID.tsv`と`NegotiationProbe-PID.tsv`のREADYを確認し、排他出力を選択して再生する。
- PCMは`%TEMP%\WASAPITap-PID-index-rate-containerBits-validBits-channels.pcm`へ保存する。
- 取得は1ストリームにつき最大20秒、記録する初期化クライアントは最大16個。既存PCMは上書きしない。
- `capture.complete_20_seconds`を確認してから解析する。形式ログのSubFormatが整数PCMであることを確認する。FFTツールはfloat PCM用ではない。
- 同じCOM実装アドレスを対象とする観測であり、全ドライバー・別実装を網羅する保証はない。
- リアルタイム経路で一時コピーとディスク書き込みを行う試験用実装であり、常用や無影響を保証しない。実アプリで手動・おまかせ両条件の取得完了と正常な経過時間更新を確認し、Win32ビルドも確認した。

試験終了後は、出力モードを元のDirectSoundへ、排他出力形式を元の手動96kHz/24bitへ戻した。DSEE設定は変更せず、アプリ再起動で観測フックを解除する。音源・対象アプリのインストール済みファイルは変更していない。

## プロジェクトへの意味

「Music Centerはどの出力方式でもハイレゾ出力できない」という結論ではない。今回の制限は標準DirectSound経路で観測され、WASAPI排他では回避ツールなしで96kHz/有効24bitと高域を確認できた。

本ツールの迂回機能は、共有出力を使いながら高域を保ちたい場合に意味がある。標準の排他出力を使える用途では、まずそちらを選択できる。

公開するのは観測用ソースと結果の要約であり、個人の実測ログ、音源名、音声PCMはリポジトリへ含めない。
