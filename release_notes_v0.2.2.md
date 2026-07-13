# Gaussian Scene Workbench v0.2.2

## English

- Fixed development and packaged launches selecting an older `Desktop\3dgs` workspace instead of the code shipped with the current build.
- The bundled repository or package directory now takes priority, while `GS_EDITOR_ROOT` remains the highest-priority explicit override.
- Added regression tests for development, packaged, and explicit-root startup paths.

## 中文

- 修复开发版和封装版启动时错误选择旧 `Desktop\3dgs` 工作区，而没有使用当前版本所附代码的问题。
- 当前仓库或安装包目录现在优先于旧工作区，显式设置的 `GS_EDITOR_ROOT` 仍具有最高优先级。
- 增加开发模式、封装模式和显式根目录三种启动路径的回归测试。

## 日本語

- 開発版およびパッケージ版の起動時に、現在のビルドではなく古い `Desktop\3dgs` ワークスペースを選択する問題を修正しました。
- 現在のリポジトリまたはパッケージディレクトリを優先し、明示的な `GS_EDITOR_ROOT` 指定は引き続き最優先されます。
- 開発版、パッケージ版、明示的ルート指定の起動経路に対する回帰テストを追加しました。
