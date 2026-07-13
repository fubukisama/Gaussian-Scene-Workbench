# Gaussian Scene Workbench v0.2.3

## English

- Restored discovery and loading of models trained in the existing `Desktop\3dgs` workspace after application updates.
- Separated the application code root from the persistent model workspace root, so updating code no longer hides `datasets`, `output`, or PSNR reports.
- Scene discovery now exposes each model's actual filesystem path and supports `GS_EDITOR_WORKSPACE_ROOT` as an explicit override.

## 中文

- 恢复应用更新后对原 `Desktop\3dgs` 工作区中已训练模型的发现和读取。
- 分离应用代码根目录与持久模型工作区，更新代码不再隐藏 `datasets`、`output` 或 PSNR 报告。
- 场景发现现在返回模型真实文件路径，并支持使用 `GS_EDITOR_WORKSPACE_ROOT` 显式指定其他工作区。

## 日本語

- アプリ更新後も既存の `Desktop\3dgs` ワークスペースにある学習済みモデルを検出して読み込めるよう修正しました。
- アプリコードのルートと永続モデルワークスペースを分離し、更新によって `datasets`、`output`、PSNR レポートが見えなくなる問題を解消しました。
- シーン検出はモデルの実ファイルパスを返し、`GS_EDITOR_WORKSPACE_ROOT` による明示的なワークスペース指定にも対応します。
