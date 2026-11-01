# 48kHz/24bit FLACの共有出力

2026-11-01、Windows 11、Music Center for PC 2.7.4 x86。[Issue #6](https://github.com/yshdsnd/mc-hires-bridge/issues/6)。

## 変更

`flac24_override`のsource形式判定とHR-BR ON判定を、96kHzに加えて48kHzにも対応させた。24bit・ステレオ・FLAC・DirectSound設定・既知のSonyバイナリと2か所の呼び出し元という条件は維持する。44.1kHz/16bitは対象外のまま。

## 実再生で確認した結果

- 従来版：48kHz/24bit音源が48kHz/16bitとしてDirectSoundレンダラーに渡され、HR-BR OFF。
- 新版：SET_24、STATUS_ON、実画面のHR-BR ONを確認。
- sourceからSeamless、EQ、DirectSoundへの観測対象入力は48,000Hz/24bit/2chの整数PCM、block align 6。DSEEはこの24bit経路には含まれなかった。
- DirectSoundのPCMバッファーを20秒取得。5,760,000バイト、960,000フレーム、1,920,000サンプル中1,816,922サンプルで下位8bitが非ゼロ。16bitを24bitにゼロ埋めしただけではない。ただし元音源とのビット単位一致やDAC出力を保証する測定ではない。
- 曲末約21秒前へシークし、同じ48kHz/24bit形式の次曲への先読みでもSET_24を確認。通知3841→3842の間も同じSeamless/EQ/DirectSound入力が継続し、サンプル時刻が曲境界を越えた。曲末でDirectSound Stopは発生せず、次曲でもHR-BR ONを維持。シーク操作に伴うStopは曲末の異常停止とは区別した。

Win32のflac24_overrideとend_probeのビルド成功。個人の曲名・実ログ・PCM・生成DLLは公開していない。観測用のend_probe、pcm_tapを併用し、検証後はMusic Centerを再起動して解除する。

今回の追加実測は48kHz/24bit/stereo FLACと同形式の曲間に限定。96kHz対応は従来の確認範囲を維持し、異なるレート・形式をまたぐ連続再生、他バージョン・他機器は追加検証していない。
