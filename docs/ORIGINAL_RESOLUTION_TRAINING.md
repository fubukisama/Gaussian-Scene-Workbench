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

训练设置现在异步读取图像文件头，显示配对的原始宽高、预计缩放后宽高、目录图像数与总像素。运行 COLMAP 时优先统计 `input`，否则优先 `images`；这些是目录预计值，不是假称已注册相机数。混合尺寸逐组列出，损坏/未知格式不猜尺寸；比例和语言切换复用当前对话框缓存，重新打开会重新读取。准备去畸变仍可能改变最终尺寸。切换 3DGS / 2DGS 保留手填迭代和比例，只有明确切换质量预设才重新应用预设。

训练监视先显示环境适配后的生效迭代、比例与实际优化器；相机加载后替换为真正用于训练的图像张数、尺寸组和总像素。增密参数、3DGS 抗锯齿/曝光补偿或 2DGS 深度混合比例在摘要提示中展示。结果目录 `scene.json` 的 `training.summary` 与持久化任务保存相同机器摘要；旧任务没有新字段仍能运行。历史手动分辨率参数只有 `1/2/4/8` 表示缩放除数，其余正值按上游语义显示为目标像素宽度，不改写旧任务。它不推断峰值显存，不是质量评分，也不改变优化器或图像数据。

两通道的监视面板现在按宽度和字体大小自动排列七项指标。缩小或浮动面板时可滚动查看完整报告、告警和曲线，不强制把底部面板撑大。长任务名称以纯文本省略显示，悬停可查看完整原文；生效参数提示也保留完整报告。滚动、调整面板和切换语言不清空训练采样或改变处理状态。

## English

Choose **Training Settings → Quality Preset → Maximum Fidelity (Original Resolution)** for either 3DGS or 2DGS. Defaults are **30,000 iterations / Original Resolution (1:1)**. An explicit `-r 1` bypasses the upstream automatic 1600 px mode. Both image dimensions are retained, with no silent resolution reduction on out-of-memory. Fidelity here concerns image sampling, not certified metric accuracy, unseen-view coverage or mesh quality.

The two backends retain their own highest-quality optimization settings. Explicit manual edits, historical jobs and complete optimizer-state resumes keep their saved values. Live language switching only changes captions. COLMAP undistortion is explicitly uncapped (`--max_image_size -1`); geometric rectification remains necessary and can change the valid image extent. Full resolution refers to the actual undistorted training images, not byte-identical camera files or RAW decoding. Previously resized datasets need reconstruction from original images. Feature-extraction working scale and viewport preview resolution are independent of training-photo resolution. Memory use and runtime can increase substantially; existing optimizer-capability and density guards remain in effect without resizing images.

Training Settings asynchronously reads image headers and groups actual paired width/height values, estimated resized dimensions, directory counts and total pixels. It prefers `input` before requested COLMAP preparation and `images` otherwise. These are directory estimates, never registered-camera counts; unknown headers stay unknown. Changing ratio/language only reformats the dialog's cached scan; reopening rereads it. Backend switching preserves manually edited iterations and ratio; explicitly changing the quality preset reapplies defaults.

The training monitor reports effective settings after environment adaptation (including actual Adam fallback), then the dimensions/count/pixels of cameras actually loaded by each trainer. Its tooltip exposes backend-specific densification/appearance settings. Persistent jobs and result `scene.json` (`training.summary`) retain this machine-readable report; old jobs without it remain compatible. Only legacy resolution values `1/2/4/8` mean divisors; other positive values are displayed as target pixel widths, preserving upstream semantics without rewriting jobs. It neither estimates peak VRAM nor changes image/optimizer data. It is not a reconstruction-quality score.

Both monitors reflow all seven metrics by width and font size. Narrow or short panels scroll to keep reports, warnings and readable curves accessible without enlarging the default bottom dock. Long task names are plain-text, middle-elided labels with complete tooltips; the effective-settings tooltip retains the complete report too. Scrolling, resizing and live language switching preserve samples and processing state.

## 日本語

3DGS、2DGS とも **学習設定 → 品質プリセット → 最高精細（元の解像度）** を選択します。既定値は **30,000 回 / 元の解像度（1:1）** です。`-r 1` を明示し、上流実装の自動 1600 px 縮小を使用しません。画像の幅・高さを保持し、メモリ不足時に黙って解像度を下げません。画像の細部を保持する設定であり、測量精度、未知視点やメッシュ品質を保証するものではありません。

最適化設定は各方式の最高品質構成を継承します。手動設定、過去のタスク、完全な最適化器状態からの再開では保存値を保持し、言語切替でも変更しません。COLMAP の歪み補正は `--max_image_size -1` で出力サイズの上限を設けません。必要な幾何補正で有効画角が変わる場合があります。1:1 は歪み補正後の学習画像の実寸を意味し、撮影ファイルのバイト一致や RAW 現像を意味しません。縮小済みデータは元画像から再構築してください。特徴抽出の作業スケールやプレビュー解像度は学習画像サイズとは別です。GPU メモリ使用量と学習時間は大幅に増加する場合があります。

学習設定では画像ヘッダーを非同期で読み取り、幅・高さの正しい組み合わせ、縮小後の予想寸法、フォルダー内の画像数と総画素数を表示します。COLMAP を実行する場合は `input`、それ以外は `images` を優先します。登録カメラ数や最終寸法ではなく、フォルダーからの予測です。不明な寸法を推測しません。解像度比や言語の切替ではキャッシュを再表示し、ダイアログを開き直すと再読取します。3DGS / 2DGS の切替は手入力の反復数と比率を保持し、品質プリセットの明示的な変更だけが既定値を再適用します。

学習モニターには環境に適合した有効設定（実際の Adam フォールバックを含む）、続いて学習器が読み込んだカメラ画像の実寸・枚数・総画素数を表示します。方式固有の高密度化や表示設定は概要のツールチップで確認できます。タスク記録と結果の `scene.json` 内 `training.summary` に同じ概要を保存し、旧タスクとの互換性を維持します。従来の解像度値は `1/2/4/8` のみが縮小除数で、その他の正値は上流の仕様どおり目標幅として表示し、タスク値を変更しません。ピーク GPU メモリの推定や品質評価ではなく、画像や最適化状態を変更しません。

両方式のモニターは幅と文字サイズに合わせて七つの指標を再配置します。小さいパネルでもスクロールして設定、警告、読みやすい曲線を確認でき、下部パネルを強制的に拡大しません。長いタスク名はプレーンテキストで中央を省略し、ツールチップで原文全体を表示します。有効設定のツールチップも報告全体を保持します。スクロール、サイズ変更、即時の言語切替で学習の記録や処理状態は変わりません。

## Capability matrix and remaining gates

| Pipeline | Original-resolution behavior | Remaining acceptance gate / limitation |
| --- | --- | --- |
| 3DGS | New original-quality preset; volumetric backend's own highest-quality settings; uncapped COLMAP training images | Full 30K representative datasets, VRAM capacity, held-out/metric quality; sparse Adam depends on installed extension |
| 2DGS | Same preset, iteration/ratio and preparation policy; surfel-specific optimization and density guards | Full 30K representative datasets, VRAM capacity and held-out/mesh quality; no 3DGS AA flags copied |
| 2DGS bounded / unbounded TSDF | Consumes the trained 2DGS output; does not redefine this training preset | Voxel/depth-fusion parameters remain separate; a full-resolution Gaussian training preset does not maximize TSDF resolution |
| SuGaR | Consumes a 3DGS result; refinement uses its independent image-size and texture controls | No shared Gaussian-preset override of SuGaR's own image-preparation cap; source-refinement quality and resources need separate validation |
| GS2Mesh | Consumes the result; separate depth-generation downsample control remains explicit | Set its own depth downsample to 1 for full-resolution depth; not automatically changed by the Gaussian training preset |
| OpenMVS texture baking | Original source images retained; independent texture-atlas budget and resolution | No increase to texture budget/max edge by this preset; baking quality remains separately validated |

Input/effective-setting reporting is shared by both Gaussian trainers. TSDF, SuGaR, GS2Mesh and OpenMVS consume their own prepared inputs and expose separate depth/refinement/texture controls; Gaussian training image counts are not presented as their processing settings or optimizer-state resume. Their existing capability gates remain in place.

Automated coverage includes both backend option/CLI mappings, unchanged old profiles, manual overrides, original-quality full-state resume metadata, uncapped COLMAP command generation, five native presets and immediate three-language switching. The opt-in `native.worker.test_training_resolution_cuda` check uses each actual CUDA runtime and installed loader to assert that a **1928 × 1084** source (>1600 px) reaches the training camera and tensor unchanged; the legacy maximum-quality profile remains **964 × 542**. This is a resolution contract check, not a 30K quality benchmark. Existing renderer-equivalence limitations documented in `RECONSTRUCTION_QUALITY.md` are unchanged.

Validated on 2026-10-02 against the new desktop package: **44/44 native CTests**, **137 packaged-build worker checks (8 runtime-specific skips)**, **118 reconstruction/backend checks (1 skip)** and **14 translation-validator checks** passed. Installed dialogs passed immediate-language switching and captured both backends in all three locales. Real 3DGS and 2DGS CUDA loaders retained the full **1928 × 1084** tensor and original RGB values; actual uncapped COLMAP PINHOLE undistortion retained all three owned fixture images at that size. Each CUDA runtime also passed its complete optimizer/Adam/RNG-state pause/resume regression. No full 30K original-resolution quality benchmark is claimed.

The training-summary upgrade was validated on 2026-10-03: **45/45 native CTests**, **154 worker checks (8 runtime-specific skips)**, **165 focused worker/backend checks (2 environment-specific skips: Open3D and the non-training Python's tensor-state check)**, and **14 translation-validator checks** passed. Installed three-language dialogs and monitors were visually checked. In each actual CUDA runtime, camera/tensor dimensions and RGB were retained at **1928 x 1084**, the half-resolution profile loaded **964 x 542**, and the shared loaded-input event helper reported those actual camera dimensions/count/pixels. This loader/event-helper test does not claim to exercise every full worker subprocess event or run a new 30K quality benchmark. Both installed trainers separately passed complete optimizer/Adam/RNG-state resume. The native GPU projected-support test also passes for both Gaussian backends and both resident/compatibility paths; see `RECONSTRUCTION_QUALITY.md` for its scope and the still-open real-model precision gate.

The same validation exposed intermittent Windows access-denied errors when atomically replacing the shared media-import transaction journal. Publication now retries only WinError 5/32, at most five replacements with four 50 ms waits, using the same fsynced temporary file. It rechecks link/reparse targets, never deletes the old journal as a fallback, and still reports persistent failures. Fault-injection tests verify both successful overwrite after a short lock and preservation/rollback of the previous dataset after retry exhaustion. This shared import path applies to all generation pipelines; no training optimizer or image preparation is retried.

The responsive-monitor upgrade was validated on 2026-10-05: **45/45 native CTests**, **154 packaged-build worker checks (8 runtime-specific skips)**, **165 focused worker/backend checks (2 environment-specific skips)** and **15 translation-validator checks** passed. Installed monitors were checked in all three languages for both backends at narrow widths and enlarged fonts, including scrolling to the preserved-height curves. Actual installed trainer subprocesses now additionally run an owned **2-iteration / 4-camera / 32 x 24** fixture in each CUDA runtime: stdout must contain exactly one loaded-input event, all configured fields must match the actual CLI, and its **3072 pixels** must agree with output `cameras.json`; a final PLY must exist. This closes the trainer-subprocess event gate left by the 2026-10-03 helper-only check, not the full GUI/worker-queue end-to-end or 30K reconstruction-quality gates. Original-resolution loader, uncapped COLMAP and complete optimizer-state resume regressions remain passing. No training algorithm, image preparation, preset or renderer math changed in this upgrade; previously documented real-model precision limitations remain open.

中文：媒体导入事务记录遇到 Windows 短暂文件锁时，最多重试 5 次、总等待 0.2 秒，只重试原子发布。持续失败仍报错，并保留旧数据及回滚检查；不会重新执行整个导入或训练。

日本語：メディア取込みのトランザクション記録に Windows の一時的なファイルロックが発生した場合、原子的な公開処理だけを最大 5 回、合計待機 0.2 秒で再試行します。継続的な失敗はエラーとして扱い、旧データの保護とロールバックを維持します。取込み全体や学習を再実行しません。

Primary source references: [official 3DGS camera loader](https://github.com/graphdeco-inria/gaussian-splatting/blob/main/utils/camera_utils.py), [official 2DGS camera loader](https://github.com/hbb1/2d-gaussian-splatting/blob/main/utils/camera_utils.py), and [COLMAP undistortion options](https://github.com/colmap/colmap/blob/main/src/colmap/image/undistortion.h).
