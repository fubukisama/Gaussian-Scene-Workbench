# Native desktop release guide / 原生发行版说明 / ネイティブ版の配布案内

This guide applies to the native desktop default branch, `main`. Current preview version:
`0.3.1-native-preview`. The native branch no longer contains the old web or
Electron application, launchers or packaging scripts.

## English

Download the artifact from a successful [Native Windows run](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/workflows/native-windows.yml)
for the desired commit. Extract the complete package to a writable directory,
preferably `D:\Apps\GSW` when that drive is available, and run
`bin/Gaussian Scene Workbench.exe`. Preserve all DLLs, plugins, backend files,
licenses and the matching `source/` tree. GitHub artifacts have retention limits;
source availability does not guarantee an unexpired binary download.

Build from this checkout:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_native.ps1 -Configuration Release -Package
```

The default package is
`native/dist/Gaussian-Scene-Workbench-0.3.1-native-preview-win-x64/`.
Use a separate `-PackageDirectoryName` while an older version is running. Save
and exit that version normally before launching the replacement. Installing a
new package does not resume a paused training job.

The Qt application and CUDA training environments are separate. Follow
[runtime setup](docs/WINDOWS_NATIVE_RUNTIME.md) and
[native build prerequisites](native/README.md). Existing model files, managed
project data and checkpoints remain user data; source cleanup does not remove
them. Current limitations and supported checkpoint types are documented in
[the workflow matrix](docs/GENERATION_PIPELINES.md).

## 简体中文

本指南面向默认分支 `main` 中的原生桌面端。

在对应提交的成功 [Native Windows 运行](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/workflows/native-windows.yml)
中下载 artifact。完整解压到可写目录，有 D 盘时建议使用 `D:\Apps\GSW`，
再运行 `bin/Gaussian Scene Workbench.exe`。保留所有 DLL、插件、后端、
许可证及匹配的 `source/` 源码。GitHub artifact 有保存时限，有源码不代表
仍有可下载的二进制包。

源码构建使用上方 `scripts\build_native.ps1 -Configuration Release -Package`。
默认输出位于 `native/dist/Gaussian-Scene-Workbench-0.3.1-native-preview-win-x64/`。
旧版运行中时使用独立的 `-PackageDirectoryName`，保存并正常退出旧版后再启动
新版。安装更新不会自动续训已暂停的任务。

Qt 桌面端与 CUDA 训练环境分开配置，见[环境说明](docs/WINDOWS_NATIVE_RUNTIME.md)
和[构建依赖](native/README.md)。模型、工程数据与检查点属于用户数据，不随代码
清理删除；各管线与完整状态续训的能力边界见[管线矩阵](docs/GENERATION_PIPELINES.md)。
旧网页、Electron、启动器和打包脚本已从本原生分支移除。

## 日本語

この手順は既定の `main` にあるネイティブデスクトップ版を対象とします。

対象コミットの成功した [Native Windows 実行](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/workflows/native-windows.yml)
から artifact を取得します。全体を書き込み可能な場所へ展開し、D ドライブが
ある場合は `D:\Apps\GSW` を優先して、`bin/Gaussian Scene Workbench.exe` を
実行します。DLL・プラグイン・バックエンド・ライセンスと対応する `source/`
を保持してください。GitHub artifact には保存期限があり、ソースの公開は常に
バイナリーを取得できることを意味しません。

ビルドには上記 `scripts\build_native.ps1 -Configuration Release -Package` を使います。
既定の出力は `native/dist/Gaussian-Scene-Workbench-0.3.1-native-preview-win-x64/` です。
旧版が起動中なら別の `-PackageDirectoryName` を使い、保存して通常の手順で
旧版を終了してから新版を起動します。更新で一時停止中の学習は自動再開しません。

Qt アプリと CUDA 学習環境は別に構成します。[実行環境](docs/WINDOWS_NATIVE_RUNTIME.md)
と[ビルド依存関係](native/README.md)を参照してください。モデル・プロジェクト
データ・チェックポイントは利用者のデータであり、コード整理で削除しません。
学習方式と完全状態再開の対応範囲は[パイプライン表](docs/GENERATION_PIPELINES.md)
を参照してください。このネイティブ分岐から旧 Web・Electron・起動器・
パッケージスクリプトは削除済みです。

## License / 许可证 / ライセンス

The original native main program is GPL-3.0-or-later under
[native/LICENSE.md](native/LICENSE.md) and [COPYING](COPYING). Original code
outside that scope retains the root [MIT license](LICENSE); third-party and
research components keep their own licenses. The GPL grant does not grant
commercial rights to every training backend. Binary redistribution includes
matching source and notices; see [source distribution](docs/NATIVE_SOURCE_DISTRIBUTION.md)
and [third-party notices](THIRD_PARTY_LICENSES.md).

中文：原生主程序按上述 GPL 范围发布，范围外原创代码保留 MIT，第三方及研究
后端遵循原许可证。GPL 不授予所有训练后端的商用权；二进制分发附对应源码及声明。

日本語：ネイティブ主プログラムは上記 GPL の範囲で配布し、範囲外の独自コードは
MIT、第三者・研究用バックエンドは元のライセンスを維持します。全学習コードの
商用許諾ではありません。バイナリーの再配布には対応ソースと通知を含めます。
