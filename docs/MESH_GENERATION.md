# Native staged mesh generation / 原生阶段网格生成 / ネイティブ段階別メッシュ生成

## 简体中文

入口：**工作流 → 生成网格**，或主工具栏“生成网格”。选择完整的训练输出目录，须包含 `cfg_args` 和 `point_cloud/iteration_N/point_cloud.ply`。单独导入的高斯 PLY 通常没有训练相机/原照片路径，不能直接代替训练输出。默认使用最新完整迭代；没有合适的源时可浏览选择。2DGS 用有界/无界 TSDF，3DGS 用 SuGaR/GS2Mesh；源与方法不匹配时拒绝执行。

处理顺序：环境检查 → 独立输入副本 → 网格提取/融合 → 完整三角形验证 → 网格预览 → 可选 OpenMVS 照片纹理。整个处理过程保留当前场景；只有完整有效网格才发布预览。外部进程日志可查看，未知阶段耗时不伪造百分比。网格提取过程中尚不提供逐体素/逐深度帧显示。

任务配置在 `.gsw/jobs/mesh-UUID.json`，结果在 `output/meshes/mesh-UUID/`。每次使用独立目录，不写回训练源，不覆盖旧模型。`result.json` 记录已验证阶段、顶点/面数量、纹理资产、失败原因。成功网格追加到工程；纹理失败或取消后，已验证几何仍可追加，任务状态仍显示失败/取消。保存工程后这些模型可重开。阶段记录不是优化器检查点，也尚不支持自动阶段恢复。

OpenMVS 可生成 OBJ/MTL/PNG/ZIP，GLB 取决于后端转换是否成功。生成的纹理包单独保存，自动显示当前只加载几何 PLY；不会宣称纹理已在视口显示。SuGaR 使用自身材质步骤，不能叠加此 OpenMVS 选项。未安装对应环境时明确报错，不伪造结果。

验收：四种网格方法的编排、输出隔离、取消、错误索引拒绝、纹理失败保留与纹理完成回归；桌面模型追加、三语即时切换、工程重开；真实 RTX 4070 Laptop GPU + 官方 2DGS 有界 TSDF 合成测试产生 **700 顶点、1274 面**。不代表实拍质量保证。无界 TSDF、SuGaR、GS2Mesh 与 OpenMVS 的代表性实拍 GPU/材质质量仍待验收；独立 OpenMVS 稠密重建、细粒度预览、阶段恢复仍待实现。

## English

Use **Workflow → Generate Mesh**, or the main toolbar. Select a trained output containing `cfg_args` and `point_cloud/iteration_N/point_cloud.ply`. The newest complete iteration is selected. An imported Gaussian PLY alone lacks the cameras/source-image configuration required here. Bounded/unbounded TSDF require 2DGS; SuGaR/GS2Mesh require 3DGS. Mismatches are rejected.

Stages: environment → isolated input copy → extraction/fusion → complete triangle validation → mesh preview → optional OpenMVS photo texturing. The current view stays visible; only complete, validated geometry is published. Unknown progress is indeterminate, with external logs available. Per-voxel/depth-frame previews are not implemented.

Configurations live in `.gsw/jobs/mesh-UUID.json`, outputs in `output/meshes/mesh-UUID/`. Unique directories preserve original training files and existing models. `result.json` records completed stages, geometry, texture assets and errors. Successful or validated partial meshes are appended; failed/cancelled texturing remains a failed/cancelled task. Saved projects retain associations on reopen. This journal is not optimizer resume, and automatic stage recovery is not implemented.

OpenMVS saves OBJ/MTL/PNG/ZIP; GLB is conditional on backend conversion. Generated texture packages are saved separately: automatic viewport loading currently shows geometry PLY only. SuGaR owns its material stage, so the OpenMVS option is disabled. Missing runtimes fail explicitly.

Acceptance includes all four method orchestration contracts, isolation/cancellation/invalid-index rejection/texture outcomes, native append/reopen/live-language lifecycle, and a real RTX 4070 Laptop GPU official bounded-TSDF synthetic run producing **700 vertices / 1274 faces**. Unbounded/SuGaR/GS2Mesh/OpenMVS representative GPU/photo-material quality remains unverified. Standalone OpenMVS dense reconstruction, fine-grained previews and stage recovery remain outstanding. Upstream algorithm source and licenses remain unchanged.

## 日本語

**ワークフロー → メッシュを生成**、または主ツールバーを使用します。`cfg_args` と `point_cloud/iteration_N/point_cloud.ply` を含む学習出力から最新の完全な反復を選びます。PLY 単体には必要なカメラ・元写真の構成がありません。有界/非有界 TSDF は 2DGS、SuGaR/GS2Mesh は 3DGS に対応し、不一致は拒否します。

環境確認 → 独立入力コピー → 抽出/融合 → 三角形全体の検証 → メッシュプレビュー → 任意の OpenMVS 写真テクスチャの順です。処理中の画面は維持され、検証済み幾何のみを表示します。進捗不明の場合は割合を推測せず、外部ログを表示します。逐次ボクセル・深度フレームの表示は未対応です。

構成は `.gsw/jobs/mesh-UUID.json`、成果は `output/meshes/mesh-UUID/` に保存します。学習元と既存モデルは上書きしません。`result.json` は完成段階・頂点/面・テクスチャ資産・エラーを記録します。成功または検証済み部分メッシュを追加し、後続工程の失敗/取消を成功扱いにしません。保存した工程の再読込でも関連付けを維持します。これは最適化器の再開ではなく、自動段階復旧も未対応です。

OpenMVS は OBJ/MTL/PNG/ZIP を保存し、GLB は変換成功時のみです。生成テクスチャは別保存で、自動表示は幾何 PLY のみです。SuGaR は独自の材質工程を使用します。未導入の実行環境は明示的にエラーとなります。

四手法の編成、隔離、取消、不正インデックス拒否、テクスチャの成否、モデル追加・工程再読込・三言語即時切替を検証します。実 RTX 4070 Laptop GPU の公式有界 TSDF 合成テストで **頂点 700・面 1274** を生成しました。実写品質の保証ではありません。非有界 TSDF/SuGaR/GS2Mesh/OpenMVS の実写 GPU・材質品質、独立した OpenMVS 密な再構築、細粒度プレビュー、段階復旧は引き続き受入または実装が必要です。

## Developer checks

```text
python -B -m unittest native.worker.test_mesh_generation
python -B -m unittest crop_editor.tests.test_training_telemetry
python -B -m unittest native.worker.test_mesh_generation_cuda
python scripts/native_i18n.py
python scripts/test_native_i18n.py
ctest --test-dir native/build-unity-gizmo --output-on-failure
```

Run orchestration tests in a training interpreter with NumPy/plyfile. The CUDA test uses the installed 2DGS source/Python and NVIDIA GPU, not a mocked renderer. `GSW_MESH_TEST_ROOT` may target the packaged backend; `GSW_MESH_TEST_WORKER_PYTHON` can separately select the desktop's worker launcher (tested with the existing Python 3.7 environment). Keep TEMP/TMP on a data drive. Native smoke flag: `--smoke-test-mesh-generation`, using the test-owned `gsw_process_output_fixture` executable. Imported `cfg_args` must be a literal `Namespace(...)` with literal keyword values; arbitrary Python expressions are rejected before upstream rendering.
