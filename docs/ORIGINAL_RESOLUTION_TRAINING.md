# Original-resolution training / 原始分辨率训练 / 元の解像度での学習

## 简体中文

原生桌面版 **训练设置 → 质量预设 → 最高精度（原始分辨率）** 同时适用于 3DGS 与 2DGS。默认 30,000 次迭代，训练分辨率明确显示为 **原始分辨率（1:1）**，传给训练器 `-r 1`；不使用上游自动缩到 1600 px 的模式。宽高都不额外降采样，显存不足时不会偷偷缩图。该预设表示图像细节保真，不保证测量精度、未知视角或网格质量。

| 预设 | 默认迭代 | 宽 / 高比例（两通道一致） |
| --- | --- | --- |
| 快速预览 | 10,000 | 1/2 |
| 标准（保留历史兼容） | 30,000 | 1/8 |
| 高质量 | 30,000 | 1/4 |
| 最高质量（保留历史兼容） | 30,000 | 1/2 |
| 最高精度（原始分辨率） | 30,000 | 1:1 |

优化设置沿用各自最高质量配置，而非把 3DGS 的体高斯/抗锯齿参数套给 2DGS。迭代数和比例仍可手动修改，界面警告按实际比例显示；切换语言只改文字，不重置参数。旧工程/任务和完整优化器状态续训保留已记录的配置；提高旧模型训练分辨率应新建训练任务，而不是冒充原状态续训。

图像准备保留原始输入；COLMAP 去畸变明确使用 `--max_image_size -1`，不限制输出图像的最长边。必要的几何去畸变仍进行，校准重映射可能改变有效画幅，不承诺与拍摄文件字节相同。1:1 指读取去畸变训练图像的实际尺寸。已经由外部软件或旧流程缩小的数据无法凭这个选项恢复细节，须从原图重新重建。特征提取的工作尺度、预览渲染比例与训练照片尺寸是不同设置；预览降级不改训练数据。这不是相机 RAW 文件解码功能。

全分辨率可能明显增加显存、内存和耗时。3DGS 的稀疏优化器在扩展不可用时仍明确回退到 Adam，但不改变分辨率。2DGS 原有密度/过度裁剪保护保持不变。网格链路见下方矩阵，不能把该训练预设当成“所有后处理参数最高”。

## English

Choose **Training Settings → Quality Preset → Maximum Fidelity (Original Resolution)** for either 3DGS or 2DGS. Defaults are **30,000 iterations / Original Resolution (1:1)**. An explicit `-r 1` bypasses the upstream automatic 1600 px mode. Both image dimensions are retained, with no silent resolution reduction on out-of-memory. Fidelity here concerns image sampling, not certified metric accuracy, unseen-view coverage or mesh quality.

The two backends retain their own highest-quality optimization settings. Explicit manual edits, historical jobs and complete optimizer-state resumes keep their saved values. Live language switching only changes captions. COLMAP undistortion is explicitly uncapped (`--max_image_size -1`); geometric rectification remains necessary and can change the valid image extent. Full resolution refers to the actual undistorted training images, not byte-identical camera files or RAW decoding. Previously resized datasets need reconstruction from original images. Feature-extraction working scale and viewport preview resolution are independent of training-photo resolution. Memory use and runtime can increase substantially; existing optimizer-capability and density guards remain in effect without resizing images.

## 日本語

3DGS、2DGS とも **学習設定 → 品質プリセット → 最高精細（元の解像度）** を選択します。既定値は **30,000 回 / 元の解像度（1:1）** です。`-r 1` を明示し、上流実装の自動 1600 px 縮小を使用しません。画像の幅・高さを保持し、メモリ不足時に黙って解像度を下げません。画像の細部を保持する設定であり、測量精度、未知視点やメッシュ品質を保証するものではありません。

最適化設定は各方式の最高品質構成を継承します。手動設定、過去のタスク、完全な最適化器状態からの再開では保存値を保持し、言語切替でも変更しません。COLMAP の歪み補正は `--max_image_size -1` で出力サイズの上限を設けません。必要な幾何補正で有効画角が変わる場合があります。1:1 は歪み補正後の学習画像の実寸を意味し、撮影ファイルのバイト一致や RAW 現像を意味しません。縮小済みデータは元画像から再構築してください。特徴抽出の作業スケールやプレビュー解像度は学習画像サイズとは別です。GPU メモリ使用量と学習時間は大幅に増加する場合があります。

## Capability matrix and remaining gates

| Pipeline | Original-resolution behavior | Remaining acceptance gate / limitation |
| --- | --- | --- |
| 3DGS | New original-quality preset; volumetric backend's own highest-quality settings; uncapped COLMAP training images | Full 30K representative datasets, VRAM capacity, held-out/metric quality; sparse Adam depends on installed extension |
| 2DGS | Same preset, iteration/ratio and preparation policy; surfel-specific optimization and density guards | Full 30K representative datasets, VRAM capacity and held-out/mesh quality; no 3DGS AA flags copied |
| 2DGS bounded / unbounded TSDF | Consumes the trained 2DGS output; does not redefine this training preset | Voxel/depth-fusion parameters remain separate; a full-resolution Gaussian training preset does not maximize TSDF resolution |
| SuGaR | Consumes a 3DGS result; refinement uses its independent image-size and texture controls | No shared Gaussian-preset override of SuGaR's own image-preparation cap; source-refinement quality and resources need separate validation |
| GS2Mesh | Consumes the result; separate depth-generation downsample control remains explicit | Set its own depth downsample to 1 for full-resolution depth; not automatically changed by the Gaussian training preset |
| OpenMVS texture baking | Original source images retained; independent texture-atlas budget and resolution | No increase to texture budget/max edge by this preset; baking quality remains separately validated |

Automated coverage includes both backend option/CLI mappings, unchanged old profiles, manual overrides, original-quality full-state resume metadata, uncapped COLMAP command generation, five native presets and immediate three-language switching. The opt-in `native.worker.test_training_resolution_cuda` check uses each actual CUDA runtime and installed loader to assert that a **1928 × 1084** source (>1600 px) reaches the training camera and tensor unchanged; the legacy maximum-quality profile remains **964 × 542**. This is a resolution contract check, not a 30K quality benchmark. Existing renderer-equivalence limitations documented in `RECONSTRUCTION_QUALITY.md` are unchanged.

Validated on 2026-10-02 against the new desktop package: **44/44 native CTests**, **137 packaged-build worker checks (8 runtime-specific skips)**, **118 reconstruction/backend checks (1 skip)** and **14 translation-validator checks** passed. Installed dialogs passed immediate-language switching and captured both backends in all three locales. Real 3DGS and 2DGS CUDA loaders retained the full **1928 × 1084** tensor and original RGB values; actual uncapped COLMAP PINHOLE undistortion retained all three owned fixture images at that size. Each CUDA runtime also passed its complete optimizer/Adam/RNG-state pause/resume regression. No full 30K original-resolution quality benchmark is claimed.

Primary source references: [official 3DGS camera loader](https://github.com/graphdeco-inria/gaussian-splatting/blob/main/utils/camera_utils.py), [official 2DGS camera loader](https://github.com/hbb1/2d-gaussian-splatting/blob/main/utils/camera_utils.py), and [COLMAP undistortion options](https://github.com/colmap/colmap/blob/main/src/colmap/image/undistortion.h).
