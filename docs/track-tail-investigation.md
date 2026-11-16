# 曲末が早く切れる問題の調査

2026-11-01、Windows 11、Music Center for PC 2.7.4 x86、96kHz/24bit/stereo FLAC。[Issue #5](https://github.com/yshdsnd/mc-hires-bridge/issues/5)。

## 原因

`flac24_override`の呼び出し元の限定条件に、次曲先読み用の呼び出しが含まれていなかった。

OpcFlac.dll（PE timestamp `0x63EB12E0`）が同じSony FLAC独自slot 7を呼ぶ際、戻り先は通常開始がRVA `0x32A85`、次曲先読みが`0x31E7A`となる。従来版は前者だけを変更していた。先読み側でもsource候補は96kHz/24bitだが、要求16をそのまま通し、現在の96kHz/24bit経路との連続再生が成立しなくなる。その後Music Centerは現在の経路を停止し、次曲を通常開始の経路で再構築していた。

## 実測で確認した順序

終端の約21秒前へシークし、同じ曲間を比較した。個人の曲名・実ログ・PCMは含めない。

- 通常再生：SeamlessFilter.axから独自通知3841 (`0xF01`)。同じ出力経路を保って次曲を準備し、約3秒後に3842 (`0xF02`)。その間DirectSoundは止まらず、サンプル時刻が曲境界を連続して越える。
- 従来ブリッジ：sourceからSeamless入力には終端まで届いていた。通知3841の約47ms後にDirectSoundのStopが呼ばれ、経路が作り直された。標準のEC_COMPLETEを待った停止ではない。
- その時点で、最後の区間の全長20.673秒に対し、DirectSound入力が受け取った最後のサンプルの終端は18.773秒。少なくとも約1.900秒はまだレンダラー入力まで届いていなかった。さらにレンダラー内の未再生分もある。約3秒早い停止を観測したが、欠落時間を常に正確に4秒とは断定しない。
- 診断版setterのログで、先読みの戻り先`0x31E7A`、source候補96kHz/24bit、16bitの素通しを確認。その直後、通常開始の`0x32A85`で24bitに戻されていた。

通知の発生元はスタックのモジュール名/RVAからSeamlessFilter.axと確認した。「先読み」「境界」の意味は前後の動作からの判断で、Sonyの公開仕様に基づく名称ではない。

## 修正と確認

既知の呼び出し元に`0x31E7A`を追加した。Sonyモジュールの版、要求値16、DirectSound設定、source候補96kHz/24bit/stereoの条件は維持する。通知の抑止や固定秒数の待機は追加しない。

修正後、同じ比較で先読みもSET_24となり、3841を受けてもDirectSoundを停止せず、同じSeamless・EQ・DirectSound入力で曲境界を越えて96kHz/24bitデータが継続した。3842も観測した。続く別の曲間でも、同じ入力オブジェクトの継続・曲境界通過・96kHz/24bit維持を確認した。2回とも終端約21秒前へシークして観測した。

前回の自動次曲試験は「次曲が再生される」ことの確認に留まり、曲末が欠けないことまで確認できていなかった。今回の確認はその不足を補うもの。出力機器でのアナログ収録や、異なる形式・レートをまたぐ連続再生は未検証。

## 終端観測DLL

`end_probe`は観測専用。IPinのEndOfStream/BeginFlush/NewSegment、IMemInputPin::Receive、グラフのIMediaEventSink::Notify、DirectSound Stopを記録する。サンプル時刻、形式、サイズ、最後の非ゼロフレームの時刻、Stop時のカーソルを記録し、PCMファイルは保存しない。時刻はGetTickCount64のミリ秒値で、異なるPC間の比較用ではない。

```powershell
cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_DIAGNOSTICS=ON
cmake --build build-Win32 --config Release --target end_probe
# 初回再生前に、実際のPIDへロードする。再起動で解除。
& .\build-Win32\Release\inject.exe $targetPid (Resolve-Path .\build-Win32\Release\end_probe.dll).Path
```

`%TEMP%\EndProbe-PID.tsv`のREADYを確認する。`flac24_override`との併用は今回の観測で確認済み。同じAPIをフックする他の観測・迂回DLLを追加しない。統計はpinポインター単位のため、再構築・ポインター再利用をまたぐ解析ではNewSegmentと形式も照合する。Receiveの記録は元Receiveを呼ぶ直前の受け渡し観測で、DACから実際に出たことを直接保証しない。

初期の観測版はストリーム内からグラフ位置を問い合わせてアプリを停止させたため、その版の試行は除外した。公開する観測版はその問い合わせを行わない。実アプリは再起動し、修正した観測版で比較し直した。

## GUIなしの曲末欠落・EOS回帰試験

個人の楽曲やMusic CenterのGUIを使わず、実行時に合成した48kHz・ステレオ・16/24bit整数PCMで曲末欠落・EOS後の無音パディング・次曲開始を判定するCLI試験（`track_tail_tests`、[Issue #19](https://github.com/yshdsnd/mc-hires-bridge/issues/19)）を追加した。PCMファイルや生成バイナリはリポジトリへ保存しない。

- 合成音源：1曲目は同相ステレオ（`L == +R`、末尾300msを強調）、次曲は逆相ステレオ（`L == -R`）をメモリー上で生成する。異なる曲長（`48,000`、`64,800`、端数`51,733`フレーム）とブロック境界（`480`、`1,024`、端数`1,111`、`24,000`フレーム）を含む。
- 判定内容：末尾全量到達、約300ms欠落（欠落のまま終了・無音置換・次曲への早期移行のいずれも非ゼロ終了）、EOS後の無音パディング長、次曲開始位置とフレーム数を区別する。
- 独立DirectShowグラフ比較（Win32でSonyフィルターが利用可能な環境）：`OmgEqxlFilter.ax`（EQ）と`OmgDseeHxFilter.ax`（DSEE HX `mode=0/1`）へ合成PCMを入力し、`IPin::EndOfStream`（EOS）前後の出力フレーム数・末尾到達・無音パディング・`EC_COMPLETE`完了イベントを比較する。Sonyフィルターがない環境（x64ビルドや未導入環境）では解析器の単体テスト（216ケース）を実行できる。

| 対象フィルター（48kHz / 16・24bit / 2ch） | EOS前の出力フレーム数 | EOS後の総出力フレーム数 | 有効末尾フレーム | EOS後の無音パディング | `EC_COMPLETE` |
| --- | --- | --- | --- | --- | --- |
| `OmgEqxlFilter.ax`（入力`48,000`、塊`24,000` / `480`） | `32,768`（未出力`15,232` = `317.3ms`） | `49,152` | `48,000`（全量） | `1,152`（`24.0ms`） | EOS前は未発生、EOS後に発生 |
| `OmgEqxlFilter.ax`（入力`51,733`、塊`1,111`） | `40,960`（未出力`10,773` = `224.4ms`） | `57,344` | `51,733`（全量） | `5,611`（`116.9ms`） | EOS前は未発生、EOS後に発生 |
| `OmgDseeHxFilter.ax` `mode=0/1`（入力`48,000`、塊`24,000`） | `47,616`（未出力`384` = `8.0ms`） | `48,000` | `48,000`（全量） | `0` | EOS前は未発生、EOS後に発生 |
| `OmgDseeHxFilter.ax` `mode=0/1`（入力`48,000`、塊`480`） | `24,064`（未出力`23,936` = `498.7ms`） | `48,000` | `48,000`（全量） | `0` | EOS前は未発生、EOS後に発生 |
| `OmgDseeHxFilter.ax` `mode=0/1`（入力`51,733`、塊`1,111`） | `48,640`（未出力`3,093` = `64.4ms`） | `51,733` | `51,733`（全量） | `0` | EOS前は未発生、EOS後に発生 |

EQはEOSを受けるまで内部に約224〜317ms（`8,192`フレーム境界単位）を保持し、EOS時に残り全量とブロック境界までのゼロパディングをフラッシュして`EC_COMPLETE`を通知する。一方、DSEE HXはEOS時にパディングなしで入力フレーム数ちょうどを出力して`EC_COMPLETE`を通知する。この試験は独立グラフでのフィルター挙動と解析器の回帰確認であり、Music Center本体での波形測定や実楽曲の自動曲送りを置き換えるものではない。

```powershell
cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_DIAGNOSTICS=ON
cmake --build build-Win32 --config Release --target track_tail_tests
# 解析器テスト（216ケース）＋Sonyフィルター独立グラフ比較（利用可能な場合）
./build-Win32/Release/track_tail_tests.exe
# 末尾全量（終了コード0）と約300ms欠落（終了コード1）の個別判定
./build-Win32/Release/track_tail_tests.exe full 24 48000 1152 9600
./build-Win32/Release/track_tail_tests.exe drop300ms 24 48000
./build-Win32/Release/track_tail_tests.exe eq-no-eos 24 48000
```
