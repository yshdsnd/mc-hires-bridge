# 状態表示

`flac24_override.dll`をロードすると、DSEE HXの左側に小さな文字を表示する。本体のHTMLやインストール済みファイルは変更せず、Music Centerを所有ウィンドウとする、フォーカスを取らない文字表示を重ねる。ボタン・枠・通知音は追加しない。

| 表示 | 意味 |
| --- | --- |
| `HR-BR RDY` | フック初期化済み。読み込み後の再生開始を待っている |
| `HR-BR ON` | 今回のsourceへ24bit設定を適用し、DirectSoundレンダラーの48kHzまたは96kHz/24bit/2ch接続と再生開始の受理を確認 |
| `HR-BR OFF` | 対象外音源、排他などの別出力モード、適用失敗、または条件を確認できない |

一時停止中は、その再生グラフの適用状態を保持する。ONはDACの測定やビットパーフェクトの保証を意味しない。RDYは、ロード前にすでに再生していた曲への適用を保証する表示ではない。

## 判定

16→24の変更は、既存の確認済みバイナリ・呼び出し元・48kHzまたは96kHz/24bit/stereo FLAC条件に加え、保存されたMusic Centerの出力モードがDirectSound（`default`）の場合に限定する。設定ファイルは読み取り専用。読み取り不能・対象プロパティ不明・別モードの場合は介入しない。44.1kHz/16bit FLACでは16bit指定をそのまま渡す。

表示はIMediaControl/IMediaFilterのRunが返った時点でグラフを調べて更新する。次の曲のsourceを先読みしても現在のsourceの適用記録を消さないよう、sourceごとに記録する。出力モードを変更した際は保存設定の再確認でもOFFへ更新する（最大約1秒＋設定保存までの時間）。

表示はウィンドウ位置・サイズ・DPIに追従し、最小化中は隠す。Music Center 2.7.4のヘッダー位置を基準とした配置であり、将来の画面変更や全DPI・テーマを保証するものではない。長い曲名に重なる場合がある。アプリを完全終了すると文字表示とフックが解除される。

## 使い方

ビルド・ロード方法は従来と同じ。追加のUI用プログラムは不要。

```powershell
cmake -S . -B build-Win32 -A Win32
cmake --build build-Win32 --config Release --target flac24_override inject
$targetPid = 1234 # 再起動後の実際のMusic Center PID
& .\build-Win32\Release\inject.exe $targetPid (Resolve-Path .\build-Win32\Release\flac24_override.dll).Path
```

**現在の状態表示付き版はRunもフックするため、`dshow_probe`、`dshow_bypass`、`dshow_bypass_timing`、`negotiation_probe`を同じプロセスに追加しない。** 過去の調査報告にある観測用併用構成は、その時点のUIなし版での検証記録。

## 検証

2026-11-01、Windows 11 / Music Center 2.7.4 x86で以下を実施。

- ロード後にDSEE HX左側へ`HR-BR RDY`を実表示。
- 96kHz/24bit FLAC再生でSET_24とSTATUS_ONを記録し、`HR-BR ON`へ切り替わることを実画面で確認。
- 既存ライブラリーの44.1kHz/16bit FLACへ切り替え、`HR-BR OFF`を実表示。PASS_THROUGH requested=16 applied=16とSTATUS_OFFを記録。音源の形式はFLAC STREAMINFOでも確認。
- 対象の96kHz/24bit FLACへ戻すと再びONへ復帰することを確認。
- 通常サイズと最大化で、DSEE HX左側の位置へ追従することを確認。
- Win32ビルド成功。出力モード設定のパーサーはDirectSound・WASAPI・ASIO・不明値・欠落・重複・途中切れなど10ケースで検証。

パーサーの回帰確認：

```powershell
cmake --build build-Win32 --config Release --target bridge_status_tests
.\build-Win32\Release\bridge_status_tests.exe
```

個人の実測ログ・音源情報・スクリーンショットは公開リポジトリに含めない。

48kHz対応追加後の実再生・PCM・曲間の検証は[48kHz/24bitの記録](shared-48k-24bit.md)を参照。
