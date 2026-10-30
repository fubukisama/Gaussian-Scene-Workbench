<div lang="en">

# Documentation · <span lang="zh-CN">文档</span> · <span lang="ja">ドキュメント</span>

[English overview](../README.md) · <span lang="zh-CN">[中文介绍](../README.zh-CN.md)</span> · <span lang="ja">[日本語の概要](../README.ja.md)</span>

## Start here · <span lang="zh-CN">从这里开始</span> · <span lang="ja">はじめに</span>

| Guide | Summary · <span lang="zh-CN">简介</span> · <span lang="ja">概要 </span>|
| --- | --- |
| [Download & install](../README_RELEASE.md) | Preview installer, requirements and update status · <span lang="zh-CN">预览版获取与环境</span> · <span lang="ja">入手と必要環境 </span>|
| [Changelog](../CHANGELOG.md) · <span lang="zh-CN">[更新日志](../CHANGELOG.zh-CN.md)</span> · <span lang="ja">[変更履歴](../CHANGELOG.ja.md)</span> | Dated changes and source references, separate from release and installation acceptance · <span lang="zh-CN">按日期记录变更与源码，不混淆发布和安装验收</span> · <span lang="ja">日付別の変更とソース参照。公開・導入の検収とは区別 </span>|
| [Native desktop build](../native/README.md) | Source build and implementation details · <span lang="zh-CN">源码构建</span> · <span lang="ja">ビルド </span>|
| [Windows runtime](WINDOWS_NATIVE_RUNTIME.md) | External reconstruction/training environments · <span lang="zh-CN">外部运行库</span> · <span lang="ja">外部実行環境 </span>|
| [Supported pipelines](GENERATION_PIPELINES.md) | Available routes, backend requirements and limitations · <span lang="zh-CN">管线支持矩阵</span> · <span lang="ja">対応表 </span>|

## Workflows · <span lang="zh-CN">工作流</span> · <span lang="ja">ワークフロー</span>

| Task | Guide |
| --- | --- |
| Photos / video / datasets · <span lang="zh-CN">素材与数据集</span> · <span lang="ja">入力データ </span>| [Workspace import](WORKSPACE_DROP.md) · [COLMAP](COLMAP_SETUP.md) |
| 3DGS / 2DGS training · <span lang="zh-CN">训练</span> · <span lang="ja">学習 </span>| [Pause & resume](TRAINING_RESUME.md) · [Runtime controls](RUNTIME_DECOUPLING.md) · [Performance](TRAINING_PERFORMANCE.md) |
| Known cameras & scale · <span lang="zh-CN">已知相机与尺度</span> · <span lang="ja">既知カメラとスケール </span>| [Camera contracts](CAMERA_SCALE_P0.md) · [Distance / baseline / control points](CAMERA_SCALE_P1.md) |
| Initialization, geometry & evaluation · <span lang="zh-CN">初始化与几何评价</span> · <span lang="ja">初期化と形状評価 </span>| [General reconstruction](GENERAL_RECONSTRUCTION.md) · [Quality measurements](TRAINING_QUALITY.md) |
| View, edit & coordinates · <span lang="zh-CN">显示、编辑与坐标</span> · <span lang="ja">表示・編集・座標 </span>| [Viewport controls](VIEWPORT_DISPLAY.md) · [Scene coordinates](SCENE_COORDINATES.md) · [Multiple models](MULTI_SCENE_IMPORT.md) |
| Large models & meshes · <span lang="zh-CN">大模型与网格</span> · <span lang="ja">大規模モデルとメッシュ </span>| [Resource budgets](RESOURCE_BUDGETS.md) · [Mesh generation](MESH_GENERATION.md) |
| Export & experiments · <span lang="zh-CN">导出与实验</span> · <span lang="ja">出力と実験 </span>| [Gaussian PLY baking](GAUSSIAN_PLY_BAKING.md) · [Recorded exports & batches](RECORDED_PLY_AND_BATCH.md) |
| Experimental methods · <span lang="zh-CN">实验性方法</span> · <span lang="ja">実験的手法 </span>| [Quality plans](QUALITY_PLAN.md) · [Capability comparison](STUDIO_COMPARISON.md) |

## Development & support · <span lang="zh-CN">开发与支持</span> · <span lang="ja">開発とサポート</span>

[Native CI](NATIVE_CI.md) · [Localization](LOCALIZATION.md) · [Diagnostics](RENDER_DIAGNOSTICS.md) · [Managed updates](NATIVE_UPDATES.md) · [Migration](NATIVE_MIGRATION.md) · [Parity gates](NATIVE_PARITY.md)

## License & scope · <span lang="zh-CN">许可与边界</span> · <span lang="ja">ライセンスと範囲</span>

[License guide / <span lang="zh-CN">许可说明</span> / <span lang="ja">ライセンス案内</span>](LICENSING.md) · [Native license](../native/LICENSE.md) · [Repository license](../LICENSE) · [Third-party notices](../THIRD_PARTY_LICENSES.md) · [Corresponding source](NATIVE_SOURCE_DISTRIBUTION.md)

The application is a native preview. Runtime checks, rendered depth products and short training runs do not certify physical geometry accuracy. CUDA/training backends have separate requirements and licenses.

<div lang="zh-CN">

中文：当前为原生预览版；运行检查、渲染深度成果和短程训练不等于实体几何精度认证。CUDA／训练后端另有环境要求与许可。

</div>

<div lang="ja">

日本語：本版はネイティブプレビューです。実行確認・描画深度・短期学習は実物の形状精度を認証しません。CUDA・学習バックエンドの要件とライセンスは別です。


</div>

</div>
