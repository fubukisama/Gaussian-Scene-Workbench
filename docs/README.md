# Documentation · 文档 · ドキュメント

[English overview](../README.md) · [中文介绍](../README.zh-CN.md) · [日本語の概要](../README.ja.md)

## Start here · 从这里开始 · はじめに

| Guide | Summary · 简介 · 概要 |
| --- | --- |
| [Download & install](../README_RELEASE.md) | Preview installer, requirements and update status · 预览版获取与环境 · 入手と必要環境 |
| [Native desktop build](../native/README.md) | Source build and implementation details · 源码构建 · ビルド |
| [Windows runtime](WINDOWS_NATIVE_RUNTIME.md) | External reconstruction/training environments · 外部运行库 · 外部実行環境 |
| [Supported pipelines](GENERATION_PIPELINES.md) | Available routes, backend requirements and limitations · 管线支持矩阵 · 対応表 |

## Workflows · 工作流 · ワークフロー

| Task | Guide |
| --- | --- |
| Photos / video / datasets · 素材与数据集 · 入力データ | [Workspace import](WORKSPACE_DROP.md) · [COLMAP](COLMAP_SETUP.md) |
| 3DGS / 2DGS training · 训练 · 学習 | [Pause & resume](TRAINING_RESUME.md) · [Runtime controls](RUNTIME_DECOUPLING.md) · [Performance](TRAINING_PERFORMANCE.md) |
| Known cameras & scale · 已知相机与尺度 · 既知カメラとスケール | [Camera contracts](CAMERA_SCALE_P0.md) · [Distance / baseline / control points](CAMERA_SCALE_P1.md) |
| Initialization, geometry & evaluation · 初始化与几何评价 · 初期化と形状評価 | [General reconstruction](GENERAL_RECONSTRUCTION.md) · [Quality measurements](TRAINING_QUALITY.md) |
| View, edit & coordinates · 显示、编辑与坐标 · 表示・編集・座標 | [Viewport controls](VIEWPORT_DISPLAY.md) · [Scene coordinates](SCENE_COORDINATES.md) · [Multiple models](MULTI_SCENE_IMPORT.md) |
| Large models & meshes · 大模型与网格 · 大規模モデルとメッシュ | [Resource budgets](RESOURCE_BUDGETS.md) · [Mesh generation](MESH_GENERATION.md) |
| Export & experiments · 导出与实验 · 出力と実験 | [Gaussian PLY baking](GAUSSIAN_PLY_BAKING.md) · [Recorded exports & batches](RECORDED_PLY_AND_BATCH.md) |
| Experimental methods · 实验性方法 · 実験的手法 | [Quality plans](QUALITY_PLAN.md) · [Capability comparison](STUDIO_COMPARISON.md) |

## Development & support · 开发与支持 · 開発とサポート

[Native CI](NATIVE_CI.md) · [Localization](LOCALIZATION.md) · [Diagnostics](RENDER_DIAGNOSTICS.md) · [Managed updates](NATIVE_UPDATES.md) · [Migration](NATIVE_MIGRATION.md) · [Parity gates](NATIVE_PARITY.md)

## License & scope · 许可与边界 · ライセンスと範囲

[License guide / 许可说明 / ライセンス案内](LICENSING.md) · [Native license](../native/LICENSE.md) · [Repository license](../LICENSE) · [Third-party notices](../THIRD_PARTY_LICENSES.md) · [Corresponding source](NATIVE_SOURCE_DISTRIBUTION.md)

The application is a native preview. Runtime checks, rendered depth products and short training runs do not certify physical geometry accuracy. CUDA/training backends have separate requirements and licenses.

中文：当前为原生预览版；运行检查、渲染深度成果和短程训练不等于实体几何精度认证。CUDA／训练后端另有环境要求与许可。

日本語：本版はネイティブプレビューです。実行確認・描画深度・短期学習は実物の形状精度を認証しません。CUDA・学習バックエンドの要件とライセンスは別です。
