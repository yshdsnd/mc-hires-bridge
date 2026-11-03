# 24bit FLACが16bitになる最初の呼び出し

調査日：2026-11-01。対象：Windows 11 / Music Center for PC 2.7.4 x86、DirectSound経路。対応するIssueは[#1](https://github.com/yshdsnd/mc-hires-bridge/issues/1)。

## 結論

`OpcFlac.dll`がFLAC sourceのSony独自インターフェースを呼び、値`16`を指定している。その呼び出しの直前には96kHz/24bitだった出力候補が、成功して戻った直後には96kHz/16bitへ変わる。これはDSEEへの最初の接続より前に起きる。

したがって、今回の環境では「DirectSoundやWindowsが24bitを16bitへ落としている」「DSEEが24bitの接続を拒否した後で16bitに再試行した」という説明ではない。Music CenterのFLAC再生モジュールによる事前の出力設定を捕捉できた。

## 呼び出しの識別

| 項目 | 実測 |
| --- | --- |
| インターフェースIID | `{1609A1C0-E0E7-4CEF-AA37-31CD15BE4ECD}` |
| メソッド | vtable slot 7（IUnknownを含め0起点。公式メソッド名は不明） |
| 実装位置 | `OmgFlacSrc.ax + 0x1860` |
| 呼び出し元の戻り先 | `OpcFlac.dll + 0x32A85` |
| 引数 | DWORD相当の値`16` |
| 戻り値 | `S_OK` |

対象x86実装の読み取り専用の逆アセンブルで、メソッドがthisと1個の32bit値を取るstdcall形式（ret 8）であることを確認してから観測した。呼び出し元もvtableの`+0x1C`、つまりslot 7を呼ぶことと一致した。メソッド名を推測でSetBitDepthなどと確定させてはいないが、形式変更という実測結果からビット深度を指定する機能と判断できる。

## 時系列

以下は1曲目の観測を要約したもの。時刻はUTCで、日本時間は9時間後。

| 時刻 | 呼び出し・状態 |
| --- | --- |
| 01:03:47.849 | `IFileSourceFilter::Load`、形式引数はNULL、結果S_OK |
| 同上、Load直後 | source出力候補は96,000Hz / 24bit / 2ch、EXTENSIBLE、valid bits 24、block align 6 |
| 01:03:47.854 | 上記Sony独自slot 7を`OpcFlac.dll`から値16で呼び出し |
| 同上、呼び出し直前 | 同じsource出力ピンの候補は96kHz/24bit |
| 同上、S_OKで戻った直後 | 同じピンの候補は96kHz/16bit、通常PCM、block align 4 |
| 01:03:47.855以降 | source→DSEEの`IPin::Connect`。形式引数はNULL。sourceが16bitを列挙し、DSEEの`ReceiveConnection`がS_OKで受け入れる |

別の96kHz/24bit FLACを次曲として開いた際にも、Load後24bit→slot 7へ16指定→戻った直後16bit、という同じ順序を再現した。2曲のFLAC検証であり、他のファイル形式・全出力モードへ一般化するものではない。

## 監視の実装

`diagnostics/negotiation_probe.cpp`、CMakeターゲット`negotiation_probe`を追加した。音声経路の変更、形式引数の書き換え、独自setterの試験呼び出しは行わず、元の呼び出しへ同じ引数を渡し、同じ結果を返す。

- `IFileSourceFilter::Load`の引数と、終了直後の出力候補。
- `IPin::Connect`、`ReceiveConnection`、`QueryAccept`、`EnumMediaTypes`、`IEnumMediaTypes::Next`。
- `IFilterGraph::ConnectDirect`、Sony filterのQueryInterface。
- 確認したSony FLAC独自slot 4/7の値、および呼び出し前後の出力候補。今回実再生で捕捉したsetterはslot 7。
- PID、TID、UTC時刻、連番、親呼び出し、モジュール名と相対アドレスのスタック。

候補を観測する追加のEnumMediaTypes/Nextは監視自身の呼び出しとして扱い、元のアプリによる列挙と区別する。ディスクへのログ出力と候補の列挙によりタイミングへの影響はあり得る。

独自メソッドのフックはx86の確認済みPEタイムスタンプとslot 4/7の相対アドレスに限定し、合わない実装ではスキップする。これは汎用のSony独自API対応ではない。

## 再現方法

```powershell
cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_DIAGNOSTICS=ON
cmake --build build-Win32 --config Release --target negotiation_probe inject
# Music Centerを完全終了して再起動し、初回再生前に実際のPIDを指定
$targetPid = 1234
$dll = (Resolve-Path .\build-Win32\Release\negotiation_probe.dll).Path
& .\build-Win32\Release\inject.exe $targetPid $dll
```

`%TEMP%\NegotiationProbe-PID.tsv`の`READY`を確認してから再生する。`AFTER_LOAD_PREFERRED`、`SCALAR_REQUEST`、`BEFORE_SCALAR_PREFERRED`、`AFTER_SCALAR_PREFERRED`、`CONNECTED`を時系列で確認する。setupのslot 4がimplementation 0、slot 7がimplementation 1に対応する。

既存の観測版・迂回版とは同時にロードしない。DLLの切り替え・解除はアプリの完全終了と再起動で行う。ログが既に存在するPIDでは上書きを避けて初期化を中止する。今回のログは音源名・フルパス・PCMを記録しないが、公開前には内容を確認すること。

## 検証と対象バイナリ

- Win32ビルド成功。観測DLLを実アプリへ適用し、2曲で同じ変更を捕捉。元の再生と経過時間更新も確認した。
- その後の最終ソースでは、列挙API失敗時に未初期化の出力ポインターをログで読まない修正を追加し、ビルド確認した。成功した交渉の観測処理は同じ。
- SHA-256（照合用。対象バイナリ本体や逆アセンブル結果は同梱しない）：
  - `OmgFlacSrc.ax`: `390C192A55E639001A65630228C21D748AE011F52773085BBF2F13B8A6951678`
  - `OpcFlac.dll`: `20E25274228DB9F460990E259815171545902433A0F19EA717B27843D9863931`

## 残る課題

値16を指定する呼び出しと効果は特定できたが、OpcFlac内部の形式選択の分岐条件や、その設計上の理由までは確定していない。常に16を固定値で指定するとは主張しない。

24bit化は未実施。次はこの事前設定を変えた際のsource出力と、24bitの可聴経路およびSonyの位置管理用経路が両立するかを検証する必要がある。現行のInfinite Pin Teeは両方の枝が同じ形式を使うため、DSEE側の受け入れ条件も別途確認が必要。
