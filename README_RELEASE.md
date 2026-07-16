# Gaussian Scene Workbench Release Guide / 发行版说明 / リリース案内

## License / 许可证 / ライセンス

Gaussian Scene Workbench is intended for open-source research, learning, and evaluation. Original project code is MIT licensed; bundled third-party components retain their own licenses. Gaussian Splatting components are limited to non-commercial research and evaluation unless separately licensed by their original owners.

Gaussian Scene Workbench 用于开源研究、学习和评估。项目原创代码采用 MIT 许可证；内置第三方组件继续遵循各自许可证。Gaussian Splatting 组件除非另行获得原始权利方许可，否则仅限非商业研究和评估用途。

Gaussian Scene Workbench は、オープンソースの研究、学習、評価用途向けです。独自コードは MIT License、同梱する第三者コンポーネントは各ライセンスに従います。Gaussian Splatting コンポーネントは、別途許可がない限り非商用の研究・評価用途に制限されます。

## English

### First Run On Windows

1. Extract `Gaussian-Scene-Workbench-0.2.6-win-x64.zip` to a writable folder, preferably outside the system drive.
2. Run `Setup Gaussian Scene Workbench.cmd` and choose a runtime install folder. Press Enter to use the same-drive default, for example `E:\Gaussian-Scene-Workbench-Runtime`.
3. Setup starts `Gaussian Scene Workbench.exe` after validation.

Run `Check Gaussian Scene Workbench Environment.cmd` to inspect the local environment without installing components.

The setup detects compatible existing installations, including same-drive Miniforge/Conda roots, the `gaussian_splatting` environment, and legacy `3DGS-Editor-Runtime` locations. It installs or repairs only missing components such as Git, Visual Studio C++ Build Tools, COLMAP, FFmpeg, Python/CUDA packages, and Node helper dependencies.

Windows, driver, or vendor installers may still display UAC, security, or reboot prompts.

### Train From Metashape Cameras

Export an undistorted COLMAP text project from Metashape with `images/` and `sparse/0/cameras.txt`, `images.txt`, and `points3D.txt`. In **Train**, enter a new dataset name, select **Metashape Cameras**, choose the project folder, then use **Train Existing**. The imported camera poses, dimensions, sparse points, and world-coordinate scale are preserved; COLMAP realignment is skipped.

### Build A Package

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1
```

Output: `release\Gaussian-Scene-Workbench-<version>-win-x64.zip`

Use `-IncludeAdvancedMeshBackends` to include local SuGaR, GS2Mesh, and OpenMVS folders.

## 中文

### Windows 首次运行

1. 将 `Gaussian-Scene-Workbench-0.2.6-win-x64.zip` 解压到普通可写目录，建议使用非系统盘。
2. 运行 `Setup Gaussian Scene Workbench.cmd` 并选择运行时安装目录。直接回车会使用同盘默认目录，例如 `E:\Gaussian-Scene-Workbench-Runtime`。
3. 环境验证通过后，安装程序会启动 `Gaussian Scene Workbench.exe`。

只检查本机环境时，运行 `Check Gaussian Scene Workbench Environment.cmd`，不会安装组件。

安装程序会识别同盘 Miniforge/Conda、已有 `gaussian_splatting` 环境以及旧版 `3DGS-Editor-Runtime`，只安装或修复缺失的 Git、Visual Studio C++ Build Tools、COLMAP、FFmpeg、Python/CUDA 包和 Node 辅助依赖。

Windows、驱动或厂商安装器仍可能显示 UAC、安全确认或重启提示。

### 使用 Metashape 相机训练

从 Metashape 导出已去畸变的 COLMAP 文本工程，目录应包含 `images/` 以及 `sparse/0/cameras.txt`、`images.txt`、`points3D.txt`。在**训练**栏输入新的数据集名称，点击 **Metashape 相机**并选择工程目录，然后使用**训练已有数据**。程序会保留相机位姿、图像尺寸、稀疏点和世界坐标尺度，并跳过 COLMAP 重新对齐。

### 构建发行包

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1
```

输出：`release\Gaussian-Scene-Workbench-<version>-win-x64.zip`

使用 `-IncludeAdvancedMeshBackends` 可包含本机 SuGaR、GS2Mesh 和 OpenMVS。

## 日本語

### Windows での初回起動

1. `Gaussian-Scene-Workbench-0.2.6-win-x64.zip` を、システムドライブ以外の書き込み可能なフォルダに展開します。
2. `Setup Gaussian Scene Workbench.cmd` を実行してランタイム先を選びます。Enter を押すと、同じドライブ上の `E:\Gaussian-Scene-Workbench-Runtime` などが既定値になります。
3. 環境確認後、`Gaussian Scene Workbench.exe` が起動します。

インストールせずに確認する場合は、`Check Gaussian Scene Workbench Environment.cmd` を実行します。

セットアップは同じドライブの Miniforge/Conda、既存の `gaussian_splatting` 環境、旧 `3DGS-Editor-Runtime` を検出し、不足している Git、Visual Studio C++ Build Tools、COLMAP、FFmpeg、Python/CUDA パッケージ、Node 依存関係のみを修復します。

Windows、ドライバ、ベンダーのインストーラにより、UAC、セキュリティ、再起動の確認が表示される場合があります。

### Metashape カメラから学習

Metashape から、`images/` と `sparse/0/cameras.txt`、`images.txt`、`points3D.txt` を含む歪み補正済み COLMAP テキストプロジェクトを出力します。**学習**欄で新しいデータセット名を入力し、**Metashape カメラ**からプロジェクトフォルダを選び、**既存データ学習**を実行します。カメラ姿勢、画像寸法、疎点群、ワールド座標スケールを保持し、COLMAP の再整列は実行しません。

### パッケージ作成

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1
```

出力：`release\Gaussian-Scene-Workbench-<version>-win-x64.zip`
