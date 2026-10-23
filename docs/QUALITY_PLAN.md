# Experimental quality plans / 实验性质量计划 / 実験的品質プラン

## English

Quality plans are an explicit opt-in in the native training dialog. Leaving the
experimental checkbox off preserves the existing presets, runtime policy and
random streams. Enabling a plan records its requested/effective settings and
identity in full native checkpoints. Geometry learning-rate multipliers affect
only position and scale rates, not camera scale, resolution or other rates.

3DGS has isolated AbsGS-style pre-cancellation statistics, Pixel-GS-style
contributor/depth statistics and Full Mip filtering. These are research
implementations, not certified reproductions or a promise of better quality.
Only one advanced mechanism is admitted at a time. 2DGS shares plan identity,
learning-rate controls, mask/depth supervision and complete-state recovery;
volumetric Abs/Pixel/Mip/MCMC adaptations are explicitly unavailable for its
surfel backend. A PLY warm start is not optimizer-state recovery.

Full Mip preserves the raw training model and writes a separate atomic,
hash-bound fused display bundle. Open the bundle's `model.ply`; keep its
`.quality.json` and camera sidecar with it. Opening raw Mip as an ordinary
Gaussian model, losing the filter sidecar, double filtering or exporting to an
unsupported format is rejected. Coordinate delivery uses the fused model while
retaining raw and filter provenance. A failed delivery does not erase a
completed training result.

MCMC remains unavailable in the desktop selector: engineering state tests do
not yet satisfy the complete GPU/recovery/resource acceptance gates. A small
diagnostic encountered an unresolved CUDA fault; its failure is retained.
The development CLI is experimental, not a production-quality alternative.
New components are independently versioned; existing installed kernels and
paused checkpoints must not be overwritten or silently reassigned.

The user-selected C2 test source replaces the old59-photo fixture for this task.
It contains135 synthetic1536×1536 images, with the original99/12/24 split and
locked cameras. Original files are not deleted. It is not independent
real-world or metric geometry evidence. Numerical CUDA tests, short training,
native subset tests, worker lifecycle, final appearance and independent
geometry remain distinct acceptance scopes. See the detailed
[capability matrix](quality_upgrade/CAPABILITY_MATRIX.json) and
[acceptance report](quality_upgrade/ACCEPTANCE_REPORT.json); no method is
automatically promoted to a default.

## 简体中文

原生训练窗口中的“实验性质量计划”默认关闭；关闭时保留已有预设、运行策略和
随机序列。启用后，完整检查点保存请求值、实际值与计划身份。几何学习率倍数只
作用于位置和缩放学习率，不改变相机尺度、分辨率或其他学习率。

3DGS 接入独立的 AbsGS 风格抵消前绝对梯度统计、Pixel-GS 风格贡献像素与深度
统计，以及 Full Mip 过滤；这些属于研究实现，不等于论文复现或画质提升保证。
一次只启用一种高级机制。2DGS 同步支持计划身份、学习率、遮罩/深度监督与完整
状态恢复；体高斯的 Abs/Pixel/Mip/MCMC 尚未验证为二维面元等价算法，明确禁用。
PLY 热启动不等于恢复优化器状态。

Full Mip 保留训练原件，另写原子发布、哈希绑定的融合显示包。打开包内
`model.ply`，并保留其 `.quality.json` 和相机文件；原始 Mip 误当普通高斯打开、
丢失过滤信息、重复过滤和不支持格式导出均会拒绝。坐标成果交付使用融合模型，
保留原件与过滤来源；交付失败不会把已经完成的训练改成训练失败。

MCMC 在桌面界面仍不可选：状态单元测试不能代替完整 GPU、续训和资源验收。
极小诊断样本出现的 CUDA 异常仍待定位，失败记录保留；开发 CLI 不作为正式
可用或画质已验收的替代方案。新内核独立版本化，不覆盖旧内核或自动改绑暂停工程。

本轮测试改用用户新指定的 C2 目录，旧59张退出测试而不删除原文件。新目录是
135张1536×1536合成图，沿用99/12/24划分与锁定相机，不冒充独立实景或公制几何
证据。数值、短训练、原生子集、worker生命周期、最终画质和独立几何分别记录，
不自动修改默认算法；完整边界见上方能力矩阵及验收报告。

## 日本語

ネイティブ学習画面の実験的品質プランは初期状態で無効です。無効時は既存の
プリセット、実行方針、乱数列を維持します。有効時は要求値・実効値とプランの
識別情報を完全チェックポイントに保存します。幾何学習率の倍率は位置とスケール
の学習率だけに適用され、カメラの尺度や画像解像度は変更しません。

3DGSにはAbsGS系の相殺前絶対勾配統計、Pixel-GS系の寄与画素・深度統計とFull
Mipフィルタを追加しています。研究実装であり、論文再現や品質向上を保証する
ものではありません。高度な方式の組合せは許可せず、個別に検証します。2DGSも
プラン識別、学習率、マスク/深度監督と完全状態復元を共有しますが、体積ガウスの
Abs/Pixel/Mip/MCMCをサーフェルへそのまま適用しません。PLYからの初期化は
オプティマイザ状態の再開ではありません。

Full Mipは元モデルを保持し、ハッシュで結び付けた表示用融合バンドルを原子的に
公開します。バンドル内の`model.ply`を対応する`.quality.json`とカメラファイル
と一緒に使用してください。生モデルの誤表示、情報欠落、二重フィルタ、不対応
形式への出力は拒否します。成果物の出力失敗は学習完了状態と分離されます。

MCMCはGPU・再開・リソースの全検証が未完了のため、デスクトップでは選択でき
ません。微小診断で発生したCUDA異常を解決済みと扱わず、開発CLIも正式受入とは
区別します。新旧カーネルは別管理し、既存の一時停止チェックポイントを上書き
または自動再割当てしません。

今回の試験は指定されたC2ディレクトリへ変更し、従来59枚は試験から除外します
が削除しません。新入力は135枚の1536×1536合成画像で、99/12/24分割と固定
カメラを維持します。独立した実写・計量幾何の証拠ではありません。数値、短時間
学習、ネイティブ部分試験、worker、最終画質、独立幾何を区別し、既定方式への
昇格は行いません。詳細は能力表と受入レポートを参照してください。
