# Native pipeline parity validation — 2026-09-30

[简体中文](#简体中文) · [English](#english) · [日本語](#日本語)

## 简体中文

- 保留远端 `0282b0e` 及其前面的 5 项 Windows/CI 更新，本轮功能提交为 `f505e77`；仅修改原生桌面分支。
- 已在 E: 独立安装 2DGS 源码、Python 3.10 / Torch 2.0.1+cu118、CUDA 11.8、兼容 MSVC 及曲面/近邻扩展。未改动原 3DGS 环境、系统 CUDA 或驱动。软件与预检可自动识别新环境，`pip check` 通过。
- 最终原生 CTest **42/42** 通过。共享后端套件 **68 项，66 通过、2 跳过**（通用解释器缺少 Torch）；两个训练解释器均补验对应张量套件，**16/16** 通过。三语目录为 1204 条 UI 文本、1230 条词条。
- 真实 3DGS CUDA 回归通过：模型、Adam、曝光与 RNG 精确恢复；短样例连续/复跑/续训 PSNR 为 6.470279 / 6.470107 / 6.470104 dB。
- 真实 2DGS 曲面 CUDA 测试通过，包括预检、暂停、完整状态恢复、连续快照、TensorBoard 评估和保留双尺度的最终 PLY。安装后端的 1200 迭代测试使用 32 个高斯，连续/复跑/续训 PSNR 为 **27.170332 / 27.157988 / 27.190107 dB**。精确恢复的是保存状态，不承诺后续 GPU 更新逐位相同；该小型合成场景不是实拍画质基准。
- 安装版在仅系统 PATH 下通过中文、英文、日文界面，以及两种后端的暂停/工程重开 smoke test。已查看三语训练对话框截图；切换语言不重建训练状态。9 项关键安装脚本/许可/依赖清单与源码 SHA-256 一致。

安装包：`native/dist/Gaussian-Scene-Workbench-0.3.1-native-pipeline-parity-win-x64`。测试数据、日志、编译和安装均在 E:，只使用测试自建场景；未操作用户模型。

边界：2DGS 视口仍为薄片近似，未实现共享 GPU 曲面预览；实拍数据、长程法线/畸变正则、TSDF/SuGaR/GS2Mesh/OpenMVS 的原生阶段任务和恢复仍按[能力表](../GENERATION_PIPELINES.md)跟进。上游研究用途许可证保留，不作 MIT 或商用重新授权。

## English

The native branch retains all five remote Windows/CI updates through `0282b0e`. Feature commit: `f505e77`. An isolated 2DGS Python/CUDA/compiler/surfel runtime is installed on E:, automatically discovered and passes `pip check`; existing 3DGS and system CUDA/drivers are unchanged.

CTest passed 42/42. Backend contracts passed 66/68 with two missing-Torch skips in the generic interpreter; both training interpreters passed the corresponding 16/16 tensor tests. Real CUDA 3DGS regression passed. The installed 2DGS adapter passed 1200 iterations of pause/restart, exact saved model/Adam/RNG restoration, continuous snapshots, TensorBoard evaluation and native two-scale PLY output. Synthetic-fixture PSNR continuous/repeat/resume: 27.170332 / 27.157988 / 27.190107 dB. This is continuity validation, not a real-data quality benchmark or a promise of bitwise-identical later GPU updates.

Installed zh_CN/en_US/ja_JP UI and both backend project-reopen smoke tests passed under a system-only PATH. Training-dialog screenshots were inspected. Nine installed backend/license/requirements files match source SHA-256. All fixtures and artifacts remain on E:; no user models were used. Exact surfel display/shared GPU preview, long-run regularization, representative datasets and native staged mesh/recovery acceptance remain open as documented in the capability matrix. Upstream research-use licensing is unchanged.

## 日本語

遠隔の Windows/CI 更新 5 件（`0282b0e` まで）を保持し、ネイティブ分岐に `f505e77` を追加しました。E: に独立した 2DGS・Python・CUDA・コンパイラー・サーフェル拡張を導入し、自動検出と `pip check` に合格。既存 3DGS とシステム CUDA・ドライバーは変更していません。

CTest は 42/42、バックエンドは 68 件中 66 件成功・Torch 不在の 2 件スキップ。両学習環境で対応するテンソル試験 16/16 を補完しました。3DGS 実 CUDA 回帰、インストール版 2DGS の 1200 反復、状態・Adam・乱数の完全復元、連続プレビュー、TensorBoard 評価、二尺度 PLY 出力も合格。合成場面の連続／再実行／再開 PSNR は 27.170332 / 27.157988 / 27.190107 dB。実写品質や、その後の GPU 更新のビット単位一致を保証するものではありません。

システム PATH のみで三言語 UI と両方式の一時停止後の再オープン試験に合格し、学習ダイアログの画像も確認しました。インストールした主要 9 ファイルの SHA-256 はソースと一致します。ユーザーモデルは使わず、検証データは E: に保存。厳密なサーフェル表示・GPU 共有、長期正則化、実写データ、段階別メッシュ生成・復旧の検証は能力表に残しています。元の研究用途ライセンスを保持しています。
