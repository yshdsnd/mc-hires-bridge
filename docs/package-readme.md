# MC HiRes Bridge

MC HiRes Bridgeは、Music Center for PCの **DirectSound（共有）モード** で、ハイレゾFLACとDSEE HXを使うためのWindows用ツールです。

**Sonyとは関係のない非公式ツールです。独自の調査結果に基づいて開発しており、Sonyによる提供・承認・サポートはありません。**

## できること

### ハイレゾFLACを元の形式で出力する

対応する48／96／192 kHz・24 bitのステレオFLACを、元のサンプリング周波数と24 bitのままDirectSoundへ渡します。Music Centerの共有モードで44.1 kHzや48 kHzの16 bitに変換されるのを防ぎます。この機能はDSEE HXの設定に関係なく使えます。

### DirectSound（共有）でDSEE HXを使う

Music Centerでは通常WASAPI排他またはASIOで使うDSEE HXを、DirectSound（共有）でも利用できます。CD品質やMP3・AACなどの対応音源を再生するときに、Music Centerの **DSEE HX** をオンにしてください。出力は24 bitで、サンプリング周波数は2倍または4倍を選べます。

| 入力のサンプリング周波数 | 2倍（初期設定） | 4倍 |
| --- | --- | --- |
| 44.1 kHz | 88.2 kHz／24 bit | 176.4 kHz／24 bit |
| 48 kHz | 96 kHz／24 bit | 192 kHz／24 bit |

表に示した形式は、BridgeがDirectSoundへ渡す形式です。Windows共有ミキサーは必要に応じて、出力デバイスの既定の形式へ変換します。

## 起動する

1. 配布ZIPを展開し、同梱ファイルを同じフォルダーに置きます。
2. `MC HiRes Bridge.exe` をダブルクリックします。
3. Music Centerで **DirectSound（共有）** を選びます。再生操作部に `HR-BR RDY` が表示されたら準備完了です。

Music Centerが起動済みの場合は、次に曲を選び直したときからBridgeが適用されます。普段の起動にはこのEXEまたはそのショートカットを使ってください。更新するときはMusic Centerを完全に終了してから、展開したファイルを入れ替えてください。

## 倍率を変える・出力を確認する

- **倍率の変更** — `HR-BR` を右クリックし、「DSEE HX 出力倍率」から2倍または4倍を選びます。選択は再起動後も保持されます。メニューは領域外クリックで閉じられます。サブメニューでEscを押すと、Music Centerが一覧へ戻る場合があります。
- **適用のタイミング** — 倍率やDSEE HXのオン／オフを再生中に変えても、現在の曲はそのまま再生されます。変更は次に新しく開く曲から反映されます。すでに先読み済みの曲は準備済みの設定で再生されます。
- **出力の確認** — `HR-BR` にマウスを置くと、現在の入力形式、処理経路、DirectSoundに渡している形式が表示されます。Music CenterのDSEE HXボタンは次に適用する設定を示すため、切り替え直後はツールチップと表示が異なることがあります。

| 表示 | 意味 |
| --- | --- |
| `HR-BR RDY` | Bridgeの起動準備ができています |
| `HR-BR ON` | ハイレゾFLACの形式維持、またはDSEE HX経由で再生しています |
| `HR-BR OFF` | 対象外の音源や出力モードなどで、Bridgeの音声処理が適用されていません |

## 対応環境

Windows 11と、標準の場所にインストールしたMusic Center for PC 2.7.4（32 bit）で確認しています。DSEE HXの対象は44.1／48 kHzのステレオ音源です。実機では44.1 kHzのWAV・FLAC・ALAC・AAC・MP3と、48 kHzのFLACを確認しています。ハイレゾWAVとDSDの原音形式維持は対象外です。

起動時にエラーが出る場合は、Music Centerを完全に終了し、ZIPを展開したフォルダーにEXEと `flac24_override.dll` が揃っていることを確認してから起動してください。

不具合は [GitHub Issues](https://github.com/yshdsnd/mc-hires-bridge/issues) へ。開発・診断資料は [GitHubリポジトリ](https://github.com/yshdsnd/mc-hires-bridge) にあります。

## ライセンス

Bridgeは同梱の `LICENSE`（BSD 2-Clause）、MinHookは `licenses/MinHook.txt` を参照してください。
