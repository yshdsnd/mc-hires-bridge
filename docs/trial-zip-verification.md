# 試用ZIPの内容と起動手順の事前検証 (Trial ZIP Verification Protocol)

## 1. 概要と目的

本ドキュメントは、Issue #21（曲末保持の正式反映）工程4に向けた準備として、開発ブランチから作成される「試用ZIP」の内容検証手順、混入検出機構、および実機（Music Center for PC 2.7.4 x86）での起動検証プロトコルを定めます（Issue #32）。

通常版への統合・正式リリースは実機再生検証の合格後に判断し、実機環境（Windows 11 / Music Center for PC 2.7.4 x86 / DirectSound共有出力）での再生確認は Codex が担当します。Antigravity は、パッケージ内容の自動検証、混入検出機構、および手順書の整備を提供します。

---

## 2. 試用ZIPの作成手順

試用ZIPは、ローカル環境または GitHub Actions ワークフロー（開発ブランチプッシュ時または手動 dispatch）から生成可能です。

### 2.1 ローカルでのビルドと試用ZIP作成
Visual Studio 2022 (Win32) 環境にて以下を実行します：

```powershell
# 1. ビルド設定とターゲット生成
cmake -S . -B build-Win32 -A Win32 -DMC_BRIDGE_BUILD_DIAGNOSTICS=ON -DMC_BRIDGE_BUILD_EXPERIMENTS=ON
cmake --build build-Win32 --config Release --target mc_hires_bridge bridge_status_tests package_manifest_tests

# 2. 単体テストの実行
.\build-Win32\Release\bridge_status_tests.exe
.\build-Win32\Release\package_manifest_tests.exe

# 3. パッケージ検証自己テスト（混入検出ロジック確認）
powershell -ExecutionPolicy Bypass -File .\tools\verify-package.ps1 -SelfTest

# 4. 試用ZIPの生成・検証（dist/ に出力）
powershell -ExecutionPolicy Bypass -File .\tools\verify-package.ps1 -BuildDir build-Win32 -CreateZip -OutputDir dist
```

生成される成果物：
- `dist/mc-hires-bridge-windows-x86.zip`
- `dist/mc-hires-bridge-windows-x86.zip.sha256`

### 2.2 GitHub Actions による開発ブランチ試用ZIP生成
`.github/workflows/build-windows.yml` は `push.branches: [main, 'dev/**']` および `workflow_dispatch` に対応しています。
1. 開発ブランチ（`dev/*`）に push すると、CI が自動で `mc_hires_bridge`, `flac24_override.dll`, テストを実行。
2. `tools/verify-package.ps1 -SelfTest` および `-CreateZip` による検証を経て、`mc-hires-bridge-windows-x86.zip` を Artifact としてアップロード（保持期間 30 日）。
3. GitHub Actions の実行サマリーから直接ダウンロード可能です。

---

## 3. 配布ZIP内容の仕様と混入検出機構

### 3.1 許可ファイル（ホワイトリスト：厳格に以下の5点のみ）

| ファイルパス | 種別 | 要件・検証基準 |
|:---|:---:|:---|
| `MC HiRes Bridge.exe` | Win32 GUI 実行ファイル | x86 (0x014C / PE32)、GUI サブシステム (2)、非DLL、署名/リソース内包 |
| `flac24_override.dll` | Win32 共有ライブラリ | x86 (0x014C / PE32)、DLL 特性フラグ (0x2000)、MinHook 内包 |
| `README.md` | 使用説明書 | UTF-8、`docs/package-readme.md` より配置、非空、使用法・対応範囲記載 |
| `LICENSE` | プロジェクトライセンス | UTF-8、BSD 2-Clause License |
| `licenses/MinHook.txt` | 依存ライブラリライセンス | UTF-8、MinHook ライセンス文面 |

### 3.2 混入禁止ファイル（ブラックリストと検出規則）

もしビルド設定の誤り等により以下のファイルが混入した場合、`tools/verify-package.ps1` および `tests/package_manifest_tests.exe` が直ちに異常を検出し、**終了コード 1** で停止します：

1. **診断DLL (Diagnostic DLLs)**:
   - パターン: `*probe*.dll`, `*tap*.dll`, `wasapi_*.dll`
   - 具体例: `probe.dll`, `dshow_probe.dll`, `wasapi_pcm_probe.dll`, `end_probe.dll`, `pcm_tap.dll`, `position_probe.dll`, `negotiation_probe.dll`
2. **実験DLL (Experimental DLLs)**:
   - パターン: `*bypass*.dll`, `*legacy*.dll`, `*experiment*.dll`, `*wav*.dll`
   - 具体例: `legacy_16bit_override.dll`, `dshow_bypass.dll`
3. **テスト・観測実行ファイル (Test / Observation Executables)**:
   - パターン: `MC HiRes Bridge.exe` 以外のすべての `*.exe`
   - 具体例: `bridge_status_tests.exe`, `package_manifest_tests.exe`, `eos_transition_tests.exe`, `smoke.exe`, `dshow_smoke.exe`, `inject.exe`, `track_regression_tool.exe`
4. **ビルド成果物・デバッグシンボル**:
   - パターン: `*.pdb`, `*.ilk`, `*.exp`, `*.lib`, `*.obj`
5. **ソースコード・生音声データ**:
   - パターン: `*.cpp`, `*.h`, `*.inl`, `*.pcm`, `*.flac`, `*.wav`, `*.log`

---

## 4. 混入検出機構の事前検証結果（Self-Test Suite）

`tools/verify-package.ps1 -SelfTest` により、正常パッケージの合致および各種混入シナリオの自動検出を実証しました：

```
=== MC HiRes Bridge Package Verification Self-Test Suite (Issue #32) ===

| Case ID | Scenario | Expected | Actual | Verdict | First Error Detected |
|:---|:---|:---:|:---:|:---:|:---|
| ST-01 | Clean standard package | PASS | PASS | **PASS** | - |
| ST-02 | Diagnostic DLL contamination (probe.dll) | FAIL (Detected) | FAIL (Detected) | **PASS** | Unexpected file in package (not on whitelist): 'probe.dll' |
| ST-03 | Diagnostic DLL contamination (dshow_probe.dll) | FAIL (Detected) | FAIL (Detected) | **PASS** | Unexpected file in package (not on whitelist): 'dshow_probe.dll' |
| ST-04 | Diagnostic DLL contamination (wasapi_pcm_probe.dll) | FAIL (Detected) | FAIL (Detected) | **PASS** | Unexpected file in package (not on whitelist): 'wasapi_pcm_probe.dll' |
| ST-05 | Experimental DLL contamination (legacy_16bit_override.dll) | FAIL (Detected) | FAIL (Detected) | **PASS** | Unexpected file in package (not on whitelist): 'legacy_16bit_override.dll' |
| ST-06 | Test executable contamination (eos_transition_tests.exe) | FAIL (Detected) | FAIL (Detected) | **PASS** | Unexpected file in package (not on whitelist): 'eos_transition_tests.exe' |
| ST-07 | Observation tool contamination (track_regression_tool.exe) | FAIL (Detected) | FAIL (Detected) | **PASS** | Unexpected file in package (not on whitelist): 'track_regression_tool.exe' |
| ST-08 | Missing primary DLL (flac24_override.dll) | FAIL (Detected) | FAIL (Detected) | **PASS** | Missing required file: 'flac24_override.dll' |
| ST-09 | Missing MinHook license file | FAIL (Detected) | FAIL (Detected) | **PASS** | Missing required file: 'licenses/MinHook.txt' |
| ST-10 | Debug symbol contamination (flac24_override.pdb) | FAIL (Detected) | FAIL (Detected) | **PASS** | Unexpected file in package (not on whitelist): 'flac24_override.pdb' |
| ST-11 | Raw audio / private file contamination (dump.pcm) | FAIL (Detected) | FAIL (Detected) | **PASS** | Unexpected file in package (not on whitelist): 'dump.pcm' |

[PASS] All 11 Self-Test cases PASSED! Detection logic verified.
```

---

## 5. Codex 実機再生検証向け起動・確認手順

実機検証（Codex 担当）を行う際は、以下の手順に従って試用ZIPを展開・起動・確認します。

### 5.1 事前確認（クリーン環境の担保）
1. 実行中の `MusicCenter.exe` があれば完全に終了します。
2. 以前の観測用DLL（`dshow_probe.dll`, `end_probe.dll` 等）がシステムディレクトリやプロセスに残っていないことを確認します（観測DLLと通常DLLの同時ロードは AGENTS.md により厳禁）。
3. Windows のサウンド設定（DirectSound共有出力）が対象音源のサンプルレート（48kHz / 96kHz / 192kHz、24bit）に対応していることを確認します。

### 5.2 展開と起動確認
1. `mc-hires-bridge-windows-x86.zip` を任意の作業フォルダー（例: `Desktop\MC_HiRes_Bridge_Trial\`）へ展開します。
2. フォルダー内に `MC HiRes Bridge.exe` と `flac24_override.dll` が同一階層に存在することを確認します。
3. `MC HiRes Bridge.exe` を通常のユーザー権限でダブルクリックして実行します。
   - コンソールウィンドウや成功ダイアログは表示されません。
   - Music Center for PC が起動し、メインウィンドウのタイトル/ヘッダー右上に **`HR-BR RDY`** と表示されることを確認します。
   - ランチャープロセス自体は注入完了後に終了コード 0 で正常終了します。

### 5.3 再生ステータスの表示確認
1. **ハイレゾFLAC再生時**:
   - 24-bit / 48kHz, 96kHz, または 192kHz のステレオFLACを再生。
   - 表示が **`HR-BR ON`** に切り替わることを確認。
2. **通常音源再生時**:
   - 16-bit / 44.1kHz のFLACまたは曲送り対象外音源を再生。
   - 表示が **`HR-BR OFF`** に切り替わることを確認。
3. **曲末保持・曲送り動作の確認**:
   - `24 -> 16 bit` 曲間遷移時に、前曲末尾（約300ms）の音が途切れず滑らかに再生完了した後に次曲が始まることを聴感およびログで確認。
   - 同一フォーマット（`96k/24b -> 96k/24b`）の先読み時に無音ギャップが生じないことを確認。
   - 曲末尾保留中に「一時停止」「シーク」「曲送り」「停止」を行った際、即座に応答しフリーズやノイズが発生しないことを確認。

### 5.4 終了確認
1. Music Center for PC を終了します。
2. プロセス終了に伴いフックが安全に解除され、クラッシュやエラーダイアログが生じないことを確認します。

---

## 6. エラー対応ガイド

| 症状 / エラー | 原因 | 対処方法 |
|:---|:---|:---|
| 「DLLが見つからない」エラー表示 | `flac24_override.dll` が EXE と別フォルダーにある | 同一フォルダーへ配置して再実行する。 |
| 「旧ブリッジの初期化完了を確認できない」 | 前回のフックがプロセスに残存している | タスクマネージャーで `MusicCenter.exe` を完全に終了してから起動する。 |
| 「起動先が見つからない」 | 標準インストール先（`Program Files (x86)\Sony\Music Center`）に存在しない | インストール先パスを確認する。 |
| `HR-BR RDY` が表示されない | DLL注入またはフック初期化の失敗 | 管理者権限で起動していないか、アンチウイルス等の干渉がないか確認する。 |
