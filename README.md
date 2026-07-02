# 3DGS Editor

**Language / 语言 / 言語:** [English](#english) | [中文](#中文) | [日本語](#日本語)

---

## English

Windows desktop editor and training helper for 3D Gaussian Splatting workflows.

The project packages a local Electron desktop shell, a browser-based crop/scene editor, COLMAP conversion helpers, and 3DGS training scripts for Windows users.

### License And Intended Use

This repository is published for open-source research, learning, and evaluation use.

Original 3DGS Editor code is MIT licensed. Bundled third-party components keep their original licenses. In particular, `gaussian-splatting/` and related CUDA modules are governed by the Gaussian-Splatting License and are limited to non-commercial research and evaluation use unless separate permission is obtained from the original licensors.

See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for details.

### Download

The one-click Windows package is published as a GitHub Release asset:

- [3DGS Editor 0.1.2 Windows x64](https://github.com/fubukisama/3DGS-Editor/releases/tag/v0.1.2)
- Asset: `3DGS-Editor-0.1.2-win-x64.zip`
- SHA256: see the `3DGS-Editor-0.1.2-win-x64.zip.sha256` release asset.

The zip is not committed to git because it is larger than GitHub's normal file limit. Use the Release asset for distribution.

### Quick Start

1. Download and extract `3DGS-Editor-0.1.2-win-x64.zip`.
2. Double-click `Setup 3DGS Editor.cmd`.
3. After setup finishes, double-click `3DGS Editor.exe`.

To verify the environment without installing anything, run:

```powershell
.\Check 3DGS Editor Environment.cmd
```

See [README_RELEASE.md](README_RELEASE.md) for English, Chinese, and Japanese release instructions.

### Repository Layout

- `crop_editor/` - local web editor and Python server.
- `resources/app/` - Electron desktop wrapper source.
- `scripts/` - setup, packaging, environment repair, and utility scripts.
- `training_kit/` - Windows batch helpers for conversion and training.
- `gaussian-splatting/` - bundled 3DGS training source and CUDA extension sources.

Generated runtime folders such as `node_modules`, `datasets`, `output`, `desktop_app`, COLMAP downloads, and packaged Electron binaries are intentionally excluded from git.

### Build A Release Package

From a prepared Windows workspace:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\update_version.ps1 -Note "Describe this change"
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1
```

The generated zip is intended to be uploaded to GitHub Releases.

### Version And Multi-Device Workflow

Before every code or package update, pull the latest remote state and update the version manifest:

```powershell
git pull --ff-only
powershell -ExecutionPolicy Bypass -File scripts\update_version.ps1 -Note "Describe this change"
```

This updates `version`, `resources/app/package.json`, and `build_manifest.json` with the source version, package version, update time, machine, branch, and commit. The desktop UI also shows the running version next to the server port, so it is clear which build is open on each device.

### Notes For Capture Data

COLMAP and 3DGS require real camera motion and parallax. Near-duplicate photos from the same viewpoint can match many features but still fail with `No good initial image pair found` because no sparse 3D reconstruction can be initialized.

For small objects or museum exhibits, capture at least 50-100 images while moving around the object with strong overlap. Avoid standing in one place and only rotating the camera.

---

## 中文

3DGS Editor 是面向 Windows 的 3D Gaussian Splatting 桌面编辑器和训练辅助工具。

本项目打包了本地 Electron 桌面外壳、浏览器式裁剪/场景编辑器、COLMAP 转换辅助工具，以及适合 Windows 用户使用的 3DGS 训练脚本。

### 许可证和用途

本仓库用于开源研究、学习和评估用途。

3DGS Editor 的原创代码采用 MIT 许可证。内置第三方组件仍遵循各自原许可证。特别是 `gaussian-splatting/` 及相关 CUDA 模块受 Gaussian-Splatting License 约束，除非另行获得原始权利方授权，否则仅限非商业研究和评估用途。

详情请看 [LICENSE](LICENSE) 和 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。

### 下载

一键 Windows 安装包发布在 GitHub Release：

- [3DGS Editor 0.1.2 Windows x64](https://github.com/fubukisama/3DGS-Editor/releases/tag/v0.1.2)
- 文件：`3DGS-Editor-0.1.2-win-x64.zip`
- SHA256：见 Release 里的 `3DGS-Editor-0.1.2-win-x64.zip.sha256`

zip 安装包没有提交进 git，因为它超过 GitHub 常规文件大小限制。请从 Release 下载发行包。

### 快速开始

1. 下载并解压 `3DGS-Editor-0.1.2-win-x64.zip`。
2. 双击 `Setup 3DGS Editor.cmd`。
3. 安装完成后，双击 `3DGS Editor.exe`。

如果只想检查环境、不安装任何东西，可以运行：

```powershell
.\Check 3DGS Editor Environment.cmd
```

中英日三语发行说明见 [README_RELEASE.md](README_RELEASE.md)。

### 仓库结构

- `crop_editor/` - 本地网页编辑器和 Python 服务。
- `resources/app/` - Electron 桌面外壳源码。
- `scripts/` - 安装、打包、环境修复和工具脚本。
- `training_kit/` - Windows 下的转换和训练批处理辅助工具。
- `gaussian-splatting/` - 内置 3DGS 训练源码和 CUDA 扩展源码。

`node_modules`、`datasets`、`output`、`desktop_app`、COLMAP 下载目录和已打包的 Electron 二进制文件等运行时生成目录不会提交进 git。

### 构建发行包

在准备好的 Windows 工作区中运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\update_version.ps1 -Note "说明本次修改"
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1
```

生成的 zip 用于上传到 GitHub Releases。

### 版本和多设备协作流程

每次修改代码或重新封装安装包前，先拉取远端最新状态，再更新版本记录：

```powershell
git pull --ff-only
powershell -ExecutionPolicy Bypass -File scripts\update_version.ps1 -Note "说明本次修改"
```

这个脚本会同步更新 `version`、`resources/app/package.json` 和 `build_manifest.json`，记录源码版本、安装包版本、更新时间、机器名、分支和提交号。桌面 UI 会在端口旁显示当前运行版本，方便多台设备之间确认到底打开的是哪个构建。

### 拍摄数据注意事项

COLMAP 和 3DGS 需要真实的相机移动和视差。如果照片几乎都来自同一位置，即使能匹配到很多特征，也可能因为无法初始化稀疏三维重建而报 `No good initial image pair found`。

拍摄小物体或展品时，建议围绕目标移动拍摄至少 50-100 张，并保持较高重叠率。不要站在原地只旋转相机。

---

## 日本語

3DGS Editor は、Windows 向けの 3D Gaussian Splatting デスクトップエディタ兼トレーニング補助ツールです。

このプロジェクトには、ローカル Electron デスクトップシェル、ブラウザベースのクロップ/シーンエディタ、COLMAP 変換ヘルパー、Windows ユーザー向けの 3DGS トレーニングスクリプトが含まれています。

### ライセンスと利用目的

このリポジトリは、オープンソースの研究、学習、評価用途向けに公開されています。

3DGS Editor 独自コードは MIT ライセンスです。同梱されている第三者コンポーネントは、それぞれの元ライセンスに従います。特に `gaussian-splatting/` と関連 CUDA モジュールは Gaussian-Splatting License の対象であり、原権利者から別途許可を得ない限り、非商用の研究および評価用途に制限されます。

詳細は [LICENSE](LICENSE) と [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) を確認してください。

### ダウンロード

ワンクリック Windows パッケージは GitHub Release で公開されています。

- [3DGS Editor 0.1.2 Windows x64](https://github.com/fubukisama/3DGS-Editor/releases/tag/v0.1.2)
- ファイル：`3DGS-Editor-0.1.2-win-x64.zip`
- SHA256：Release の `3DGS-Editor-0.1.2-win-x64.zip.sha256` を確認してください。

zip は GitHub の通常ファイルサイズ制限を超えるため、git にはコミットしていません。配布には Release アセットを使用してください。

### クイックスタート

1. `3DGS-Editor-0.1.2-win-x64.zip` をダウンロードして展開します。
2. `Setup 3DGS Editor.cmd` をダブルクリックします。
3. セットアップ完了後、`3DGS Editor.exe` をダブルクリックします。

インストールせずに環境だけ確認する場合は、次を実行します。

```powershell
.\Check 3DGS Editor Environment.cmd
```

英語、中国語、日本語のリリース手順は [README_RELEASE.md](README_RELEASE.md) を参照してください。

### リポジトリ構成

- `crop_editor/` - ローカル Web エディタと Python サーバー。
- `resources/app/` - Electron デスクトップラッパーのソース。
- `scripts/` - セットアップ、パッケージング、環境修復、ユーティリティスクリプト。
- `training_kit/` - Windows 用の変換およびトレーニング補助バッチ。
- `gaussian-splatting/` - 同梱の 3DGS トレーニングソースと CUDA 拡張ソース。

`node_modules`、`datasets`、`output`、`desktop_app`、COLMAP ダウンロード、パッケージ済み Electron バイナリなどの実行時生成フォルダは git から除外しています。

### リリースパッケージの作成

準備済みの Windows ワークスペースで実行します。

```powershell
powershell -ExecutionPolicy Bypass -File scripts\update_version.ps1 -Note "変更内容を記入"
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1
```

生成された zip は GitHub Releases にアップロードする想定です。

### バージョンと複数端末での作業手順

コード変更やパッケージ作成の前に、必ずリモートの最新状態を取得してからバージョン情報を更新します。

```powershell
git pull --ff-only
powershell -ExecutionPolicy Bypass -File scripts\update_version.ps1 -Note "変更内容を記入"
```

このスクリプトは `version`、`resources/app/package.json`、`build_manifest.json` を更新し、ソース版、パッケージ版、更新時刻、端末名、ブランチ、コミットを記録します。デスクトップ UI でもポート番号の横に実行中のバージョンが表示されるため、複数端末でどのビルドを開いているか確認できます。

### 撮影データに関する注意

COLMAP と 3DGS には、実際のカメラ移動と視差が必要です。同じ視点から撮った近似重複写真では、多くの特徴点が一致しても、疎な 3D 再構成を初期化できず `No good initial image pair found` で失敗する場合があります。

小物体や展示物を撮影する場合は、対象の周囲を移動しながら少なくとも 50-100 枚を撮影し、十分な重なりを確保してください。同じ場所に立ったままカメラだけを回転させる撮影は避けてください。
