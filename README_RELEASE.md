# 3DGS Editor Release / 发行版说明 / リリース案内

## English

### First Run On A New Windows PC

1. Extract the zip to a normal writable folder, for example `C:\3DGS-Editor`.
2. Double-click `Setup 3DGS Editor.cmd`.
3. After setup finishes, double-click `3DGS Editor.exe`.

If the runtime is incomplete, the app shows a setup dialog before opening the editor.

To check the local environment without installing anything, double-click `Check 3DGS Editor Environment.cmd`.

### Automatic Setup Scope

`Setup 3DGS Editor.cmd` automatically checks and installs or repairs:

- Miniforge in `%USERPROFILE%\miniforge3`
- Conda environment `gaussian_splatting`
- Visual Studio 2022 C++ Build Tools
- Git
- COLMAP, downloaded into `third_party\colmap`
- Video import runtime: OpenCV Python package and `ffmpeg.exe`
- 3DGS Python packages and CUDA extensions
- Crop editor Node helper dependencies

Windows or vendor installers can still show security, UAC, driver, or reboot prompts. The setup cannot bypass those OS-level confirmations.

Setup is repeatable. Installed and working components are skipped automatically; only missing or failed components are installed or repaired.

### Package A New Release

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1
```

The zip is written to `release\3DGS-Editor-<version>-win-x64.zip`.

To include local SuGaR, GS2Mesh, and OpenMVS folders:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1 -IncludeAdvancedMeshBackends
```

## 中文

### 新 Windows 电脑首次运行

1. 把 zip 解压到普通可写目录，例如 `C:\3DGS-Editor`。
2. 双击 `Setup 3DGS Editor.cmd`。
3. 安装完成后，双击 `3DGS Editor.exe`。

如果运行环境不完整，软件会先显示环境安装窗口，而不是直接报错或黑屏。

如果只想检查本机环境、不安装任何东西，双击 `Check 3DGS Editor Environment.cmd`。

### 自动安装范围

`Setup 3DGS Editor.cmd` 会自动检查、安装或修复：

- `%USERPROFILE%\miniforge3` 下的 Miniforge
- Conda 环境 `gaussian_splatting`
- Visual Studio 2022 C++ Build Tools
- Git
- COLMAP，自动下载到 `third_party\colmap`
- 视频导入运行时：OpenCV Python 包和 `ffmpeg.exe`
- 3DGS Python 包与 CUDA 扩展
- 裁剪编辑器所需的 Node 辅助依赖

Windows 或厂商安装器仍可能弹出安全确认、管理员权限、驱动或重启提示。这类系统级确认无法由软件绕过。

Setup 可以反复运行。已经安装并验证通过的组件会自动跳过，只会安装或修复缺失、失败的组件。

### 重新打包发行版

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1
```

生成的 zip 位于 `release\3DGS-Editor-<version>-win-x64.zip`。

如需包含本机 SuGaR、GS2Mesh、OpenMVS：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1 -IncludeAdvancedMeshBackends
```

## 日本語

### 新しい Windows PC での初回起動

1. zip を通常の書き込み可能なフォルダに展開します。例: `C:\3DGS-Editor`
2. `Setup 3DGS Editor.cmd` をダブルクリックします。
3. セットアップ完了後、`3DGS Editor.exe` をダブルクリックします。

実行環境が不足している場合、エディタを開く前にセットアップ画面が表示されます。

インストールせずに環境だけ確認する場合は、`Check 3DGS Editor Environment.cmd` をダブルクリックします。

### 自動セットアップの対象

`Setup 3DGS Editor.cmd` は以下を自動で確認、インストール、修復します。

- `%USERPROFILE%\miniforge3` の Miniforge
- Conda 環境 `gaussian_splatting`
- Visual Studio 2022 C++ Build Tools
- Git
- COLMAP。`third_party\colmap` に自動ダウンロードされます
- 動画読み込み環境: OpenCV Python パッケージと `ffmpeg.exe`
- 3DGS の Python パッケージと CUDA 拡張
- クロップエディタ用の Node 補助依存関係

Windows またはベンダーのインストーラにより、セキュリティ確認、管理者権限、ドライバ、再起動の確認が表示される場合があります。これらの OS レベルの確認はアプリ側では回避できません。

Setup は繰り返し実行できます。インストール済みで正常なコンポーネントは自動でスキップされ、不足または失敗したものだけをインストールまたは修復します。

### リリースパッケージの作成

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1
```

zip は `release\3DGS-Editor-<version>-win-x64.zip` に作成されます。

ローカルの SuGaR、GS2Mesh、OpenMVS も含める場合:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1 -IncludeAdvancedMeshBackends
```
