# Actual training demo assets

<div lang="en">

## Video and preview

[Continuous native-operation MP4](gsw-native-operation.en.mp4) (73.35 seconds, English UI) · Subtitles: [English](gsw-native-operation.en.srt), <a href="gsw-native-operation.zh-CN.srt" lang="zh-CN">简体中文</a>, <a href="gsw-native-operation.ja.srt" lang="ja">日本語</a>. The three localized GIFs are 16.63-second selected excerpts, not continuous captures: [English](gsw-native-preview.en.gif), <a href="gsw-native-preview.zh-CN.gif" lang="zh-CN">中文</a>, <a href="gsw-native-preview.ja.gif" lang="ja">日本語</a>.

[Remote recording run 40817099235](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40817099235) passed actual inputs, both original-coordinate PLY payload checks, and a numeric move followed by exact undo. The viewer is the genuine, source-bound `0.3.9-native-preview` build [`a5393be`](https://github.com/fubukisama/Gaussian-Scene-Workbench/tree/a5393be3772b8d49c6be93a767dc7dfd5abcec5a), under its former product name; the external capture driver is [`6be3d75`](https://github.com/fubukisama/Gaussian-Scene-Workbench/tree/6be3d756eff0db2f44209e2ea51cfdb11b6140bb). Neither is represented as acceptance of the latest installed application.

The full video retains all 1,327 decoded source frames and original timing. Its 1600 × 936 application area is neither cropped nor scaled; an 88-pixel English caption footer is added outside it. There are no cuts, motion interpolation, synthetic UI frames or added audio. H.264 re-encoding is lossy. The GIFs downsample the real UI and select source intervals, listed in the [composition receipt](composition-receipt.json). External captions use Segoe UI for Latin text, Microsoft YaHei for Chinese and Yu Gothic for Japanese; subtitle players choose their own fonts.

Media review on 2026-10-29 inspected actual video samples, both final models and export dialogs, move/undo states and localized preview/caption frames. Controls and captions remain readable and complete, with no unrelated local desktop content. The short-trained models retain visible low-opacity floaters; they were not retouched. This review is a presentation check, not model-quality or physical-accuracy acceptance. The generated receipt deliberately retains `composed_not_visually_accepted`: file generation does not grant visual or research acceptance.

## Training source

This directory contains an original synthetic demonstration, not user research data. [readme-training-demo.zip](readme-training-demo.zip) contains six real PLY snapshots (initialization, step 600 and step 1500 for both 3DGS and 2DGS), one training RGB image, and hash-bound manifests. ZIP SHA256: `58624bea4b458e5122d6f544ef96ab09fcd8e440cf99523c3724dae4bc6c9c44`.

The two native backends actually optimized 24 of 30 authored 320 × 320 views; validation and test images were excluded. Initialization used 12,000 neutral random-volume points, seed 73129 and explicit bounds, without truth depth, normals or surface points. Units remain unknown/arbitrary. Final counts are 13,818 (3DGS) and 9,532 (2DGS), with densification enabled. This small 1,500-step demonstration is not a convergence, speed, geometric-accuracy or full-state-resume benchmark.

Training source: [`f997710`](https://github.com/fubukisama/Gaussian-Scene-Workbench/tree/f9977105ee5024aa94c9b263410f84f0fed77862). The source helpers added with these assets reproduce the original CPU dataset, supervise bounded training and package sanitized snapshots: `scripts/readme_demo_dataset.py`, `scripts/readme_demo_training.py`, and `scripts/package_readme_training_demo.py`. Compatible external CUDA training environments are required; these are not bundled by the demo.

The dispatch-only README recording workflow uses real native inputs on a disposable remote Windows runner. It replays the retained training snapshots; it does **not** pretend the runner performs live GPU training. Its pinned historical viewer/source identity is separate from the training identity and from current desktop-delivery acceptance. Per-snapshot file-write times include startup and I/O and are only approximate observations. No user photographs, projects, recordings or checkpoints are included.

</div>

<div lang="zh-CN">

## 视频与预览

[完整原生操作 MP4](gsw-native-operation.en.mp4) 为 73.35 秒英文界面原速录像，另附[中文字幕](gsw-native-operation.zh-CN.srt)。[中文动图](gsw-native-preview.zh-CN.gif) 为 16.63 秒真实选段，包含两个后端的生成阶段、视角操作、移动撤销和导出，不是连续训练录像。

录制由[远端运行 40817099235](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40817099235) 完成，核对了两个导出文件的原始高斯数据，以及移动后撤销恢复的状态。查看器为源码绑定的历史 `a5393be` / `0.3.9-native-preview`，仍使用旧产品名；录制器为 `6be3d75`，训练源码身份见下文，不能据此宣称当前安装版已验收。

完整版保留全部 1,327 个源帧与原始时序，1600 × 936 应用区域不裁切、不缩放，字幕底栏在画面外增加。无剪切、插帧、合成 UI 或新增音频；H.264 转码有损。动图仅缩小并选取实际片段，来源区间及哈希见[制作清单](composition-receipt.json)。中文、日文、拉丁文字分别使用微软雅黑、游ゴシック和 Segoe UI；独立字幕的字体由播放器控制。

2026-10-29 已检查实际视频抽样帧、两种模型及导出弹窗、移动撤销与三语字幕，未裁去控件或修饰模型。模型仍可见低透明度浮点，本次只验收演示素材，不代表训练质量、实物精度或当前软件全套验收。生成清单的 `composed_not_visually_accepted` 保持原样，不将“生成文件”自动升级为视觉或研究结论。

## 训练来源

此目录仅包含原创合成演示，不是用户研究数据。[readme-training-demo.zip](readme-training-demo.zip) 包含 3DGS、2DGS 各自的初始化、第 600 步和第 1500 步真实 PLY、一张训练图及哈希绑定清单。

两个原生后端均实际训练 1,500 步，仅使用 30 张原创 320 × 320 图像中的 24 张训练图，验证图与测试图不参与优化。初始化为 12,000 个中性灰随机体积点，种子 73129、范围显式指定，不使用真值深度、法向或表面点；单位保持未知／任意。启用增密后的最终数量分别为 13,818 和 9,532。本演示不证明长期收敛、训练速度、几何精度或完整状态续训。

远端原生操作录像回放上述真实训练快照，并非远端实时 GPU 训练。训练源码身份、历史查看器身份与当前软件交付验收分别记录；阶段文件写入时间包含启动与 I/O，仅为近似观测。素材不包含用户照片、工程、录屏或检查点。复现脚本及训练源码链接见上方，需自行配置兼容的外部 CUDA 运行库。

</div>

<div lang="ja">

## 動画とプレビュー

[ネイティブ操作の全編 MP4](gsw-native-operation.en.mp4) は英語 UI の 73.35 秒の等速録画で、[日本語字幕](gsw-native-operation.ja.srt)を用意しています。[日本語 GIF](gsw-native-preview.ja.gif) は実際の映像を抜粋した 16.63 秒で、両バックエンドの保存結果、視点操作、移動と取り消し、出力を示します。連続した学習録画ではありません。

[リモート実行 40817099235](https://github.com/fubukisama/Gaussian-Scene-Workbench/actions/runs/40817099235) で、両出力の元ガウシアンデータおよび移動後の取り消し状態を検証しました。ビューアーは旧製品名の履歴ビルド `a5393be` / `0.3.9-native-preview`、録画ドライバーは `6be3d75` です。学習ソースとは別の識別情報であり、現在のインストール版の受入検証を意味しません。

全編は元の 1,327 フレームと時系列を保持し、1600 × 936 のアプリ領域を切り抜き・拡縮せず、外側に字幕フッターを追加しています。カット、フレーム補間、合成 UI、追加音声はありません。H.264 再符号化は非可逆です。GIF の抜粋区間とハッシュは[制作マニフェスト](composition-receipt.json)を参照してください。ラテン文字は Segoe UI、中国語は Microsoft YaHei、日本語は游ゴシックを使用し、字幕プレーヤーの字体は各プレーヤーに依存します。

2026-10-29 に実際の動画の抽出フレーム、両モデルと出力ダイアログ、移動と取り消し、各言語字幕を確認しました。モデルには低不透明度の浮遊ガウシアンが残り、修整していません。これはデモ素材の表示確認であり、品質・実物精度・現行版全体の検証ではありません。生成マニフェストの `composed_not_visually_accepted` は変更せず、ファイル生成だけで視覚・研究上の合格としません。

## 学習データの出所

このディレクトリは独自の合成デモのみを含み、ユーザーの研究データではありません。[readme-training-demo.zip](readme-training-demo.zip) には、3DGS と 2DGS それぞれの初期化・600 ステップ・1,500 ステップの実際の PLY、学習用画像 1 枚、およびハッシュで対応付けたマニフェストを収録しています。

両ネイティブバックエンドで実際に 1,500 ステップ学習し、独自に生成した 320 × 320 の 30 視点のうち 24 視点のみを最適化に使用しました。検証・テスト画像は使用していません。初期化は明示範囲内の中立グレーのランダム点 12,000 個、シード 73129 で、正解深度・法線・表面点を使いません。単位は未知／任意のままです。高密度化を有効にした最終数は 13,818 個と 9,532 個です。長期収束、速度、幾何精度、完全状態からの再開を示すベンチマークではありません。

リモートのネイティブ操作録画は実際の学習スナップショットの再生であり、リモートでのリアルタイム GPU 学習ではありません。学習ソース、履歴のあるビューアー、現在の配布版の受入検証は区別して記録します。段階ごとのファイル書き込み時刻は起動と I/O を含む概算です。ユーザーの写真、プロジェクト、録画、チェックポイントは含みません。再現用スクリプトと学習ソースは上記を参照してください。互換性のある外部 CUDA 実行環境が必要です。

</div>
