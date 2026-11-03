# DirectSound共有出力での24bit化

調査日：2026-11-01。Windows 11、Music Center for PC 2.7.4 x86、96kHz/24bit/stereo FLAC。[Issue #3](https://github.com/yshdsnd/mc-hires-bridge/issues/3)。

## 結果と経路

共有出力で96kHz/24bitのPCMをDirectSoundへ渡すことができた。取得した20秒のPCMで24bitの下位桁と高域を確認し、経過時間も更新された。

必要だった変更は、OpcFlacがSony FLACの独自slot 7へ渡す16を、対象を限定して24へ置き換えることだけ。これを受けてMusic Center自身が次の経路を構築した。

```text
Sony FLAC source → Seamless → EQ → DirectSound
             全接続96kHz/24bit
```

DSEEはグラフに残るが未接続。source→DSEEの接続は`0x80040217`で失敗し、その後アプリ自身が別の接続を構築して再生を続けた。新フックはグラフの接続を変更しない。

最初は既存の`dshow_bypass_timing`もロードしたが、DSEE入力が未接続となり`BYPASS_SKIPPED eligible=0`を記録。時間維持用の分岐は追加されなかった。完全終了・再起動し、`flac24_override`と観測用DLLだけでも同じ結果を再現した。Seamlessを可聴経路に残せたことで、従来のsource→DirectSound直結で発生した位置表示停止も起きなかった。Sony内部の位置取得関数自体は未特定。

## 形式とPCM

- DirectSound実再生バッファー：96,000Hz、24bit、2ch、整数PCM、block align 6。CreateSoundBuffer成功。
- DirectSound内部のWASAPI初期化：共有モード（share mode 0）、96,000Hz、float32、2ch。排他へ変更したものではない。
- 独立試験の取得PCM：20秒、1,920,000フレーム、11,520,000バイト。3,840,000サンプル中3,763,656サンプルで24bitの下位8bitが非ゼロ。単なる16bitのゼロ埋めではない。
- 最初の試行と独立試験の20秒PCMはバイト単位で一致。
- 左ch FFT（65,536点Hann、hop 32,768、先頭0.8秒除外、55窓平均）の帯域電力は20Hz–20kHz比で、22.05–24kHzが−57.9120dB、24–40kHzが−47.5352dB、40–46kHzが−58.1289dB。

取得箇所はDirectSoundのバッファー。Windowsミキサー通過後やDAC実出力を直接測ったものではなく、共有モードの音量・ミキシング・音声効果を含めたビットパーフェクトは主張しない。

## 試験用実装

`src/bridge/flac24_override.cpp`、Win32専用ターゲット`flac24_override`を追加。インストール済みファイルや音源を変更せず、実行中の呼び出し引数を変更する。

適用条件は、確認済みOmgFlacSrc.axのPEタイムスタンプ`0x63EB12C3`とslot 7のRVA `0x1860`、呼び出し元OpcFlac.dllのタイムスタンプ`0x63EB12E0`と戻り先RVA `0x32A85`（通常開始）または`0x31E7A`（次曲先読み）、元指定値16、設定直前のsource候補が96kHz/24bit/2chの整数PCMであること。

24設定自体が失敗した場合は元の16へ戻す。その後の接続失敗を自動復旧する機能ではない。今回の接続失敗後の再構築はMusic Center自身の動作。

この調査時点のDLLは出力モードを独立には判定していなかった。その後の[状態表示付き版](status-ui.md)では、保存設定がDirectSoundの場合だけ変更する条件を追加した。

```powershell
cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_DIAGNOSTICS=ON
cmake --build build-Win32 --config Release --target flac24_override inject
# DirectSoundを選択し、完全終了・再起動後、初回再生前に実際のPIDを指定
$targetPid = 1234
& .\build-Win32\Release\inject.exe $targetPid (Resolve-Path .\build-Win32\Release\flac24_override.dll).Path
```

`%TEMP%\Flac24Override-PID.tsv`のREADYを確認して再生し、SET_24と実際の出力形式を確認する。ロード成功だけで24bit化成功と判断しない。完全終了・再起動で解除され、自動適用は追加しない。

この調査時点の独立試験では`dshow_probe`、`probe`、`pcm_tap`を観測用に併用。その後の状態表示付き版はRunもフックするため、`dshow_probe`を併用しない。`negotiation_probe`は同じ独自メソッドをフックするため併用しない。従来の迂回DLLも追加しない。

## 検証範囲と制限

Win32ビルド成功。2回の新規プロセスで96kHz/24bit共有出力を確認。経過時間、一時停止（00:44で停止）、停止中のシーク（02:04）、再開後の時間進行（03:08）、曲末付近へシークしてからの自動次曲を確認した。2曲目もSET_24、96kHz/24bitの実再生バッファー、共有WASAPI初期化が成功し、時間表示も進行した。長時間安定性、ギャップレスの厳密性、他のサンプルレート・形式・アプリバージョン・出力機器は未検証。

EQを通るため、その設定は音声に影響し得る。今回の経路ではDSEEは未接続となり使用できない。今回は低レベルの調査用DLLであり、GUI・自動適用・常用向けの復旧機能はまだない。

個人の実測ログ、音源情報、PCM、Sonyのバイナリは公開せず、結果の要約と独自ソースのみをリポジトリに含める。

後続調査で、従来版には次曲先読み時の適用漏れによる曲末欠落が判明した。修正内容と曲間の再検証は[曲末の調査](track-tail-investigation.md)を参照。上記の初回自動次曲試験は、曲末の完全性を確認したものではない。
