# Model-generation pipeline parity / 模型生成链路对齐 / 生成パイプラインの整合

[简体中文](#简体中文) · [English](#english) · [日本語](#日本語)

## 简体中文

自 2026-09-23 起，3DGS、2DGS 和其他已支持生成链路共同规划、同步验收。不能把显示导出文件、重新开始训练或重跑阶段称为完整状态续训。运行环境可用性与软件已实现能力分开检查。

先前批次（2026-09-26）补齐原生 2DGS 的监督训练入口、迭代边界暂停、优化器状态续训、连续快照、结构化指标和双尺度 PLY 显示。直接适配官方 [hbb1/2d-gaussian-splatting](https://github.com/hbb1/2d-gaussian-splatting) 的 `train.py`，固定提交 `f3e3b9fa67bbd1c75e05167ff37391d8dab2a678`；保留原损失、法线/畸变正则项调度和密度控制算法，运行时调用其 `GaussianModel.capture/restore`。不修改用户的外部源码或模型。

| 链路 | 原生入口 / 暂停续训 | 过程显示 | 剩余验收 |
| --- | --- | --- | --- |
| 3DGS | 有 / 完整状态 | 稀疏点云 → 初始化 → GPU 或快照 → 结果 | GPU 共享依赖平台；历史曲线未跨进程恢复 |
| 2DGS | 已接入 / 完整状态，真实 CUDA 小样例通过 | 同一阶段顺序，连续限量快照；双尺度 PLY 薄片近似 | 代表性实拍数据质量验收、精确曲面视口、共享 GPU 预览 |
| 2DGS bounded / unbounded | 原生独立阶段任务 / 不支持优化器续训 | 原画面保留 → 验证后的网格 → 可选 OpenMVS 材质预览 | 有界真实 CUDA 小样例通过；无界实拍质量、逐体素预览与阶段恢复待验收 |
| SuGaR | 原生类型化任务 / 无完整状态恢复 | 完成网格 → 自身材质经共享桥接自动显示 | 真实细化与材质环境、实拍质量、阶段恢复待验收 |
| GS2Mesh | 原生类型化任务 / 无优化器恢复 | 完成网格 → 可选 OpenMVS 材质经共享桥接自动显示 | 真实立体深度环境、实拍质量、阶段重试待验收 |
| OpenMVS | 原生可选照片纹理阶段 / 无优化器恢复 | 烘焙时保留网格 → 验证后的贴图网格 | 真实照片烘焙质量与独立稠密重建入口仍待验收/实现 |

机器可检查矩阵在 `native/worker/generation_capabilities.py`，测试要求覆盖共享后端的全部训练/网格方法，并为每个能力差异提供原因和下一验收项。2026-09-30 第五批加入原生“工作流 → 生成网格”及独立输出、取消、完整三角形校验、成功/部分成果追加和工程重开；第六批补齐共享材质桥接（`texture_preview` → `texture_ready`），四种方法的生成 OBJ/MTL/图片可转为原生自有 PLY/图集并自动显示。原始资产保持不变，失败保留前序成果。该内部桥接不是任意 OBJ 导入，仅支持不透明漫反射三角形、非平铺 UV；原分辨率图集受 8192 px / 256 MiB 图片工作预算约束，其他着色语义明确拒绝。详细用法与剩余实拍质量/环境边界见 [MESH_GENERATION.md](MESH_GENERATION.md)。不将多个研究后端套用同一检查点格式，也不将材质预览视为优化器恢复。

2DGS 部署：2026-09-30 按用户授权已在 E 盘补齐独立环境，未替换系统 CUDA、驱动或原 3DGS 环境。软件自动查找安装盘的 `Gaussian-Scene-Workbench-Runtime/2dgs` 和 `conda/envs/gsw_2dgs/python.exe`；仍支持 `TWO_DGS_DIR` / `TWO_DGS_PYTHON` 显式覆盖，以及源码下 `.venv/Scripts/python.exe`。显式错误路径不会悄悄回退。预检验证实际曲面扩展。普通启动只检测环境，不自动下载安装。

真实 RTX 4070 Laptop GPU 测试已通过：16 次迭代检查点/Adam/RNG 精确恢复；1200 次迭代连续预览、暂停重启、TensorBoard 评估和双尺度 PLY 完成输出正常。后者连续/复跑/续训 PSNR 为 27.176575 / 27.137341 / 27.168127 dB（32 个高斯、合成小样例），不是实拍质量保证。长程法线/畸变正则阶段、真实数据集质量和精确曲面视口仍需进一步验收。

状态包括两个原生尺度、SH、密度统计、Adam、Python/NumPy/Torch/CUDA RNG、剩余相机采样序列、迭代和三个平滑损失。没有曝光优化器的 2DGS 不伪造曝光状态。清单记录后端，拒绝交叉加载；旧版未标后端的原生检查点仅视为 3DGS。2DGS 续训另验证源实现文件指纹。观察用法向厚度仅用于显示，原始 PLY/训练张量/检查点保持两个尺度；该仿射薄片预览不等同于透视准确的曲面光栅化。

## English

All generation workflows must advance together, with explicit technical differences and acceptance gates. Runtime installation is separate from implemented support. The earlier training batch adapts official 2DGS `train.py` at the revision above, preserving losses, regularization schedules and density control. It adds supervised execution, post-optimizer pause, full-state resume, structured telemetry and bounded continuous snapshots. External source files remain untouched. The Gaussian-Splatting research/evaluation license is retained in `native/worker/licenses/2dgs-LICENSE.md`; this is not an MIT relicensing or commercial-use grant.

3DGS keeps shared-GPU/snapshot previews. 2DGS uses snapshots and a display-only affine thin-disk approximation; exact surfel rendering and shared GPU preview remain outstanding. Two-scale source data is unchanged. 2DGS checkpoint state includes native model/Adam/density statistics, RNG, camera sampling, iteration, elapsed time and all smoothed losses, but no invented exposure optimizer. Backend mismatches are rejected before deserialization. Old untagged native manifests are 3DGS only. Input and implementation fingerprints prevent incompatible continuation; explicit trust confirmation is still required for pickle.

On 2026-09-30, bounded/unbounded TSDF, SuGaR and GS2Mesh gained native typed, isolated stage jobs. OpenMVS is an optional photo-texturing stage, not a standalone native dense reconstruction pipeline. Jobs preserve the current view, publish validated completed meshes, support cancellation, append successful/partial geometry, and persist model associations across project reopen. See [MESH_GENERATION.md](MESH_GENERATION.md). Per-voxel/depth previews, stage recovery and full optimizer resume are not implemented. The executable matrix records runtime and quality acceptance gates separately. The earlier training acceptance below does not describe this mesh batch.

Batch six adds the same generated-material adapter to all four mesh methods (`texture_preview` → `texture_ready`). SuGaR supplies its own materials; TSDF/GS2Mesh may use OpenMVS. Owned OBJ/MTL/images become a native-owned PLY/atlas and automatically appear in the viewport without changing original assets. Preparation failures preserve earlier validated stages. This is not general OBJ import: only opaque diffuse triangles with non-tiled UVs are supported; full-resolution packing must fit 8192 px per atlas dimension and a 256 MiB image working budget, with explicit rejection of unsupported shader semantics. Real-photo backend/material quality and runtime gates remain unchanged; a preview is not optimizer resume.

An isolated runtime was installed on E: with user authorization on 2026-09-30, without changing system CUDA/drivers or the 3DGS environment. Discovery checks `Gaussian-Scene-Workbench-Runtime/2dgs` and `conda/envs/gsw_2dgs/python.exe` on the installation drive. Explicit `TWO_DGS_DIR` / `TWO_DGS_PYTHON` overrides and source-local `.venv/Scripts/python.exe` remain supported. Invalid explicit overrides fail rather than silently selecting something else. Normal application startup only probes the environment; it does not download dependencies.

Real RTX 4070 Laptop GPU tests passed: exact serialized model/Adam/RNG restoration, a 1200-iteration continuous-preview/pause/restart run, TensorBoard evaluation, and final two-scale PLY. Continuous/repeat/resume PSNR was 27.176575 / 27.137341 / 27.168127 dB on a synthetic 32-Gaussian fixture, not a real-data quality guarantee. Long-run normal/distortion regularization, representative datasets and exact surfel viewport rendering still need acceptance. A separate bounded-TSDF CUDA test produced 700 vertices / 1274 faces; other mesh runtime/material quality gates remain in MESH_GENERATION.md.

## 日本語

3DGS、2DGS、および他の生成パイプラインを共通の計画・受入基準で更新します。実行環境の導入と実装済み機能は別に確認します。先行する学習段階では上記コミットの公式 2DGS `train.py` を直接移植し、損失、法線・歪み正則化のスケジュール、密度制御を維持します。監視下での実行、最適化器更新後の一時停止、完全な状態からの再開、構造化メトリクス、連続スナップショットを追加します。外部ソースは変更しません。研究・評価用途の元ライセンスを同梱し、MIT への変更や商用利用許諾は行いません。

3DGS は GPU 共有またはスナップショットを使用。2DGS は薄い円盤のアフィン近似表示で、厳密なサーフェル描画と GPU 共有は未対応です。表示用の厚みを元 PLY や学習テンソルへ書き込みません。チェックポイントは二つの尺度、SH、密度統計、Adam、乱数、カメラのサンプル列、反復、経過時間、三種類の平滑化損失を保持します。存在しない露出最適化器は追加しません。異なる手法の状態は読み込み前に拒否し、旧形式の原生マニフェストは 3DGS のみと扱います。入力・実装ファイルの指紋と明示的な信頼確認も必要です。

2026-09-30、bounded/unbounded TSDF、SuGaR、GS2Mesh にネイティブ段階別ジョブを追加しました。OpenMVS は任意の写真テクスチャ工程のみで、独立した密な再構築の入口ではありません。処理中の画面維持、取消、三角形全体の検証、完成・部分成果の追加、工程再読込での関連付けを実装しました。[MESH_GENERATION.md](MESH_GENERATION.md) を参照してください。逐次深度・ボクセルプレビュー、段階別復旧、最適化器の完全再開は未対応です。以下は先行する学習工程の受入記録です。

第六段階では四つのメッシュ手法に共通の生成材質変換（`texture_preview` → `texture_ready`）を追加します。SuGaR は独自の材質、TSDF/GS2Mesh は任意の OpenMVS を使用します。ジョブ所有の OBJ/MTL/画像から原生所有の PLY/アトラスを作り、元資産を変更せず自動表示します。準備に失敗しても前段の検証済み成果は保持します。汎用 OBJ インポートではなく、不透明なディフューズ材質の三角形と反復しない UV のみを受け入れます。元解像度での配置は各辺 8192 px、画像作業予算 256 MiB に制限し、未対応のシェーダー効果は明示的に拒否します。実写材質品質・実行環境の受入項目は変更せず、プレビューを最適化器の再開とは扱いません。

2026-09-30、ユーザーの許可に基づいて E: に独立環境を導入しました。システム CUDA・ドライバーと既存 3DGS 環境は変更しません。インストール先ドライブの `Gaussian-Scene-Workbench-Runtime/2dgs` と `conda/envs/gsw_2dgs/python.exe` を自動検出し、`TWO_DGS_DIR` / `TWO_DGS_PYTHON` とソース内 `.venv/Scripts/python.exe` も使用できます。明示指定が不正な場合は黙って別環境へ切り替えません。通常起動時は検査のみで、自動ダウンロードは行いません。

実 RTX 4070 Laptop GPU で状態・Adam・乱数の完全復元、1200 反復の連続プレビューと再開、TensorBoard 評価、二尺度 PLY 出力を検証しました。合成 32 ガウシアンの連続／再実行／再開 PSNR は 27.176575 / 27.137341 / 27.168127 dB です。実写品質の保証ではありません。長期の法線・歪み正則化、代表的データセット、厳密なサーフェル表示は引き続き検証が必要です。別の有界 TSDF CUDA テストでは頂点 700・面 1274 を生成しました。他のメッシュ環境・材質品質の受入項目は MESH_GENERATION.md を参照してください。

## Verified Windows runtime / 已验证 Windows 环境 / 検証済み Windows 環境

- Official source: `hbb1/2d-gaussian-splatting@f3e3b9fa67bbd1c75e05167ff37391d8dab2a678`; recursive submodules retained; no source patches.
- Python 3.10.21, PyTorch 2.0.1+cu118, torchvision 0.15.2+cu118; Python dependencies pinned in `scripts/requirements-native-2dgs.txt`.
- CUDA 11.8.89 compiler/runtime/headers from NVIDIA's Conda channel, installed in `E:/Gaussian-Scene-Workbench-Runtime/cuda-11.8`.
- MSVC 14.38.33130 / compiler 19.38.33145, official Microsoft Visual Studio catalog payloads verified against catalog SHA-256 and extracted into an isolated E: directory. Existing Windows SDK 10.0.26100.0 used read-only. No administrator or system-compiler changes.
- CUDA extensions built for the local GPU (`TORCH_CUDA_ARCH_LIST=8.9`), so other GPU architectures require rebuilding. Build uses `DISTUTILS_USE_SDK=1`, `MAX_JOBS=4`, explicit MSVC/SDK include/library paths and `pip install --no-build-isolation` for both upstream submodules.
- NumPy 1.26.4 avoids the NumPy 2 / Torch 2.0 ABI conflict. Matplotlib 3.8.4 retains the upstream TensorBoard color-map API; the native worker uses the non-interactive Agg backend.

以上是本机隔离部署记录，不把编译工具或研究代码重新授权为 MIT。 / This is a local isolated-install record, not MIT relicensing of the compiler or research code. / 上記は独立導入の記録であり、コンパイラーや研究コードを MIT に変更するものではありません。

## Developer checks

### Native density-control correction / 原生密度控制修正 / 原生密度制御の修正 (2026-09-30)

中文：原生 3DGS / 2DGS 共用 `training_density_control.py`。世界尺度裁剪以相机范围和初始化点云 5%–95% 稳健包围范围半对角线的较大值为基准（阈值仍为该尺度的 0.1）；不改变用于增密/位置学习率的相机尺度。保留上游克隆、分裂、Adam 参数同步和正常透明度/尺寸清理。透明度重置后 300 次迭代以内，裁剪阈值最多为 0.005；随后恢复后端原阈值。一次最终裁剪最多删除有效高斯的 20%，按透明度由低到高、同值按源顺序删除，其他候选暂缓并在训练面板提示；非有限坐标/尺度/透明度不受保留预算保护。这不是数量下限或几何质量保证，也不是关闭裁剪。

English: Native 3DGS and 2DGS share a geometry-aware final-pruning policy. Its world-size threshold is 0.1 times the larger of camera extent and the half-diagonal of robust 5th–95th-percentile initialization bounds. Clone/split and position-learning-rate extent stay unchanged. After opacity resets, the cull threshold is capped at 0.005 for 300 iterations, then returns to the backend threshold. At most 20% of finite Gaussians are removed per final pass, ordered by increasing opacity with stable source-order ties; remaining candidates are deferred and reported in the monitor. Non-finite geometry/opacity is still removed. This is neither a minimum-count promise nor a geometric-quality guarantee; normal pruning remains enabled.

日本語：原生 3DGS / 2DGS は初期点群の 5%–95% 範囲の半対角線とカメラ範囲の大きい方を世界スケールに使用し、その 0.1 を枝刈り閾値とします。高密度化と位置学習率のカメラ範囲、公式のクローン・分割・Adam 処理は変更しません。不透明度リセット後の 300 反復は閾値を最大 0.005 に制限し、その後は元の閾値に戻します。最終処理で有限ガウシアンの最大 20% を不透明度の低い順に削除し、同値は元の順序を維持します。残りは保留して監視パネルに表示し、非有限の座標・尺度・不透明度は保護対象にしません。最小個数や幾何品質の保証ではなく、正常な枝刈りは継続します。

Scale calibration is serialized with native optimizer checkpoints. The density-control source digest is part of training identity, so pre-correction checkpoints are explicitly rejected for full-state resume rather than silently changing optimization semantics. Their original results remain available; corrected training must start a new run. PLY import is not optimizer resume.

中文：标定尺度进入完整状态检查点；密度控制源码指纹参与训练身份校验。修正前检查点不允许静默续训，应新建训练，原成果保持不变。日本語：スケールを状態に保存し、密度制御のソース指紋を再開時に検証します。修正前のチェックポイントは再開せず、新規学習が必要です。元の成果は保持します。

Parity: bounded/unbounded TSDF consume guarded native 2DGS results; SuGaR/GS2Mesh consume guarded native 3DGS results. Their mesh/depth stages have no Gaussian-density optimizer, and SuGaR's separate refinement does **not** acquire this policy or full-state resume. OpenMVS photo texturing has no Gaussian pruning. These distinctions are executable capability-matrix entries; acceptance still requires representative TSDF/refinement/depth/material quality, not just a larger input count.

中文：保护同步覆盖两种原生训练，TSDF、SuGaR、GS2Mesh 可使用对应修正后的输入；SuGaR 自身细化尚未接入该保护。网格/深度/纹理阶段没有同一高斯裁剪操作，其实拍验收仍独立。日本語：TSDF は修正された 2DGS、SuGaR/GS2Mesh は修正された 3DGS を入力にできます。SuGaR 独自の細化には未適用で、メッシュ・深度・写真テクスチャの品質は別に検証が必要です。

Real-data regression: the original nine-image, 2503-point COLMAP reconstruction was replayed at the same 7000-iteration / 1:8 quick settings without replacing original files. The original output contains 254 Gaussians with 18.2524 dB on the upstream five-training-view evaluation; the corrected run contains 41537 with 40.2916 dB on the same evaluation. A separate read-only evaluation of all nine training views gives 18.7689 → 40.7246 dB and confirms visibly restored details. This is training-view fit at 241 × 135, not held-out/novel-view or mesh-quality acceptance. The nine-image limited capture cannot reveal unseen geometry. Counts/quality vary slightly between independent CUDA runs. Baseline evidence and owned outputs are in the local `.tmp/2dgs-collapse-diagnosis` directory, not distributed user data.

中文：同一九张实拍图、2503 稀疏点、7000 次 quick 回归中，254 → 41537 高斯，同一五训练视角 PSNR 18.2524 → 40.2916 dB；不是留出视角或网格质量保证。日本語：同一九画像・2503 疎点・7000 quick 反復で 254 → 41537、同じ五つの学習視点の PSNR は 18.2524 → 40.2916 dB。未知視点やメッシュ品質の受入ではありません。

```text
python -B -m unittest native.worker.test_two_dgs_adapter native.worker.test_training_checkpoint native.worker.test_training_preview native.worker.test_training_density_control
python -B -m unittest crop_editor.tests.test_training_telemetry
python scripts/native_i18n.py
python scripts/test_native_i18n.py
ctest --test-dir native/build-unity-gizmo --output-on-failure
```

Run tensor tests with the configured training Python. Do not mix the server telemetry suite's optional-dependency stubs into that process. `native_2dgs_training_resume` validates the owned desktop-worker lifecycle, not a CUDA training run.

For the real surfel test, set `TWO_DGS_DIR` to the installed source, `GSW_CHECKPOINT_TEST_BACKEND=2dgs`, optionally `GSW_CHECKPOINT_TEST_ITERATIONS=1200`, then run `python -B -m unittest native.worker.test_training_checkpoint_cuda` using the 2DGS interpreter and DLL PATH. `GSW_CHECKPOINT_TEST_ROOT` can target the packaged backend. Keep TEMP/TMP on a data drive.
