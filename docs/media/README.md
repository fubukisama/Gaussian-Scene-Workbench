# Actual training demo assets

<div lang="en">

This directory contains an original synthetic demonstration, not user research data. [readme-training-demo.zip](readme-training-demo.zip) contains six real PLY snapshots (initialization, step 600 and step 1500 for both 3DGS and 2DGS), one training RGB image, and hash-bound manifests. ZIP SHA256: `58624bea4b458e5122d6f544ef96ab09fcd8e440cf99523c3724dae4bc6c9c44`.

The two native backends actually optimized 24 of 30 authored 320 × 320 views; validation and test images were excluded. Initialization used 12,000 neutral random-volume points, seed 73129 and explicit bounds, without truth depth, normals or surface points. Units remain unknown/arbitrary. Final counts are 13,818 (3DGS) and 9,532 (2DGS), with densification enabled. This small 1,500-step demonstration is not a convergence, speed, geometric-accuracy or full-state-resume benchmark.

Training source: [`f997710`](https://github.com/fubukisama/Gaussian-Scene-Workbench/tree/f9977105ee5024aa94c9b263410f84f0fed77862). The source helpers added with these assets reproduce the original CPU dataset, supervise bounded training and package sanitized snapshots: `scripts/readme_demo_dataset.py`, `scripts/readme_demo_training.py`, and `scripts/package_readme_training_demo.py`. Compatible external CUDA training environments are required; these are not bundled by the demo.

The dispatch-only README recording workflow uses real native inputs on a disposable remote Windows runner. It replays the retained training snapshots; it does **not** pretend the runner performs live GPU training. Its pinned historical viewer/source identity is separate from the training identity and from current desktop-delivery acceptance. Per-snapshot file-write times include startup and I/O and are only approximate observations. No user photographs, projects, recordings or checkpoints are included.

</div>

<div lang="zh-CN">

此目录仅包含原创合成演示，不是用户研究数据。[readme-training-demo.zip](readme-training-demo.zip) 包含 3DGS、2DGS 各自的初始化、第 600 步和第 1500 步真实 PLY、一张训练图及哈希绑定清单。

两个原生后端均实际训练 1,500 步，仅使用 30 张原创 320 × 320 图像中的 24 张训练图，验证图与测试图不参与优化。初始化为 12,000 个中性灰随机体积点，种子 73129、范围显式指定，不使用真值深度、法向或表面点；单位保持未知／任意。启用增密后的最终数量分别为 13,818 和 9,532。本演示不证明长期收敛、训练速度、几何精度或完整状态续训。

远端原生操作录像回放上述真实训练快照，并非远端实时 GPU 训练。训练源码身份、历史查看器身份与当前软件交付验收分别记录；阶段文件写入时间包含启动与 I/O，仅为近似观测。素材不包含用户照片、工程、录屏或检查点。复现脚本及训练源码链接见上方，需自行配置兼容的外部 CUDA 运行库。

</div>

<div lang="ja">

このディレクトリは独自の合成デモのみを含み、ユーザーの研究データではありません。[readme-training-demo.zip](readme-training-demo.zip) には、3DGS と 2DGS それぞれの初期化・600 ステップ・1,500 ステップの実際の PLY、学習用画像 1 枚、およびハッシュで対応付けたマニフェストを収録しています。

両ネイティブバックエンドで実際に 1,500 ステップ学習し、独自に生成した 320 × 320 の 30 視点のうち 24 視点のみを最適化に使用しました。検証・テスト画像は使用していません。初期化は明示範囲内の中立グレーのランダム点 12,000 個、シード 73129 で、正解深度・法線・表面点を使いません。単位は未知／任意のままです。高密度化を有効にした最終数は 13,818 個と 9,532 個です。長期収束、速度、幾何精度、完全状態からの再開を示すベンチマークではありません。

リモートのネイティブ操作録画は実際の学習スナップショットの再生であり、リモートでのリアルタイム GPU 学習ではありません。学習ソース、履歴のあるビューアー、現在の配布版の受入検証は区別して記録します。段階ごとのファイル書き込み時刻は起動と I/O を含む概算です。ユーザーの写真、プロジェクト、録画、チェックポイントは含みません。再現用スクリプトと学習ソースは上記を参照してください。互換性のある外部 CUDA 実行環境が必要です。

</div>
