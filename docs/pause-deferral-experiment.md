# 異形式曲送り時の Pause 保留・末尾欠落防止試験（Issue #23）

[Issue #22](https://github.com/yshdsnd/mc-hires-bridge/issues/22) の調査結果に基づき、[Issue #23](https://github.com/yshdsnd/mc-hires-bridge/issues/23) にて実装した試験版（`flac24_pause_experiment.dll`）の設計、検証結果、および Music Center 実機での動作確認手順をまとめる。

本試験ターゲットは、実再生での総合検証が完了するまで通常の配布パッケージ（`cmake --install` およびリリース ZIP）からは除外される。

---

## 1. 曲末の早期停止で観測した挙動

Music Center for PC 2.7.4 x86 では、48 kHz/24 bit → 48 kHz/16 bit FLAC の自動曲送りで、23秒の前曲が約20.1秒で途切れた。#22 の診断では、通知3841、Pause、DirectSound停止、位置変更の順を観測した。#23 の24 bit観測モードでも同じ停止順を確認した。

調査時の仮説は、先読み後の早期Pauseと後続の位置変更が、旧経路に残る音声の出力を中断するというもの。#23 の保留モードでは、前曲は23秒の末尾まで出力され、約160 ms後に次曲が始まった。

---


## 2. 試験版アーキテクチャと機能

本試験版（`flac24_pause_experiment.dll`）は、24bit FLAC 再生能力を完全に維持したまま、環境変数による動作モード切り替えと安全な Pause 保留制御を提供する。

### モード切り替え（`MC_BRIDGE_PAUSE_MODE`）

| モード名 | 設定値 | 挙動 |
| :--- | :--- | :--- |
| **観察モード** | `MC_BRIDGE_PAUSE_MODE=observe` | 24bit FLAC 出力を維持し、全イベント（`PAUSE`, `STOP`, `SEEK_SETPOSITIONS`, `DS_EVENT_3841`, `DOWNSTREAM_PCM`, `DOWNSTREAM_EOS`, `DS_STOP`）をログ出力するが、Pause 保留および合成 EOS 送出は行わない。 |
| **実験モード**（デフォルト） | `MC_BRIDGE_PAUSE_MODE=defer` または未設定 | 形式変更曲送り時の `OpcDSPlay.dll+0xDAC9` からの `Pause()` を保留し、EQ への EOS 送出と DirectSound ドレインを待機して末尾欠落を完全に解消する。 |

### 制御シーケンス（実験モード）

```
[上流デコーダ]      [OmgEqxlFilter (EQ)]      [DirectSound]          [OpcDSPlay.dll]
      |                      |                      |                       |
      |-- (最終PCM出力) ---->|                      |                       |
      |-- EndOfStream ------>| (内部保持)           |                       |
      |                      |                      |                       |
   (イベント 3841 発火) -------------------------------------------------->|
      |                      |                      |                       |
      |                      |                      |   [RVA 0xDAC9]        |
      |                      |                      |   IMediaControl::Pause()
      |                      |                      |           |           |
      |               === フックが Pause をインターセプト ===   |           |
      |                      |                      |           |           |
      |                      |<- IPin::EndOfStream() (1回送出)  |           |
      |                      |-- (保持PCMフラッシュ)-->|           |           |
      |                      |                      | (PCM再生) |           |
      |                      |                      |           |           |
      |          === メッセージポンプ待機 (curPos >= stopPos) ==|           |
      |                      |                      |           |           |
      |                      |                      | (全PCM完了)|          |
      |                      |                      |<-- DS_STOP|           |
      |                      |                      |           |           |
      |               === 保留解除: originalControlPause() 実行 ===         |
      |                      |                      |           |           |
      |                      |                      |           |<-- S_OK --|
      |                      |                      |                       |
      |                      |                      |   IMediaSeeking::     |
      |                      |                      |   SetPositions()      |
      |                      |                      |   (Flush & 次曲準備)  |
```

### 安全機構と誤発火防止
1. **呼出元 RVA / PE タイムスタンプ照合**:
   `OpcDSPlay.dll` の PE タイムスタンプ `0x63EB12DD` および RVA `0xDAC9` を厳格に判定する。別バージョンや未検証の呼出元からの Pause は通常処理へそのまま通過させる。
2. **非形式変更・手動操作時の非干渉**:
   - **同形式連続再生（`24bit -> 24bit`）**: `SameFormat24` の場合は Pause 保留を発動せず、シームレス再生を維持。
   - **手動 Pause / シーク**: ユーザーによる一時停止やシーク操作時は保留せず、即座に一時停止・位置変更を実行。
   - **プレイリスト終端**: 次曲なしの場合は通常ドレインまたは自然終了に委ねる。
3. **安全タイムアウト**:
   万一 DirectSound からの再生完了シグナルが得られない場合でも、最大 `4.5 秒` の安全上限で自動解除し、UI ハングを防止。

---

## 3. ヘッドレス検証（`eos_transition_tests`）

本実装に対する全 13 項目の単体・結合テストを実装し、Win32 Release ビルドで全件合格を確認済み。

### ビルドとテスト実行手順
```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_EXPERIMENTS=ON
& $cmake --build build-Win32 --config Release --target eos_transition_tests flac24_pause_experiment
.\build-Win32\Release\eos_transition_tests.exe
```

### テスト項目一覧
- **Test 1〜10**:
  - 非 23 秒任意長・時刻なしサンプル
  - 重複 EOS 抑止保証
  - 同形式 24bit 先読み維持
  - 形式変更・未確定時の EOS 送出
  - Seek / Flush / Stop / TrackChange による状態クリア
  - 複数 DirectShow グラフの同時追跡
  - DirectShow 実 EQ フィルター内部保持フレーム出力
  - COM vtable フックおよび測位フラグ安全性
  - Stop 保留時の `trackTail::analyzeStream` 検証
  - プレイリスト終端ドレイン検証
- **Test 11（Issue #23）: `IMediaControl::Pause` 保留と実ストリーム末尾検証**:
  - `OpcDSPlay.dll+0xDAC9` からの Pause を保留し、EQ へ EOS を送出して 23.000 秒まで PCM が出力されることを `#19` の末尾解析器で確認（`tailComplete == true`, `missingTailFrames == 0`）。
- **Test 12（Issue #23）: 観察モード（`observe`）と実験モード（`defer`）の切り替え検証**:
  - `MC_BRIDGE_PAUSE_MODE=observe` 時は Pause 保留が行われず、`defer` 時のみ保留が有効になることを確認。
- **Test 13（Issue #23）: 非形式変更時の Pause 非保留検証**:
  - 手動 Pause、同形式 24bit 再生中、シーク実行中、16bit グラフ等では Pause が一切保留されないことを確認。

---

## 4. Music Center 実機での動作確認手順（Codex 用）

実機検証を行う際は、以下の手順に従って観察モードと実験モードの比較検証を実施する。

### 1. 試験 DLL の配置
ビルドされた `build-Win32\Release\flac24_pause_experiment.dll` を Music Center の実行環境（またはランチャー環境）へ配置する。

### 2. 観察モードでのログ採取（現状の切断タイミングの確認）
```powershell
$env:MC_BRIDGE_PAUSE_MODE = "observe"
$env:MC_BRIDGE_LOG_PATH = "C:\path\to\observe.log"
# Music Center を起動し、48kHz/24bit -> 48kHz/16bit の自動曲送りを再生
```
**確認ポイント**:
- `PAUSE [OpcDSPlay] caller=OpcDSPlay.dll+0xDAC9` が約 20.1 秒時点で記録されること。
- 直後に `DS_STOP` が発生し、音声が途切れること。

### 3. 実験モードでの再生確認（末尾欠落の解消確認）
```powershell
$env:MC_BRIDGE_PAUSE_MODE = "defer"
$env:MC_BRIDGE_LOG_PATH = "C:\path\to\defer.log"
# Music Center を起動し、48kHz/24bit -> 48kHz/16bit の自動曲送りを再生
```
**確認ポイント**:
- `PAUSE [OpcDSPlay] caller=OpcDSPlay.dll+0xDAC9` に対して `[PauseDeferral] deferring Pause until track end` が記録されること。
- `DOWNSTREAM_EOS delivered to EQ` により EQ から残余フレームが出力されること。
- 直近の `DS_STOP` が 23.0 秒（曲末尾）付近まで遅延され、音が途切れずに最後まで聴こえること。
- 旧曲の再生完了後、スムーズに 48kHz/16bit の次曲再生が開始されること。

### 4. 悪影響（デグレ）検証マトリクス
- **同形式連続再生**: `96kHz/24bit -> 96kHz/24bit` でギャップレス再生が阻害されないこと。
- **手動一時停止**: 再生中に一時停止ボタンを押した際、即座に停止すること。
- **手動シーク**: 再生バーをクリックした際、即座にシーク位置から再生されること。
- **手動曲送り**: 次曲ボタンを押した際、待たされずに即座に次曲へスキップすること。
- **プレイリスト終端**: 最後の曲が末尾まで再生された後、正常に停止状態になること。
