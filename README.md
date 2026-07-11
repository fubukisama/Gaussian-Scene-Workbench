# Gaussian Scene Workbench

**Gaussian Scene Workbench** is a Windows desktop research environment for Gaussian scene reconstruction, experiment management, analysis, editing, rendering, and export. It supports workflows beyond 3DGS training while retaining compatibility with existing 3DGS Editor projects and runtime settings.

中文名：**高斯场景研究工作台**
日本語名：**ガウスシーン研究ワークベンチ**

## Native 0.3 Development

The application is being rebuilt as a fully native Qt 6/C++ desktop program on the [`agent/native-desktop-0.3`](https://github.com/fubukisama/Gaussian-Scene-Workbench/tree/agent/native-desktop-0.3) branch. This target does not embed HTML, a browser engine, Electron, Node.js, or a local web server.

- Current version: `0.3.0-native-preview`
- Updated: `2026-07-11`
- Current native slice: full-source rectangle/lasso selection, delete undo/redo, and lossless cropped PLY export.
- Windows builds: the [Native Windows workflow](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/workflows/native-windows.yml) publishes a downloadable artifact for each successful branch update.
- Architecture and parity plan: [docs/NATIVE_MIGRATION.md](docs/NATIVE_MIGRATION.md)
- Local build instructions: [native/README.md](native/README.md)

The released `0.2.1` Electron application remains the stable fallback while the native branch reaches feature parity.

## Capabilities

- Prepare image and video datasets with COLMAP-assisted reconstruction tools.
- Train and manage Gaussian Splatting methods, including 3DGS and 2DGS workflows.
- Resume checkpoints, clone experiments, compare parameters, inspect curves, and review job history.
- Load large Gaussian scenes, inspect cameras, select visible Gaussians on the GPU, crop, and edit scene content.
- Run rendering and PSNR analysis, manage assets, and export splat, GLB, and mesh results.
- Use resizable persistent panels, adaptive UI scaling, and a SIBR-style 60-frame GPU render-performance meter.

## License And Intended Use

This repository is intended for open-source research, learning, and evaluation. Original Gaussian Scene Workbench code is MIT licensed. Bundled third-party components keep their original licenses. In particular, `gaussian-splatting/` and related CUDA modules are governed by the Gaussian-Splatting License and are limited to non-commercial research and evaluation unless separate permission is obtained from the original licensors.

See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Windows Download

- [Gaussian Scene Workbench 0.2.1 Windows x64](https://github.com/fubukisama/Gaussian-Scene-Workbench/releases/tag/v0.2.1)
- Package: `Gaussian-Scene-Workbench-0.2.1-win-x64.zip`
- Verify the package with the matching `.sha256` Release asset.

The large Windows package is distributed through GitHub Releases rather than committed to git.

## Quick Start

1. Download and extract `Gaussian-Scene-Workbench-0.2.1-win-x64.zip` to a writable folder, preferably outside the system drive.
2. Double-click `Setup Gaussian Scene Workbench.cmd` and choose a runtime folder. Press Enter to use the same-drive default, such as `E:\Gaussian-Scene-Workbench-Runtime`.
3. Setup launches the application when it finishes. Later, start it with `Gaussian Scene Workbench.exe`.

To inspect the environment without installing components:

```powershell
.\Check Gaussian Scene Workbench Environment.cmd
```

Existing `GS_EDITOR_*` environment variables, `gaussian_splatting` Conda environments, and legacy `3DGS-Editor-Runtime` installations remain supported.

## Repository Layout

- `crop_editor/` - local web workbench and Python service.
- `resources/app/` - Electron desktop application.
- `native/` - Qt 6/C++ desktop replacement and native tests.
- `docs/NATIVE_MIGRATION.md` - renderer, training, and web-runtime retirement plan.
- `scripts/` - setup, packaging, environment repair, and utilities.
- `training_kit/` - Windows conversion and training helpers.
- `gaussian-splatting/` - bundled training and CUDA extension sources.

Generated folders such as `node_modules`, `datasets`, `output`, `desktop_app`, downloaded COLMAP tools, and packaged Electron binaries are excluded from git.

## Build A Release

```powershell
powershell -ExecutionPolicy Bypass -File scripts\update_version.ps1 -Version 36.10.1 -PackageVersion 0.2.1 -Bump none -Note "SIBR-style GPU performance meter"
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1
```

The generated package is written to `release\Gaussian-Scene-Workbench-<version>-win-x64.zip`.

## Capture Guidance

COLMAP and Gaussian Splatting reconstruction require real camera motion and parallax. For small objects or exhibits, capture at least 50-100 overlapping images while moving around the subject. Rotating the camera from one fixed position often prevents sparse reconstruction initialization.

---

## 中文

**Gaussian Scene Workbench（高斯场景研究工作台）** 是面向 Windows 的高斯场景研究环境，覆盖数据处理、场景重建、训练实验管理、分析评估、可视化编辑、渲染与导出，不再局限于单一的 3DGS 训练或裁剪功能。

### 主要功能

- 使用图像或视频准备数据集，并辅助完成 COLMAP 重建。
- 管理 3DGS、2DGS 等 Gaussian Splatting 训练流程。
- 恢复检查点、克隆实验、对比参数、查看训练曲线和任务历史。
- 加载大型高斯场景，查看相机，使用 GPU 选择可见高斯并进行裁剪与编辑。
- 执行渲染、PSNR 分析、资产管理以及 splat、GLB、网格导出。
- 支持可调整并持久化的面板、高 DPI 自适应界面缩放，以及 SIBR 风格的 60 帧 GPU 渲染性能监控。

### 许可证与用途

本仓库用于开源研究、学习和评估。Gaussian Scene Workbench 的原创代码采用 MIT 许可证，内置第三方组件仍遵循各自原许可证。特别是 `gaussian-splatting/` 及相关 CUDA 模块受 Gaussian-Splatting License 约束，除非另行获得原始权利方授权，否则仅限非商业研究和评估用途。

详情见 [LICENSE](LICENSE) 和 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。

### Windows 下载

- [Gaussian Scene Workbench 0.2.1 Windows x64](https://github.com/fubukisama/Gaussian-Scene-Workbench/releases/tag/v0.2.1)
- 安装包：`Gaussian-Scene-Workbench-0.2.1-win-x64.zip`
- 使用 Release 中对应的 `.sha256` 文件校验安装包。

### 快速开始

1. 下载并解压安装包到普通可写目录，建议放在非系统盘。
2. 双击 `Setup Gaussian Scene Workbench.cmd` 并选择运行时目录。直接回车会使用同盘默认目录，例如 `E:\Gaussian-Scene-Workbench-Runtime`。
3. 安装完成后软件会自动启动。以后双击 `Gaussian Scene Workbench.exe` 即可运行。

只检查环境、不安装组件时运行：

```powershell
.\Check Gaussian Scene Workbench Environment.cmd
```

已有的 `GS_EDITOR_*` 环境变量、`gaussian_splatting` Conda 环境和旧版 `3DGS-Editor-Runtime` 仍然兼容。

### 源码结构与打包

`crop_editor/` 是网页工作台和 Python 服务，`resources/app/` 是 Electron 桌面应用，`scripts/` 与 `training_kit/` 提供环境安装、打包、数据转换和训练辅助工具。

生成发行包：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1
```

生成文件位于 `release\Gaussian-Scene-Workbench-<version>-win-x64.zip`。

### 原生桌面版开发

`0.3.0-native-preview` 已在 `agent/native-desktop-0.3` 分支开始重构。该版本使用 Qt 6/C++，不加载 HTML、Electron、Node.js 或本地网页服务器。GitHub Actions 会在每次推送后自动生成 Windows 预览 artifact；迁移阶段仍保留 `0.2.1` 作为稳定版本。

---

## 日本語

**Gaussian Scene Workbench（ガウスシーン研究ワークベンチ）** は、Windows 向けの Gaussian Splatting 研究環境です。データ準備、シーン再構成、学習実験管理、解析、編集、レンダリング、エクスポートを一つのデスクトップアプリで扱います。

### 主な機能

- 画像・動画データセットの準備と COLMAP 再構成支援。
- 3DGS、2DGS を含む Gaussian Splatting 学習ワークフロー。
- チェックポイント再開、実験の複製、パラメータ比較、学習曲線、ジョブ履歴。
- GPU による可視 Gaussian 選択、クロップ、シーン編集、カメラ確認。
- レンダリング、PSNR 解析、アセット管理、splat・GLB・メッシュ出力。
- 高 DPI 対応 UI スケーリング、状態を保持するパネル、SIBR 方式の 60 フレーム GPU 描画性能表示。

### ライセンス

本リポジトリは、オープンソースの研究、学習、評価用途向けです。Gaussian Scene Workbench 独自コードは MIT License で提供されます。同梱される第三者コンポーネントは各ライセンスに従い、`gaussian-splatting/` と関連 CUDA モジュールは、別途許可がない限り非商用の研究・評価用途に制限されます。

詳細は [LICENSE](LICENSE) と [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) を参照してください。

### Windows 版

- [Gaussian Scene Workbench 0.2.1 Windows x64](https://github.com/fubukisama/Gaussian-Scene-Workbench/releases/tag/v0.2.1)
- パッケージ：`Gaussian-Scene-Workbench-0.2.1-win-x64.zip`

1. パッケージをシステムドライブ以外の書き込み可能なフォルダに展開します。
2. `Setup Gaussian Scene Workbench.cmd` を実行し、ランタイム先を選択します。既定値は同じドライブ上の `Gaussian-Scene-Workbench-Runtime` です。
3. セットアップ後は `Gaussian Scene Workbench.exe` で起動します。

環境確認のみを行う場合：

```powershell
.\Check Gaussian Scene Workbench Environment.cmd
```

既存の `GS_EDITOR_*` 環境変数、`gaussian_splatting` Conda 環境、旧 `3DGS-Editor-Runtime` は引き続き利用できます。

### ネイティブデスクトップ版の開発

`0.3.0-native-preview` は `agent/native-desktop-0.3` ブランチで開発中です。Qt 6/C++ を使用し、HTML、Electron、Node.js、ローカル Web サーバーを実行しません。GitHub Actions は各更新から Windows プレビュー artifact を生成します。機能が揃うまでは `0.2.1` を安定版として維持します。
