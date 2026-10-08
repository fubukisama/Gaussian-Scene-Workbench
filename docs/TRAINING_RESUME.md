# Native training pause / resume

[简体中文](#简体中文) · [English](#english) · [日本語](#日本語)

## 简体中文

工具栏和「工作流」菜单新增「暂停训练」「继续训练」。新启动的原生 3DGS 和 2DGS 训练入口均接入；相机解算、导入和未适配网格阶段不会误启用暂停。2DGS 的环境与待验收差异见[链路对齐](GENERATION_PIPELINES.md)。

点击暂停后，等待当前迭代完成密度控制和优化器更新，再保存完整状态与 PLY 预览，最后退出训练进程释放显存。保存过程中请勿强制关闭；「停止任务」仍是取消，不等于安全暂停。暂停后保留视口模型、迭代和监视指标，工程重新打开后仍可继续。

续训直接复用随附 Graphdeco 的 `GaussianModel.capture/restore`（原许可证不变），补全曝光张量/优化器、随机数状态、剩余相机采样队列、累计时间及日志平滑值。保持原总迭代数和学习率/密度控制计划，不从 PLY 重新初始化，也不重新运行 COLMAP；重建输入缺失时直接拒绝续训，不自动恢复缓存或重新解算。已正常暂停的工程不会在重开时替换后来选中的模型及其变换。历史曲线不跨进程恢复；不会伪造旧采样。旧版任意检查点和外部 PTH 不支持；2DGS 使用独立的双尺度状态及曲面训练入口，不含曝光优化器。

完整状态先写唯一临时文件，再原子发布 `output/<scene>/.gsw-resume/ready.json`；只有新清单发布成功后才回收上一个状态文件。磁盘写入失败保留上一个有效检查点。续训核对 SHA-256、图像/相机文件内容和训练参数；源数据变更时拒绝继续。首次启动和续训需读取训练输入计算指纹，耗时取决于数据量。工程托管路径使用相对记录，另存为时跟随工程迁移；外部数据保持原外部位置。

检查点使用 PyTorch pickle 序列化。只加载可信且未被他人替换的本机训练文件；校验和无法证明来源。恢复需要用户明确确认，不在打开工程时自动反序列化。

“工作流 → 实验与生成档案”分别保留每个实验的参数、数据集、输出位置、状态、成果对象 ID 和可用续训入口。新实验不替换先前实验的档案，也不允许复用已归档实验的输出目录；旧版单一活动入口在读取时迁入档案，活动入口仍只是最近任务的快捷指针。选中记录后可以“查看所选成果”或“继续所选实验”。完整状态元数据就绪只说明入口可供核查，续训前仍需验证文件内容、身份与参数并确认信任。已完成实验不会自动重新训练，失效配置或检查点会说明不可续训的原因。

重新打开工程不启动任何任务，也不读取 pickle。上次遗留的运行记录标为中断；档案不是自动执行队列。切换到其他实验的数据集需要明确确认，只有实际启动成功后才采用该数据集，启动失败保留原工程上下文与先前入口。档案以工程内 `.gsw/experiments` 的独立原子 JSON 记录保存，托管路径使用相对位置，外部引用保持原位置；任务面板日志和历史曲线仍不跨会话恢复。

“暂停预览”只冻结任务画面，训练仍继续，也不保存完整状态或释放训练进程持有的显存；需要安全退出训练时仍使用“暂停训练”。四种网格方法及其可选 OpenMVS 纹理阶段也保存参数、成果和状态并支持独立观察，但没有完整优化器状态续训，不能把预览 PLY 或再次启动阶段称为完整恢复。

验证覆盖原子写入失败、损坏与路径越界拒绝、参数/影像变更、暂停和取消区分、续训不覆盖输出、不重跑 COLMAP、三语即时切换、工程重开、CPU Adam 连续/恢复训练完全一致。可选 CUDA 集成测试逐项检查实际模型/优化器/曝光/RNG 的精确恢复，并比较小型合成场景的训练质量连续性。独立 CUDA 连续运行也存在数值波动，因此不承诺恢复后各浮点参数逐位相同，也不把合成样例指标当作真实数据质量保证。

## English

**Pause Training** and **Resume Training** appear in the toolbar and Workflow menu for newly started native 3DGS and 2DGS jobs. Pause waits for an optimizer-step boundary, saves full state and a PLY preview, then exits to release GPU memory. Stop remains cancellation. The preview and project resume entry survive pausing/reopening. COLMAP, import, unadapted meshing stages, legacy checkpoints and arbitrary external PTH files are not supported by this resume path.

The vendored Graphdeco capture/restore implementation is reused under its existing license, augmented with exposure/Adam, RNG, pending camera samples, elapsed time and smoothed log values. Original iteration schedules are retained; historical curves are not reconstructed. Atomic manifest-last publication preserves the previous checkpoint on write failure. Input-content fingerprints and SHA-256 detect incompatible/corrupt state, but do not establish trust. Hashing inputs costs time proportional to dataset size. Managed paths migrate with Save As; external paths remain external. Loading pickle-based checkpoints requires explicit user trust confirmation and never happens automatically on project open.

Regression tests cover state safety, worker/desktop lifecycle, immediate trilingual labels and reopening. CPU Adam continuation is exact. The opt-in CUDA test verifies exact checkpoint tensor/optimizer/RNG restoration and image-quality continuity; it does not promise bitwise reproducibility of subsequent GPU updates.

Missing reconstruction inputs fail safely instead of invoking automatic alignment/cache recovery. Reopening a safely paused project preserves any model selection and transforms subsequently saved by the user.

**Workflow → Experiments and Generation History** retains separate settings,
dataset/output locations, status, result object ID and resume availability for
each experiment. A new experiment does not replace earlier records and cannot
reuse an archived experiment's output directory. The legacy active resume entry
is migrated when read; the active entry remains only a latest-task shortcut.
**View Selected Result** and **Resume Selected Experiment** operate on the
selected record. Ready full-state metadata is not proof that checkpoint contents
are valid: resume still verifies content, identity and settings and requires
explicit trust. Completed experiments are not automatically retrained, and
unavailable configurations/checkpoints report why resume is blocked.

Opening a project starts no task and deserializes no pickle. Stale running records
become interrupted; history is not an automatic queue. Resuming another dataset
requires confirmation, with dataset adoption deferred until an actual successful
launch; launch failure retains the previous project context and resume entry.
Independent atomic JSON records live in `.gsw/experiments`, with relative managed
paths and unchanged external references. Task-panel logs and historical metric
curves remain session-only.

**Pause Preview** freezes observation without pausing optimization, saving a full
checkpoint or releasing the training process's GPU allocations; use **Pause
Training** for a safe training exit. All four mesh methods and their optional
OpenMVS texturing stages retain settings/results/status and independent
observation, but have no full optimizer resume. A preview PLY or restarting a
stage is not full-state restoration.

## 日本語

ツールバーと「ワークフロー」に「学習を一時停止」「学習を再開」を追加します。新しく開始したネイティブ 3DGS と 2DGS ジョブが対象です。最適化器の更新後に完全な状態と PLY プレビューを保存し、プロセスを終了して GPU メモリを解放します。「タスクを停止」は引き続きキャンセルです。プレビューと再開対象はプロジェクトを開き直しても保持します。COLMAP、インポート、未対応のメッシュ段階、旧チェックポイント、任意の外部 PTH は対象外です。

同梱 Graphdeco の capture/restore を元のライセンスのまま再利用し、露出と Adam、乱数状態、未使用カメラのサンプル列、経過時間、平滑化ログ値を追加します。元の反復スケジュールを保持し、過去の曲線は再構築しません。マニフェストを最後に原子的に公開するため、書き込み失敗時も以前の有効な状態が残ります。入力内容の指紋と SHA-256 で不一致・破損を検出しますが、信頼性は保証しません。入力のハッシュ計算にはデータ量に応じた時間が必要です。管理対象のパスは「名前を付けて保存」で移行し、外部パスはそのままです。pickle の読み込みには明示的な信頼確認が必要で、プロジェクトを開いた際に自動実行しません。

状態の安全性、ワーカーと UI のライフサイクル、三言語の即時切り替え、再オープンを検証します。CPU Adam の連続実行と復元後の実行は完全一致します。任意実行の CUDA テストはテンソル・最適化器・乱数状態の完全復元と画質の連続性を検証しますが、その後の GPU 更新のビット単位での再現性は保証しません。

再構築入力が欠落した場合、自動アラインメントやキャッシュ復元を行わずに再開を拒否します。正常に一時停止したプロジェクトを開き直しても、その後ユーザーが保存したモデルの選択や変換を置き換えません。

「ワークフロー → 実験と生成履歴」で各実験の設定、データセット、出力先、状態、成果オブジェクト ID、再開可否を個別に保持します。新規実験は以前の記録を置き換えず、履歴に登録済みの実験の出力先を再利用できません。旧版の単一の有効な再開入口は読み込み時に移行し、有効な入口は最新タスクへの近道としてのみ残します。「選択した成果を表示」「選択した実験を再開」は選択中の記録を対象とします。完全な状態のメタデータがあっても内容が検証済みとは限らず、再開前にファイル内容、同一性、設定を検証して信頼確認を求めます。完了済みの実験を自動的に再学習せず、設定や状態が使用できない場合は理由を表示します。

プロジェクトを開いてもタスクを起動せず、pickle を読み込みません。残っていた実行中の記録は中断済みとし、自動実行キューにはしません。別の実験のデータセットへの切り替えには確認が必要で、実際の起動成功後にのみ採用します。起動失敗では元の工程コンテキストと再開入口を保持します。`.gsw/experiments` に実験ごとの JSON を原子的に保存し、管理対象パスは相対位置、外部参照は元の位置とします。タスクのログと過去の指標曲線は引き続きセッション内のみです。

「プレビューを一時停止」は表示のみを止め、最適化の一時停止、完全な状態の保存、学習プロセスの GPU 領域の解放は行いません。学習を安全に終了する場合は「学習を一時停止」を使います。四つのメッシュ方式と任意の OpenMVS テクスチャ工程も設定・成果・状態と独立した観察を保持しますが、完全な最適化器状態からの再開はありません。プレビュー PLY や工程の再実行を完全な復元とは呼びません。

2DGS uses the official upstream adapter with two-scale state and no exposure optimizer. The isolated runtime, passing CUDA resume/preview smoke tests and remaining real-data/thin-disk preview limitations are documented in [generation pipeline parity](GENERATION_PIPELINES.md).

2DGS は公式アダプターを使用し、二尺度の状態を保持します。露出最適化器は追加しません。独立環境、合格した実 CUDA の再開・プレビュー試験、実写品質と薄片近似表示の制限は[生成パイプラインの整合](GENERATION_PIPELINES.md)を参照してください。

## Developer validation

中文：重跑或修复同一数据集的 COLMAP 会改变旧检查点的输入身份。启动前会列出受影响的可续训实验并要求确认；完整状态和成果保留，但不是数据版本快照。需要保留旧实验的可续训输入时，应使用独立数据集。

English: Rerunning or repairing COLMAP in the same dataset changes the input identity of previous checkpoints. Launch lists affected resumable experiments and requires confirmation. States/results are retained, but the archive is not a dataset-version snapshot; use a separate dataset to preserve old resumable inputs.

日本語：同じデータセットで COLMAP を再実行・修復すると、以前のチェックポイントの入力同一性が変わります。開始前に影響を受ける再開可能な実験を表示し、確認を求めます。状態と成果は保持しますが、データのバージョンスナップショットではありません。以前の再開可能な入力を保持するには、別のデータセットを使用してください。

```text
python scripts/native_i18n.py
python -m unittest native.worker.test_training_checkpoint
ctest --test-dir native/build-unity-gizmo -R native_training_resume --output-on-failure
# Use the configured CUDA training Python and DLL PATH, with TEMP/TMP on E: or D:
python -m unittest native.worker.test_training_checkpoint_cuda
```

The CUDA fixture creates only temporary synthetic data. `GSW_CHECKPOINT_TEST_ROOT` can point at a packaged backend to exercise installed source. The desktop smoke test uses a test-owned stdin worker; `GSW_PROCESS_OUTPUT_FIXTURE` can locate the build-tree helper when testing an installed executable. Normal application preferences are isolated during all `--smoke-test*` runs.

The 2026-10-08 history acceptance boundary uses the public history dialog and
test-owned workers: independently retained experiment settings/results, project
reopen, explicit selection of a resumable native experiment, unavailable-state
diagnostics and failed-launch rollback. Store tests check atomicity, managed-path
portability and rejection of linked/traversing paths. Relevant CTests are
`generation_history`, `native_generation_history`, `native_training_resume` and
`native_2dgs_training_resume`; these describe test surfaces, not a new CUDA
quality result or a full persistent task queue. See
[CONTINUOUS_TRAINING_PREVIEW.md](CONTINUOUS_TRAINING_PREVIEW.md) for the independent
preview-pause boundary.

## 验证记录 / Validation / 検証 — 2026-09-23

- 最终全量 CTest **41/41** 通过；后端套件 62 项中 61 通过、1 项因通用 Python 无 Torch 跳过。训练 Python 中全部 9 项检查点测试及 1 项实际 CUDA 集成测试通过，补验了该跳过项。
- 独立安装版在仅系统 PATH 下通过中/英/日界面、窗口/列表和暂停后多模型工程重开测试，检查了三语界面截图。安装包中训练脚本、worker、server、检查点模块与源码 SHA-256 一致。
- 安装后端小型 CUDA 样例保留 32 个高斯；模型、Adam、曝光和随机状态逐项精确恢复。16 次迭代的连续/连续复跑/续训 PSNR 为 6.470175 / 6.470049 / 6.470275 dB。它仅用于验证训练连续性，不代表真实模型质量或后续浮点参数逐位一致。
- 定位并修复测试配置串用：旧方案按 Windows PID 保存配置，ID 复用后可能读到旧工具锁定状态。现在使用每次独立且自动清理的临时目录，并有残留配置回归测试；不修改用户配置。

Final validation passed all 41 native tests. The 62-test backend suite had 61 passes and one Torch-dependent skip, subsequently covered by all nine checkpoint tests plus a real CUDA test in the training environment. The installed build passed trilingual UI and paused-project reopening checks with system-only PATH; backend source hashes match. The synthetic 32-Gaussian CUDA test verifies exact restored state and quality continuity, not real-scene quality or bitwise subsequent GPU updates. A reproduced Windows PID-reuse settings collision was fixed with per-run temporary test settings and a regression test; user preferences are unchanged.

最終検証ではネイティブ 41 件がすべて成功しました。バックエンド 62 件のうち 61 件が成功し、Torch 依存の 1 件はスキップ後、学習環境のチェックポイント 9 件と実 CUDA テストで補完しました。インストール版をシステム PATH のみで実行し、三言語 UI と一時停止後の再オープンを確認しました。バックエンドのハッシュも一致しています。32 ガウシアンの合成 CUDA テストは完全な状態復元と画質の連続性を検証し、実データの品質や後続 GPU 更新のビット一致を保証するものではありません。Windows PID 再利用によるテスト設定の混入を独立した一時ディレクトリと回帰テストで修正し、ユーザー設定は変更していません。

Package: `Gaussian-Scene-Workbench-0.3.1-native-training-resume-win-x64`

Application SHA-256: `427A1B35FA3C79FEF76CBA96F794F12C3BED8DB15652038A5E7831035F85611C`
