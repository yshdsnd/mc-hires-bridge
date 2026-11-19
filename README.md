# MC HiRes Bridge

[Wune](https://github.com/yshdsnd/wune)の開発によって再生中の音声の周波数成分を確認できるようになり、Music Center for PCのDirectSound出力でハイレゾ音源を再生しても、約22kHzを上限として、それ以上の周波数成分が出力されていないことに気付きました。同じ音源をfoobar2000で再生すると高域まで確認できたことが、このツールを開発するきっかけです。

MC HiRes Bridgeは、Music Center for PC本体のファイルを改変することなく、Music Center使用時に音源の高域を保ったハイレゾ出力ができるようにすることを目的としています。

**Sonyとは関係のない非公式ツールです。独自の調査結果に基づいて開発しており、Sonyによる提供・承認・サポートはありません。**

## v0.2.0でできること

- **ハイレゾFLACの形式を維持** — 対応する48／96／192 kHz・24 bitのステレオFLACを、元のレートと24 bitのままDirectSoundへ渡します。
- **共有モードでDSEE HXを利用** — 44.1／48 kHzの対応音源を24 bitで出力します。HR-BRの右クリックで2倍（88.2／96 kHz）または4倍（176.4／192 kHz）を選べます。
- **現在の出力を確認** — HR-BRにマウスを置くと、入力形式、処理経路、DirectSoundへ渡す形式が表示されます。

Windows共有ミキサーは必要に応じてデバイスの既定形式へ変換します。対応環境、対象形式、設定変更の適用タイミングは[利用ガイド](docs/package-readme.md)を参照してください。

## ダウンロードと起動

[Releases](https://github.com/yshdsnd/mc-hires-bridge/releases)から配布ZIPを取得し、展開したフォルダーの **MC HiRes Bridge.exe** をダブルクリックします。Music CenterでDirectSoundを選び、DSEE HXを使う場合はオンにしてください。更新時はMusic Centerを完全に終了してからファイルを入れ替えてください。

再生中にDSEE HXのオン／オフや倍率を変えると、次に新しく開く曲から適用されます。先読み済みの曲は準備済みの設定で再生します。倍率メニューは領域外クリックで閉じてください。EscでMusic Centerが一覧へ戻り、メニューが残る制限があります。

## ドキュメント

- [利用ガイド・配布用README](docs/package-readme.md)
- [v0.2.0 リリースノート](docs/releases/v0.2.0.md)
- [開発・診断・ビルド方法](docs/development.md)
- [状態表示の仕組みと検証記録](docs/status-ui.md)

[GitHub Issues](https://github.com/yshdsnd/mc-hires-bridge/issues)で不具合・改善案・検証結果を管理します。ソースの独自部分は[BSD 2-Clause License](LICENSE)、同梱のMinHookは[MinHookのライセンス](vendor/minhook/LICENSE.txt)に従います。
