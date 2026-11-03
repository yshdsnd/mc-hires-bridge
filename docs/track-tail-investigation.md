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
