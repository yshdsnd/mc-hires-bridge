# MC HiRes Bridge

**MC HiRes Bridgeは、Sonyとは関係のない非公式ツールです。独自の調査結果に基づいて開発しており、Sonyが提供・承認・サポートするものではありません。**

Music Center for PCのDirectSound共有出力で、対応するハイレゾFLACの24bit出力を維持する非公式ツールです。

## 使い方

1. ZIPをフォルダーごと展開します。
2. `MC HiRes Bridge.exe`をダブルクリックします。
3. Music Centerが起動し、`HR-BR RDY`と表示されたら準備完了です。

確認画面はありません。対応音源の再生時は`HR-BR ON`、対象外の音源や別の出力モードでは`HR-BR OFF`になります。

EXEと`flac24_override.dll`は同じフォルダーのまま使用してください。EXEのショートカットからも起動できます。Music Centerの終了でブリッジも解除されるため、以後はこのEXEから起動してください。

Music Centerが起動済みなら、そのプロセスへロードします。ロード前から再生中の曲へは遡って適用しません。次曲または曲を選び直したときから適用されます。更新時はMusic Centerを完全終了してからファイルを入れ替えてください。

HR-BRにマウスを置くと、取得できた入力形式、処理経路、DirectSoundの接続形式を表示します。DirectSound欄はWindows共有ミキサーに入る前の形式です。

## 対応範囲・エラー

Windows 11、Music Center for PC 2.7.4（32bit、標準インストール先）、DirectSound共有出力。48・96・192 kHz／24 bitのステレオFLACが対象です。DSDとハイレゾWAVは対象外です。

音源のサンプルレートをWindows共有出力まで維持するには、Windows側のオーディオデバイス設定と機器もそのレートに対応している必要があります。

通常の権限で起動してください。失敗したときだけ理由とエラー番号を表示します。旧版がロード済みの場合はMusic Centerを完全終了して再実行してください。Music Center本体や音源ファイルは変更しません。

## DSEE HXの試聴（開発版）

Music CenterでDirectSound出力を選び、DSEE HXをオンにしてから曲を再生してください。試聴した範囲は44.1 kHzのステレオ音源で、16 bit WAV／FLAC／ALAC、96／256 kbps AAC、96／192／320 kbps MP3です。設定の切り替えは、曲を選び直すなどして再生経路が作り直されたときに反映されます。EQを外した経路を使用します。

DSEEをオフにするとCD品質・圧縮音源は通常経路で再生されます。ハイレゾFLACの24 bit処理はDSEE設定に関係なく有効です。

### 出力倍率の切り替え（右クリックメニュー）
`HR-BR` バッジを右クリックすると、DSEE HX出力倍率メニューが表示されます。
- **2倍 (88.2 / 96 kHz)**: 既定値。
- **4倍 (176.4 / 192 kHz)**: 176.4 / 192 kHzへアップスケーリング。

選択内容は `%APPDATA%\Sony\Music Center\config\mc_hires_bridge.json` に自動保存されます。再生中の音声は中断せず、次に新しく開く曲から適用されます（次曲が先読みされている場合はその次の曲から適用）。4倍接続が失敗した場合は自動的に通常経路へフォールバックします。

## ライセンス・詳細

独自部分は同梱の`LICENSE`（BSD 2-Clause）、MinHookは`licenses/MinHook.txt`を参照してください。

ソース・詳細・不具合報告：https://github.com/yshdsnd/mc-hires-bridge
