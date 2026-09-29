# Model-generation pipeline parity / 模型生成链路对齐 / 生成パイプラインの整合

[简体中文](#简体中文) · [English](#english) · [日本語](#日本語)

## 简体中文

自 2026-09-23 起，3DGS、2DGS 和其他已支持生成链路共同规划、同步验收。不能把显示导出文件、重新开始训练或重跑阶段称为完整状态续训。运行环境可用性与软件已实现能力分开检查。

本批（2026-09-26）先补齐原生 2DGS 的监督训练入口、迭代边界暂停、优化器状态续训、连续快照、结构化指标和双尺度 PLY 显示。直接适配官方 [hbb1/2d-gaussian-splatting](https://github.com/hbb1/2d-gaussian-splatting) 的 `train.py`，固定提交 `f3e3b9fa67bbd1c75e05167ff37391d8dab2a678`；保留原损失、法线/畸变正则项调度和密度控制算法，运行时调用其 `GaussianModel.capture/restore`。不修改用户的外部源码或模型。

| 链路 | 原生入口 / 暂停续训 | 过程显示 | 剩余验收 |
| --- | --- | --- | --- |
| 3DGS | 有 / 完整状态 | 稀疏点云 → 初始化 → GPU 或快照 → 结果 | GPU 共享依赖平台；历史曲线未跨进程恢复 |
| 2DGS | 已接入 / 完整状态，真实 CUDA 小样例通过 | 同一阶段顺序，连续限量快照；双尺度 PLY 薄片近似 | 代表性实拍数据质量验收、精确曲面视口、共享 GPU 预览 |
| 2DGS bounded / unbounded | 共享后端已有，原生类型化任务尚缺 | 原生阶段预览尚缺 | TSDF 阶段产物、取消、输出校验及重开；不是优化器续训 |
| SuGaR | 共享后端已有，原生适配尚缺 | 原生阶段预览尚缺 | 独立优化器、网格与纹理各阶段的状态和验收 |
| GS2Mesh | 共享后端已有，原生适配尚缺 | 原生阶段预览尚缺 | 深度估计/融合阶段完成产物校验与安全重试 |
| OpenMVS | 共享后端已有，原生适配尚缺 | 原生阶段预览尚缺 | 稠密点云/网格/纹理各进程的进度、取消与阶段恢复 |

机器可检查矩阵在 `native/worker/generation_capabilities.py`，测试要求覆盖共享后端的全部训练/网格方法，并为每个能力差异提供原因和下一验收项。未完成项不得显示为已完成。后续优先接入原生类型化网格阶段任务，再逐个补齐阶段预览与恢复；不将多个研究后端简单套用同一检查点格式。

2DGS 部署：2026-09-30 按用户授权已在 E 盘补齐独立环境，未替换系统 CUDA、驱动或原 3DGS 环境。软件自动查找安装盘的 `Gaussian-Scene-Workbench-Runtime/2dgs` 和 `conda/envs/gsw_2dgs/python.exe`；仍支持 `TWO_DGS_DIR` / `TWO_DGS_PYTHON` 显式覆盖，以及源码下 `.venv/Scripts/python.exe`。显式错误路径不会悄悄回退。预检验证实际曲面扩展。普通启动只检测环境，不自动下载安装。

真实 RTX 4070 Laptop GPU 测试已通过：16 次迭代检查点/Adam/RNG 精确恢复；1200 次迭代连续预览、暂停重启、TensorBoard 评估和双尺度 PLY 完成输出正常。后者连续/复跑/续训 PSNR 为 27.176575 / 27.137341 / 27.168127 dB（32 个高斯、合成小样例），不是实拍质量保证。长程法线/畸变正则阶段、真实数据集质量和精确曲面视口仍需进一步验收。

状态包括两个原生尺度、SH、密度统计、Adam、Python/NumPy/Torch/CUDA RNG、剩余相机采样序列、迭代和三个平滑损失。没有曝光优化器的 2DGS 不伪造曝光状态。清单记录后端，拒绝交叉加载；旧版未标后端的原生检查点仅视为 3DGS。2DGS 续训另验证源实现文件指纹。观察用法向厚度仅用于显示，原始 PLY/训练张量/检查点保持两个尺度；该仿射薄片预览不等同于透视准确的曲面光栅化。

## English

All generation workflows must advance together, with explicit technical differences and acceptance gates. Runtime installation is separate from implemented support. This batch adapts official 2DGS `train.py` at the revision above, preserving losses, regularization schedules and density control. It adds supervised execution, post-optimizer pause, full-state resume, structured telemetry and bounded continuous snapshots. External source files remain untouched. The Gaussian-Splatting research/evaluation license is retained in `native/worker/licenses/2dgs-LICENSE.md`; this is not an MIT relicensing or commercial-use grant.

3DGS keeps shared-GPU/snapshot previews. 2DGS uses snapshots and a display-only affine thin-disk approximation; exact surfel rendering and shared GPU preview remain outstanding. Two-scale source data is unchanged. 2DGS checkpoint state includes native model/Adam/density statistics, RNG, camera sampling, iteration, elapsed time and all smoothed losses, but no invented exposure optimizer. Backend mismatches are rejected before deserialization. Old untagged native manifests are 3DGS only. Input and implementation fingerprints prevent incompatible continuation; explicit trust confirmation is still required for pickle.

Bounded/unbounded TSDF, SuGaR, GS2Mesh and OpenMVS exist in the shared backend but still lack native typed stage jobs. They need stage-aware progress/preview/cancellation, validated artifacts and safe recovery, not a fake Gaussian optimizer resume. The executable capability matrix covers every training/mesh method and records each limitation and next gate. Native staged meshing is the next integration priority.

An isolated runtime was installed on E: with user authorization on 2026-09-30, without changing system CUDA/drivers or the 3DGS environment. Discovery checks `Gaussian-Scene-Workbench-Runtime/2dgs` and `conda/envs/gsw_2dgs/python.exe` on the installation drive. Explicit `TWO_DGS_DIR` / `TWO_DGS_PYTHON` overrides and source-local `.venv/Scripts/python.exe` remain supported. Invalid explicit overrides fail rather than silently selecting something else. Normal application startup only probes the environment; it does not download dependencies.

Real RTX 4070 Laptop GPU tests passed: exact serialized model/Adam/RNG restoration, a 1200-iteration continuous-preview/pause/restart run, TensorBoard evaluation, and final two-scale PLY. Continuous/repeat/resume PSNR was 27.176575 / 27.137341 / 27.168127 dB on a synthetic 32-Gaussian fixture, not a real-data quality guarantee. Long-run normal/distortion regularization, representative datasets, exact surfel viewport rendering and staged native mesh jobs still need acceptance.

## 日本語

3DGS、2DGS、および他の生成パイプラインを共通の計画・受入基準で更新します。実行環境の導入と実装済み機能は別に確認します。本段階では上記コミットの公式 2DGS `train.py` を直接移植し、損失、法線・歪み正則化のスケジュール、密度制御を維持します。監視下での実行、最適化器更新後の一時停止、完全な状態からの再開、構造化メトリクス、連続スナップショットを追加します。外部ソースは変更しません。研究・評価用途の元ライセンスを同梱し、MIT への変更や商用利用許諾は行いません。

3DGS は GPU 共有またはスナップショットを使用。2DGS は薄い円盤のアフィン近似表示で、厳密なサーフェル描画と GPU 共有は未対応です。表示用の厚みを元 PLY や学習テンソルへ書き込みません。チェックポイントは二つの尺度、SH、密度統計、Adam、乱数、カメラのサンプル列、反復、経過時間、三種類の平滑化損失を保持します。存在しない露出最適化器は追加しません。異なる手法の状態は読み込み前に拒否し、旧形式の原生マニフェストは 3DGS のみと扱います。入力・実装ファイルの指紋と明示的な信頼確認も必要です。

bounded/unbounded TSDF、SuGaR、GS2Mesh、OpenMVS は共通バックエンドにありますが、ネイティブ段階別ジョブ、進捗・プレビュー、取消、成果物検証と安全な復旧は未完了です。最適化器の再開と同一視しません。機械検証可能な能力表に全手法の制限と次の受入項目を記録し、次に段階別メッシュジョブの統合を進めます。

2026-09-30、ユーザーの許可に基づいて E: に独立環境を導入しました。システム CUDA・ドライバーと既存 3DGS 環境は変更しません。インストール先ドライブの `Gaussian-Scene-Workbench-Runtime/2dgs` と `conda/envs/gsw_2dgs/python.exe` を自動検出し、`TWO_DGS_DIR` / `TWO_DGS_PYTHON` とソース内 `.venv/Scripts/python.exe` も使用できます。明示指定が不正な場合は黙って別環境へ切り替えません。通常起動時は検査のみで、自動ダウンロードは行いません。

実 RTX 4070 Laptop GPU で状態・Adam・乱数の完全復元、1200 反復の連続プレビューと再開、TensorBoard 評価、二尺度 PLY 出力を検証しました。合成 32 ガウシアンの連続／再実行／再開 PSNR は 27.176575 / 27.137341 / 27.168127 dB です。実写品質の保証ではありません。長期の法線・歪み正則化、代表的データセット、厳密なサーフェル表示、段階別メッシュ処理は引き続き検証が必要です。

## Verified Windows runtime / 已验证 Windows 环境 / 検証済み Windows 環境

- Official source: `hbb1/2d-gaussian-splatting@f3e3b9fa67bbd1c75e05167ff37391d8dab2a678`; recursive submodules retained; no source patches.
- Python 3.10.21, PyTorch 2.0.1+cu118, torchvision 0.15.2+cu118; Python dependencies pinned in `scripts/requirements-native-2dgs.txt`.
- CUDA 11.8.89 compiler/runtime/headers from NVIDIA's Conda channel, installed in `E:/Gaussian-Scene-Workbench-Runtime/cuda-11.8`.
- MSVC 14.38.33130 / compiler 19.38.33145, official Microsoft Visual Studio catalog payloads verified against catalog SHA-256 and extracted into an isolated E: directory. Existing Windows SDK 10.0.26100.0 used read-only. No administrator or system-compiler changes.
- CUDA extensions built for the local GPU (`TORCH_CUDA_ARCH_LIST=8.9`), so other GPU architectures require rebuilding. Build uses `DISTUTILS_USE_SDK=1`, `MAX_JOBS=4`, explicit MSVC/SDK include/library paths and `pip install --no-build-isolation` for both upstream submodules.
- NumPy 1.26.4 avoids the NumPy 2 / Torch 2.0 ABI conflict. Matplotlib 3.8.4 retains the upstream TensorBoard color-map API; the native worker uses the non-interactive Agg backend.

以上是本机隔离部署记录，不把编译工具或研究代码重新授权为 MIT。 / This is a local isolated-install record, not MIT relicensing of the compiler or research code. / 上記は独立導入の記録であり、コンパイラーや研究コードを MIT に変更するものではありません。

## Developer checks

```text
python -B -m unittest native.worker.test_two_dgs_adapter native.worker.test_training_checkpoint native.worker.test_training_preview
python -B -m unittest crop_editor.tests.test_training_telemetry
python scripts/native_i18n.py
python scripts/test_native_i18n.py
ctest --test-dir native/build-unity-gizmo --output-on-failure
```

Run tensor tests with the configured training Python. Do not mix the server telemetry suite's optional-dependency stubs into that process. `native_2dgs_training_resume` validates the owned desktop-worker lifecycle, not a CUDA training run.

For the real surfel test, set `TWO_DGS_DIR` to the installed source, `GSW_CHECKPOINT_TEST_BACKEND=2dgs`, optionally `GSW_CHECKPOINT_TEST_ITERATIONS=1200`, then run `python -B -m unittest native.worker.test_training_checkpoint_cuda` using the 2DGS interpreter and DLL PATH. `GSW_CHECKPOINT_TEST_ROOT` can target the packaged backend. Keep TEMP/TMP on a data drive.
