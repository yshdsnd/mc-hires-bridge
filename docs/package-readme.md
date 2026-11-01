# MC HiRes Bridge

Music Center for PCのDirectSound共有出力で、対応するハイレゾFLACの24bit出力を維持する非公式ツールです。

## 使い方

1. ZIPをフォルダーごと展開します。
2. `MC HiRes Bridge.exe`をダブルクリックします。
3. Music Centerが起動し、`HR-BR RDY`と表示されたら準備完了です。

確認画面はありません。対応音源の再生時は`HR-BR ON`、対象外の音源や別の出力モードでは`HR-BR OFF`になります。

EXEと`flac24_override.dll`は同じフォルダーのまま使用してください。EXEのショートカットからも起動できます。Music Centerの終了でブリッジも解除されるため、以後はこのEXEから起動してください。

Music Centerが起動済みなら、そのプロセスへロードします。ロード前から再生中の曲へは遡って適用しません。次曲または曲を選び直したときから適用されます。更新時はMusic Centerを完全終了してからファイルを入れ替えてください。

## 対応範囲・エラー

Windows 11、Music Center for PC 2.7.4（32bit、標準インストール先）、DirectSound共有出力。48kHzまたは96kHz／24bit／ステレオFLACが対象です。44.1kHz／16bitなどは変更しません。

通常の権限で起動してください。失敗したときだけ理由とエラー番号を表示します。旧版がロード済みの場合はMusic Centerを完全終了して再実行してください。Music Center本体や音源ファイルは変更しません。

## ライセンス・詳細

独自部分は同梱の`LICENSE`（BSD 2-Clause）、MinHookは`licenses/MinHook.txt`を参照してください。

ソース・詳細・不具合報告：https://github.com/yshdsnd/mc-hires-bridge
