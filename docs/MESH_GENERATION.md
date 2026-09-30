# Native staged mesh generation / 原生阶段网格生成 / ネイティブ段階別メッシュ生成

## 简体中文

入口：**工作流 → 生成网格**，或主工具栏“生成网格”。选择完整的训练输出目录，须包含 `cfg_args` 和 `point_cloud/iteration_N/point_cloud.ply`。单独导入的高斯 PLY 通常没有训练相机/原照片路径，不能直接代替训练输出。默认使用最新完整迭代；没有合适的源时可浏览选择。2DGS 用有界/无界 TSDF，3DGS 用 SuGaR/GS2Mesh；源与方法不匹配时拒绝执行。

处理顺序：环境检查 → 独立输入副本 → 网格提取/融合 → 完整三角形验证 → 网格预览 → 可选照片纹理/后端材质 → 材质预览准备（`texture_preview`）→ 贴图网格就绪（`texture_ready`）。整个处理过程保留当前场景；只有完整有效网格才发布预览，烘焙和材质准备期间保留已完成几何。外部进程日志可查看，未知阶段耗时不伪造百分比。网格提取过程中尚不提供逐体素/逐深度帧显示。

任务配置在 `.gsw/jobs/mesh-UUID.json`，结果在 `output/meshes/mesh-UUID/`。每次使用独立目录，不写回训练源，不覆盖旧模型。`result.json` 记录已验证阶段、顶点/面数量、纹理资产、失败原因。成功网格追加到工程；纹理失败或取消后，已验证几何仍可追加，任务状态仍显示失败/取消。保存工程后这些模型可重开。阶段记录不是优化器检查点，也尚不支持自动阶段恢复。

OpenMVS 可生成 OBJ/MTL/PNG/ZIP，GLB 取决于后端转换是否成功。原始生成资产全部保留；新增原生自有桥接在任务模型的 `native-material/` 中写入 `preview.ply` 与 `atlas.png`，验证后自动加载贴图网格，保存工程后保留材质关联。四种方法共用此桥接：有界/无界 TSDF 和 GS2Mesh 使用可选 OpenMVS 材质，SuGaR 使用自身材质步骤、不能叠加 OpenMVS 选项；没有材质的结果仍显示几何。转换是生成结果的内部适配，不是通用 OBJ/GLB 导入，也不是优化器状态恢复。未安装对应环境时明确报错，不伪造结果。

桥接仅接受不透明、漫反射贴图的三角形 OBJ。多材质/多页原始分辨率图片拼入一个图集并重映射逐面角 UV，保留接缝、顶点坐标及完整三角形，不改写原 OBJ/MTL/图片；几何和 UV 暂存到磁盘。图集每边至多 8192 px，图片转换工作预算为 256 MiB，超限时明确失败，不偷偷缩图或简化几何。非三角面、平铺/越界 UV、贴图变换选项、透明度、非白漫反射乘色或其他不支持的着色效果均拒绝，而不是丢弃后冒充成功；动态/HDR/带 EXIF 旋转的图片也拒绝。材质准备失败或取消时保留已验证网格与原始纹理包，并维持失败/取消状态。桥接不降采样；GPU 上传仍受设备纹理尺寸上限约束，触发时明确提示。

验收范围：四种网格方法的编排、输出隔离、取消、错误索引拒绝、纹理失败保留与材质桥接；单页/多页、UV 接缝、非法材质/路径/预算拒绝；桌面贴图自动载入、缺失图集不能标为成功、三语即时切换不重载模型、工程重开保留贴图。具体执行结果见对应验证记录。此前真实 RTX 4070 Laptop GPU + 官方 2DGS 有界 TSDF 合成测试产生 **700 顶点、1274 面**；本轮材质适配不代表实拍质量保证。无界 TSDF、SuGaR、GS2Mesh 与 OpenMVS 的代表性实拍 GPU/材质质量仍待验收；独立 OpenMVS 稠密重建、细粒度预览、阶段恢复仍待实现。

## English

Use **Workflow → Generate Mesh**, or the main toolbar. Select a trained output containing `cfg_args` and `point_cloud/iteration_N/point_cloud.ply`. The newest complete iteration is selected. An imported Gaussian PLY alone lacks the cameras/source-image configuration required here. Bounded/unbounded TSDF require 2DGS; SuGaR/GS2Mesh require 3DGS. Mismatches are rejected.

Stages: environment → isolated input copy → extraction/fusion → complete triangle validation → mesh preview → optional photo texturing/backend materials → material-preview preparation (`texture_preview`) → textured mesh ready (`texture_ready`). The current view stays visible; only complete, validated geometry is published, and completed geometry remains visible during baking/material preparation. Unknown progress is indeterminate, with external logs available. Per-voxel/depth-frame previews are not implemented.

Configurations live in `.gsw/jobs/mesh-UUID.json`, outputs in `output/meshes/mesh-UUID/`. Unique directories preserve original training files and existing models. `result.json` records completed stages, geometry, texture assets and errors. Successful or validated partial meshes are appended; failed/cancelled texturing remains a failed/cancelled task. Saved projects retain associations on reopen. This journal is not optimizer resume, and automatic stage recovery is not implemented.

OpenMVS saves OBJ/MTL/PNG/ZIP; GLB is conditional on backend conversion. Original generated assets remain intact. A native-owned adapter creates `native-material/preview.ply` and `native-material/atlas.png` inside the job's model directory, then automatically loads the validated textured mesh. Saved projects retain this material association. All four methods share this adapter: bounded/unbounded TSDF and GS2Mesh can use optional OpenMVS texturing; SuGaR supplies its own materials, so its OpenMVS option is disabled. Results without materials remain geometry-only. This is an internal generated-asset adapter, not general OBJ/GLB import or optimizer resume. Missing runtimes fail explicitly.

Only opaque diffuse-textured triangle OBJ assets are accepted. Multiple material/image pages are packed at their original resolution into one atlas, with remapped face-corner UVs preserving seams, vertex coordinates and complete triangles. Original OBJ/MTL/images are not rewritten; geometry and UV tables are spooled to disk. Each atlas dimension is limited to 8192 px, with a 256 MiB image-conversion working budget. Over-budget assets fail explicitly rather than being silently downsampled or geometrically simplified. Non-triangle faces, tiled/out-of-range UVs, diffuse-map transform options, transparency, non-white diffuse multipliers and unsupported shader effects are rejected, as are animated/HDR/EXIF-rotated images. Failed/cancelled preparation preserves validated geometry and original texture packages without marking the task successful. The adapter does not downsample; GPU uploads remain subject to the device texture-size limit, with an explicit warning if it applies.

Acceptance coverage includes all four method contracts, isolation/cancellation/invalid-index rejection/material outcomes; single/multiple atlas pages, seams and invalid material/path/budget rejection; native automatic texture loading, missing-atlas failure, live languages without model reload, and project reopen. Consult the matching validation record for execution results. The earlier real RTX 4070 Laptop GPU official bounded-TSDF synthetic run produced **700 vertices / 1274 faces**. Material-adapter tests do not certify real-photo quality. Unbounded/SuGaR/GS2Mesh/OpenMVS representative GPU/photo-material quality remains unverified. Standalone OpenMVS dense reconstruction, fine-grained previews and stage recovery remain outstanding. Upstream algorithm source and licenses remain unchanged.

## 日本語

**ワークフロー → メッシュを生成**、または主ツールバーを使用します。`cfg_args` と `point_cloud/iteration_N/point_cloud.ply` を含む学習出力から最新の完全な反復を選びます。PLY 単体には必要なカメラ・元写真の構成がありません。有界/非有界 TSDF は 2DGS、SuGaR/GS2Mesh は 3DGS に対応し、不一致は拒否します。

環境確認 → 独立入力コピー → 抽出/融合 → 三角形全体の検証 → メッシュプレビュー → 任意の写真テクスチャ/バックエンド材質 → マテリアルプレビュー準備（`texture_preview`）→ テクスチャ付きメッシュ準備完了（`texture_ready`）の順です。処理中の画面は維持され、検証済み幾何のみを表示します。ベイク・材質準備中も完成した幾何を保持します。進捗不明の場合は割合を推測せず、外部ログを表示します。逐次ボクセル・深度フレームの表示は未対応です。

構成は `.gsw/jobs/mesh-UUID.json`、成果は `output/meshes/mesh-UUID/` に保存します。学習元と既存モデルは上書きしません。`result.json` は完成段階・頂点/面・テクスチャ資産・エラーを記録します。成功または検証済み部分メッシュを追加し、後続工程の失敗/取消を成功扱いにしません。保存した工程の再読込でも関連付けを維持します。これは最適化器の再開ではなく、自動段階復旧も未対応です。

OpenMVS は OBJ/MTL/PNG/ZIP を保存し、GLB は変換成功時のみです。元の生成資産はすべて保持します。ネイティブ所有の変換処理がジョブのモデルディレクトリ内に `native-material/preview.ply` と `native-material/atlas.png` を作成し、検証後にテクスチャ付きメッシュを自動表示します。保存・再読込後も材質の関連付けを維持します。四手法でこの処理を共有します。有界/非有界 TSDF と GS2Mesh は任意の OpenMVS 材質、SuGaR は独自の材質工程を使用し、OpenMVS オプションは無効です。材質がない結果は幾何のみを表示します。生成資産の内部変換であり、汎用 OBJ/GLB インポートや最適化器の再開ではありません。未導入の実行環境は明示的にエラーとなります。

不透明なディフューズテクスチャ付き三角形 OBJ のみを受け入れます。複数材質・画像ページを元の解像度のまま一枚のテクスチャアトラスへ配置し、面のコーナーごとの UV を再配置してシーム・頂点座標・完全な三角形を保持します。元 OBJ/MTL/画像は変更せず、幾何と UV はディスクに一時保存します。アトラスは各辺 8192 px 以下、画像変換の作業メモリ予算は 256 MiB です。超過時は明示的に失敗し、黙って縮小・簡略化しません。非三角形、反復/範囲外 UV、テクスチャ変換オプション、透明度、白以外のディフューズ乗算色、未対応のシェーダー効果、動画/HDR/EXIF 回転画像を拒否します。準備の失敗・取消時も検証済み幾何と元テクスチャ資産を保持し、成功扱いにしません。変換時は画像を縮小しませんが、GPU への転送はデバイス上限の対象となり、該当時に明示表示します。

受入対象は四手法の編成、隔離、取消、不正インデックス拒否と材質処理、単一/複数画像ページ、UV シーム、不正材質・パス・メモリ上限の拒否、テクスチャ自動表示、アトラス欠落の失敗判定、モデルを再読込しない三言語即時切替、保存・再読込です。実行結果は対応する検証記録を参照してください。以前の実 RTX 4070 Laptop GPU 公式有界 TSDF 合成テストで **頂点 700・面 1274** を生成しました。本段階の変換テストは実写品質を保証しません。非有界 TSDF/SuGaR/GS2Mesh/OpenMVS の実写 GPU・材質品質、独立した OpenMVS 密な再構築、細粒度プレビュー、段階復旧は引き続き受入または実装が必要です。

## Developer checks

OpenMVS compatibility / OpenMVS 兼容 / OpenMVS 互換性：Only the OpenMVS collector normalizes its fixed legacy `Tr 1` marker to opaque `d 1` in copied assets, and records `material_normalization=openmvs_opaque_Tr`. The adapter uses that normalized copy; original files and generic/SuGaR transparency rules remain unchanged. / 仅 OpenMVS 收集器将其固定旧标记 `Tr 1` 在副本中改为不透明 `d 1`，并记录上述标识；适配器读取该副本，原文件和通用/SuGaR 透明度规则不变。/ OpenMVS 収集時のみ固定の旧 `Tr 1` をコピー内で不透明な `d 1` に変換し、上記識別子を記録します。元ファイルおよび汎用/SuGaR の透明度規則は変更しません。 This follows the upstream [MaterialLib save/load implementation](https://github.com/cdcseacave/openMVS/blob/master/libs/IO/OBJ.cpp); it is not general transparent-material support.

```text
python -B -m unittest native.worker.test_mesh_generation
python -B -m unittest native.worker.test_mesh_material_preview
python -B -m unittest crop_editor.tests.test_training_telemetry
python -B -m unittest native.worker.test_mesh_generation_cuda
python scripts/native_i18n.py
python scripts/test_native_i18n.py
ctest --test-dir native/build-unity-gizmo --output-on-failure
```

Run orchestration tests in a training interpreter with NumPy/plyfile. The CUDA test uses the installed 2DGS source/Python and NVIDIA GPU, not a mocked renderer. `GSW_MESH_TEST_ROOT` may target the packaged backend; `GSW_MESH_TEST_WORKER_PYTHON` can separately select the desktop's worker launcher (tested with the existing Python 3.7 environment). Keep TEMP/TMP on a data drive. Native smoke flag: `--smoke-test-mesh-generation`, using the test-owned `gsw_process_output_fixture` executable. Imported `cfg_args` must be a literal `Namespace(...)` with literal keyword values; arbitrary Python expressions are rejected before upstream rendering.
