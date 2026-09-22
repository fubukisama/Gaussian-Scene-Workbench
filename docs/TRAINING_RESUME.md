# Native training pause / resume

[简体中文](#简体中文) · [English](#english) · [日本語](#日本語)

## 简体中文

工具栏和「工作流」菜单新增「暂停训练」「继续训练」。仅本版本新启动的原生 3DGS 训练支持；相机解算、导入和 2DGS 不会误启用暂停。

点击暂停后，等待当前迭代完成密度控制和优化器更新，再保存完整状态与 PLY 预览，最后退出训练进程释放显存。保存过程中请勿强制关闭；「停止任务」仍是取消，不等于安全暂停。暂停后保留视口模型、迭代和监视指标，工程重新打开后仍可继续。

续训直接复用随附 Graphdeco 的 `GaussianModel.capture/restore`（原许可证不变），补全曝光张量/优化器、随机数状态、剩余相机采样队列、累计时间及日志平滑值。保持原总迭代数和学习率/密度控制计划，不从 PLY 重新初始化，也不重新运行 COLMAP；重建输入缺失时直接拒绝续训，不自动恢复缓存或重新解算。已正常暂停的工程不会在重开时替换后来选中的模型及其变换。历史曲线不跨进程恢复；不会伪造旧采样。旧版检查点、外部任意 PTH、2DGS 续训暂不支持。

完整状态先写唯一临时文件，再原子发布 `output/<scene>/.gsw-resume/ready.json`；只有新清单发布成功后才回收上一个状态文件。磁盘写入失败保留上一个有效检查点。续训核对 SHA-256、图像/相机文件内容和训练参数；源数据变更时拒绝继续。首次启动和续训需读取训练输入计算指纹，耗时取决于数据量。工程托管路径使用相对记录，另存为时跟随工程迁移；外部数据保持原外部位置。

检查点使用 PyTorch pickle 序列化。只加载可信且未被他人替换的本机训练文件；校验和无法证明来源。恢复需要用户明确确认，不在打开工程时自动反序列化。一次工程只记录一个当前续训入口，新训练会先提醒替换该入口。

验证覆盖原子写入失败、损坏与路径越界拒绝、参数/影像变更、暂停和取消区分、续训不覆盖输出、不重跑 COLMAP、三语即时切换、工程重开、CPU Adam 连续/恢复训练完全一致。可选 CUDA 集成测试逐项检查实际模型/优化器/曝光/RNG 的精确恢复，并比较小型合成场景的训练质量连续性。独立 CUDA 连续运行也存在数值波动，因此不承诺恢复后各浮点参数逐位相同，也不把合成样例指标当作真实数据质量保证。

## English

**Pause Training** and **Resume Training** appear in the toolbar and Workflow menu for newly started native 3DGS jobs. Pause waits for an optimizer-step boundary, saves full state and a PLY preview, then exits to release GPU memory. Stop remains cancellation. The preview and project resume entry survive pausing/reopening. COLMAP, import, 2DGS, legacy checkpoints and arbitrary external PTH files are not supported by this resume path.

The vendored Graphdeco capture/restore implementation is reused under its existing license, augmented with exposure/Adam, RNG, pending camera samples, elapsed time and smoothed log values. Original iteration schedules are retained; historical curves are not reconstructed. Atomic manifest-last publication preserves the previous checkpoint on write failure. Input-content fingerprints and SHA-256 detect incompatible/corrupt state, but do not establish trust. Hashing inputs costs time proportional to dataset size. Managed paths migrate with Save As; external paths remain external. Loading pickle-based checkpoints requires explicit user trust confirmation and never happens automatically on project open.

Regression tests cover state safety, worker/desktop lifecycle, immediate trilingual labels and reopening. CPU Adam continuation is exact. The opt-in CUDA test verifies exact checkpoint tensor/optimizer/RNG restoration and image-quality continuity; it does not promise bitwise reproducibility of subsequent GPU updates.

Missing reconstruction inputs fail safely instead of invoking automatic alignment/cache recovery. Reopening a safely paused project preserves any model selection and transforms subsequently saved by the user.

## 日本語

ツールバーと「ワークフロー」に「学習を一時停止」「学習を再開」を追加します。本バージョンで新しく開始したネイティブ 3DGS ジョブのみが対象です。最適化器の更新後に完全な状態と PLY プレビューを保存し、プロセスを終了して GPU メモリを解放します。「タスクを停止」は引き続きキャンセルです。プレビューと再開対象はプロジェクトを開き直しても保持します。COLMAP、インポート、2DGS、旧チェックポイント、任意の外部 PTH は対象外です。

同梱 Graphdeco の capture/restore を元のライセンスのまま再利用し、露出と Adam、乱数状態、未使用カメラのサンプル列、経過時間、平滑化ログ値を追加します。元の反復スケジュールを保持し、過去の曲線は再構築しません。マニフェストを最後に原子的に公開するため、書き込み失敗時も以前の有効な状態が残ります。入力内容の指紋と SHA-256 で不一致・破損を検出しますが、信頼性は保証しません。入力のハッシュ計算にはデータ量に応じた時間が必要です。管理対象のパスは「名前を付けて保存」で移行し、外部パスはそのままです。pickle の読み込みには明示的な信頼確認が必要で、プロジェクトを開いた際に自動実行しません。

状態の安全性、ワーカーと UI のライフサイクル、三言語の即時切り替え、再オープンを検証します。CPU Adam の連続実行と復元後の実行は完全一致します。任意実行の CUDA テストはテンソル・最適化器・乱数状態の完全復元と画質の連続性を検証しますが、その後の GPU 更新のビット単位での再現性は保証しません。

再構築入力が欠落した場合、自動アラインメントやキャッシュ復元を行わずに再開を拒否します。正常に一時停止したプロジェクトを開き直しても、その後ユーザーが保存したモデルの選択や変換を置き換えません。

## Developer validation

```text
python scripts/native_i18n.py
python -m unittest native.worker.test_training_checkpoint
ctest --test-dir native/build-unity-gizmo -R native_training_resume --output-on-failure
# Use the configured CUDA training Python and DLL PATH, with TEMP/TMP on E: or D:
python -m unittest native.worker.test_training_checkpoint_cuda
```

The CUDA fixture creates only temporary synthetic data. `GSW_CHECKPOINT_TEST_ROOT` can point at a packaged backend to exercise installed source. The desktop smoke test uses a test-owned stdin worker; `GSW_PROCESS_OUTPUT_FIXTURE` can locate the build-tree helper when testing an installed executable. Normal application preferences are isolated during all `--smoke-test*` runs.

## 验证记录 / Validation / 検証 — 2026-09-23

- 最终全量 CTest **41/41** 通过；后端套件 62 项中 61 通过、1 项因通用 Python 无 Torch 跳过。训练 Python 中全部 9 项检查点测试及 1 项实际 CUDA 集成测试通过，补验了该跳过项。
- 独立安装版在仅系统 PATH 下通过中/英/日界面、窗口/列表和暂停后多模型工程重开测试，检查了三语界面截图。安装包中训练脚本、worker、server、检查点模块与源码 SHA-256 一致。
- 安装后端小型 CUDA 样例保留 32 个高斯；模型、Adam、曝光和随机状态逐项精确恢复。16 次迭代的连续/连续复跑/续训 PSNR 为 6.470175 / 6.470049 / 6.470275 dB。它仅用于验证训练连续性，不代表真实模型质量或后续浮点参数逐位一致。
- 定位并修复测试配置串用：旧方案按 Windows PID 保存配置，ID 复用后可能读到旧工具锁定状态。现在使用每次独立且自动清理的临时目录，并有残留配置回归测试；不修改用户配置。

Final validation passed all 41 native tests. The 62-test backend suite had 61 passes and one Torch-dependent skip, subsequently covered by all nine checkpoint tests plus a real CUDA test in the training environment. The installed build passed trilingual UI and paused-project reopening checks with system-only PATH; backend source hashes match. The synthetic 32-Gaussian CUDA test verifies exact restored state and quality continuity, not real-scene quality or bitwise subsequent GPU updates. A reproduced Windows PID-reuse settings collision was fixed with per-run temporary test settings and a regression test; user preferences are unchanged.

最終検証ではネイティブ 41 件がすべて成功しました。バックエンド 62 件のうち 61 件が成功し、Torch 依存の 1 件はスキップ後、学習環境のチェックポイント 9 件と実 CUDA テストで補完しました。インストール版をシステム PATH のみで実行し、三言語 UI と一時停止後の再オープンを確認しました。バックエンドのハッシュも一致しています。32 ガウシアンの合成 CUDA テストは完全な状態復元と画質の連続性を検証し、実データの品質や後続 GPU 更新のビット一致を保証するものではありません。Windows PID 再利用によるテスト設定の混入を独立した一時ディレクトリと回帰テストで修正し、ユーザー設定は変更していません。

Package: `Gaussian-Scene-Workbench-0.3.1-native-training-resume-win-x64`

Application SHA-256: `427A1B35FA3C79FEF76CBA96F794F12C3BED8DB15652038A5E7831035F85611C`
