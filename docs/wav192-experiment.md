# 192kHz/24bit WAV用の試験実装

この機能は通常のランチャー／配布ZIPには含めない。Music Center本体での曲末・次曲・先読みの検証が終わるまで、対応済み形式として案内しない。

## 適用条件

- DirectSound選択時、グラフが停止状態でPause/Runに入る直前だけ接続を変更する。
- SonyWavParser2.axのCLSIDとx86 PE timestamp `0x63EB12BE`、またはQUARTZ.dll実装の標準WAVパーサーCLSIDを照合。Seamlessを使う場合はtimestamp `0x63EB12BA`も照合。
- パーサーの出力候補が192kHz/24bit/2chの通常整数PCM、block align 6、毎秒1,152,000バイト。
- 同じグラフに対象パーサーが1個あり、音声経路がパーサー→［DSEE］→Seamless→［EQ］→DirectSoundの形に限る。標準WAVパーサーの場合だけ、Seamlessを含まない形も扱う。
- FLACの既存適用範囲は48/96kHzのまま。DSDや192kHz FLACはこの実験の対象外。

## 変更内容

先にDSEEの形式設定を補正する。OmgDseeHxFilter.ax timestamp `0x63EB12C1`、独自IID `{6AF76DBE-8634-4FD3-93ED-982EDE1BFDD5}` のslot 3（RVA `0x36CA`、9 DWORDを値渡し）に限定する。呼び出し元は静的に確認したOpcWavEx.dll timestamp `0x63EB12E7`、戻り先RVA `0x33E47`に限定。mode 0/1・2ch・入力192kHz/24bit・要求出力44.1kHz/16bit・format 16の場合だけ、mode 0・出力192kHz/24bit・format 24へ変更する。他のフィールドは保持し、設定失敗時は元の要求へ戻す。この補正はSeamlessに低い形式を確定させないための準備であり、DSEEを経路に残して再生する機能ではない。

`wav192_route.h`で接続と形式・現在位置を保存してから接続を解除し、パーサー→Seamless→DirectSoundへつなぐ。パーサーの形式候補を複製してsubtypeをMEDIASUBTYPE_PCMにする。音声バッファーを変換せず、各フィルターオブジェクトも削除・置換しない。EQとDSEEは音声経路から外れる。

接続後にSeamless入力とDirectSound入力の形式を確認し、位置を復元する。失敗した場合は保存した形式で元の全接続を戻す。復元にも失敗した場合はRunを進めずエラーにする。再適用と再生中の変更は避ける。Seamless独自通知の抑制、EC_COMPLETEの偽装、固定時間待ちは行わない。

試験DLLは形式設定フックと既存のRunフックに加え、初回Pauseの前にも処理する。変更したDSEEがまだ接続されている場合は再生を進めない。通常版とは同じAPIをフックするため、同時にロードしてはいけない。試験DLLをMusic Center本体へロードしての検証も実施した。通常インストール先は置き換えていない。

## ビルドと独立検証

```powershell
cmake -S . -B build-Win32 -A Win32
cmake --build build-Win32 --config Release --target flac24_wav192_experiment wav192_route_probe wav192_dsee_probe
# Sonyフィルターを検査プロセス自身にロードする。注入は行わない。
& .\build-Win32\Release\wav192_route_probe.exe '対象192kHz24bit.wav'
& .\build-Win32\Release\wav192_route_probe.exe '対象192kHz24bit.wav' rollback
& .\build-Win32\Release\wav192_route_probe.exe '対象192kHz24bit.wav' tail
& .\build-Win32\Release\wav192_route_probe.exe '対象44.1kHz16bit.wav' skip
# 実DSEEを含む独立グラフ。先行する形式設定補正と迂回を組み合わせる。
& .\build-Win32\Release\wav192_dsee_probe.exe '対象192kHz24bit.wav' tail
# 元の低い形式で接続した場合は、変更に失敗して元の経路へ復元されることを確認。
& .\build-Win32\Release\wav192_dsee_probe.exe '対象192kHz24bit.wav' low
```

独立検証はパーサー→Seamless→EQ→DirectSoundから開始し、EQの迂回、途中の接続失敗からの復元、同じオブジェクトでの再適用、再生中の見送り、元WAVとのPCM照合を行う。曲末モードは最後の4秒、通常モードは30秒から検査する。比較箇所はDirectSound直前。DAC出力を測るものではない。`RESULT 0`だけでなく、各試験の結果とPCM件数も確認する。

**Music Center本体では標準WAVパーサーを使う192kHz/24bit/2ch WAV 1ファイルで検証した。** 実DSEEを含む独立グラフの試験は追加したが、独立検査はMusic Center固有のSeamless設定や次曲先読みを完全には再現せず、実アプリ由来のsetter呼び出しを再現するものではない。実アプリでは指定RVAからの呼び出しを確認した。複数のパーサーがある先読み構成では適用を見送る。ロード前にすでにPauseしているグラフも対象外である。実アプリの初回Pause/Run直前の状態が条件に合うかを確認する必要がある。

後段を単にRun直前に付け替えるだけでは不十分。すでに44.1kHz/16bitを受け入れたSeamlessの入力では、高い形式への再接続が拒否される。このケースは元の形式で復元する。DSEE設定を先に補正してから構築した独立グラフでは、同じSeamlessオブジェクトを使って迂回できる。独自メモリー領域を書き換えて形式キャッシュを消す方法は使用しない。

## 残る確認

- 異なる192kHz WAV同士や48/96kHz FLACをまたぐ曲間。
- 複数パーサーを同じグラフで使う先読みとギャップレスの厳密性。
- 長時間安定性、別ファイル・別環境への適用範囲。

これらの確認前に通常のDLLやインストール先を試験版へ置き換えない。

## 実アプリで分かった構成差

実アプリのWAV再生では標準WAVパーサーを使用した。DSEEの初回設定modeは1であり、UIでDSEEをOFFにする操作だけでは初回のmode 1を避けられなかった。そこで既知の呼び出し元と192kHz/24bit条件を満たすmode 0/1を対象に、準備用のmodeを0へ補正する。

初回はSeamlessを残す。繰り返し時には標準WAVパーサー→DSEE→EQ→DirectSoundというSeamlessなしの経路も構築された。この形は標準WAVパーサーに限り、パーサー→DirectSoundへ変更する。Sony純正パーサーでSeamlessを外す変更は行わない。

192kHz/24bitの実DirectSoundバッファー、192kHz/float32の共有WASAPI初期化、レンダラー入力20秒分と元WAVのバイト一致、Pause/再開/シーク、192kHz曲末から対象外のDSD曲への移行を実測した。これはDAC実出力の測定ではない。

`end_probe_pcm192`は明示ビルドする観測用ターゲット。通常のend_probeと同じフックを持つため併用しない。192kHz/24bit/2chのDirectSoundレンダラー入力を最大20秒だけローカルTEMPへ保存し、通常版end_probeは引き続きPCMを保存しない。生PCM・個人ログはリポジトリへ追加しない。

実アプリの具体的な検証結果は[192k-wav-playback.md](192k-wav-playback.md)を参照。
