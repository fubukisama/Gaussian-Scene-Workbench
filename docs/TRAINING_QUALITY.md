# Controllable quality / 可控质量 / 調整可能な品質

## What is controlled

Quality is the default priority, not a promise that one preset fits every scene.
The existing preset, image resolution, iteration count, density settings and
explicit backend choices are controls. They are not a percentage of guaranteed
model quality. A faster experiment may deliberately choose a different setting;
its effective settings and measured differences must remain visible.

Native trainers save `quality-profile.json` alongside their own output. It
records dataset/optimization/pipeline settings, the performance policy, loaded
image dimensions and the observed renderer/loss. It is an execution record, not
an independent quality evaluation. Native full-state checkpoints also retain
this record. No original images or saved experiments are overwritten.

Training PSNR measures fit to a sampled training image. It is not a certificate
for unseen views, 2DGS surface accuracy or a mesh. A completed training task is
therefore not labelled quality-validated merely because its progress is100%.
The desktop monitor explicitly retains this distinction after completion.

## Compare supplied measurements

`scripts/compare_training_quality.py` compares real baseline/candidate measurement
reports against an explicit user contract. It does not render, train, invent
measurements, or independently verify that a supplied view really was held out.
The measurement producer remains responsible for correct provenance and units.

Each report has version1, an explicit `identity` with `dataset_id`, `viewset_id`,
`resolution_id`, `mask_id` and `metric_protocol_id`, a `split` of `held_out` or
`training_fit`, and `metrics`. Optional `settings` records the effective choices.
All five identities and the split must match to compare numerical differences.
Use `none` for an explicit no-mask policy, not a missing field. The metric
protocol must specify units, cropping, color processing and aggregation.

The contract contains `version:1`, `required_domains` (`image`/`geometry`) and
`metrics`. Each requested metric can set `absolute_min`, `absolute_max` and/or
`max_regression`. PSNR and SSIM are higher-is-better; LPIPS, depth RMSE, normal
MAE and mesh distance are lower-is-better. Regression tolerance is user-defined;
there is no silent zero-tolerance rule or automatic reduction of quality.

Illustrative contract only—not a recommended universal threshold:

```json
{"version":1,"required_domains":["image"],"metrics":{"psnr":{"max_regression":0.1}}}
```

```text
python scripts/compare_training_quality.py --baseline baseline.json --candidate candidate.json --contract contract.json --output new-comparison.json
```

The output reports every metric's delta, thresholds, image/geometry status and
changed settings. Exit codes are0(pass),1(regressed),2(invalid),3(unverified),
4(not comparable). Existing report files are never overwritten. A pass applies
only to the selected measured domains and thresholds, not to all model quality.
Missing evidence/thresholds or training-only evidence cannot produce an overall
independent-validation pass. Image results alone cannot establish geometry.
Explicit zero-MSE PSNR uses a JSON object for positive infinity; NaN, bare
Infinity and booleans in numeric fields are rejected.

## Exact-semantics performance controls

Cross-step image prefetch, FP32 capacity storage and on-demand surfel normals
have separate desktop switches under the overall bounded-runtime switch.
They do not silently change resolution, SH degree, the clone/split/prune
algorithm or an active geometric loss. A local RNG copy predicts the next
camera; actual training sampling and checkpoint RNG remain authoritative.
Single-slot/budget/optional-prefetch allocation limits keep the ordinary upload
path. Capacity storage keeps bounded, zero-initialized spare backing; checkpoints
may include that bounded spare rather than attempting a full CUDA clone while
memory is constrained. Unsupported layouts retain the original storage path.

The pinned2DGS adapter skips only unrequested Python depth-derived surface
normal processing before the normal loss is active. It does not remove the
CUDA auxiliary maps or alter active distortion/depth/normal formulas. Unknown
external-renderer versions keep their original behavior. Optional FastGS and
low-precision algorithms have separate compatibility/quality gates; they are
not made default simply to win a timing comparison.

## 中文

默认质量优先，但允许显式调整速度与质量的取舍。预设、分辨率、迭代数、增密参数和
后端选择是控制项，不是可保证的“质量百分比”。每个实验保存生效配置与图像尺寸；
训练完成、训练PSNR上升，都不等于独立验证或网格几何精度已经达标。

质量对照按相同数据、视图、分辨率、遮罩和指标协议比较，显示逐指标变化与配置差异。
容差由用户明确设定；缺少指标、阈值或独立验证时标为未验证，条件不一致时不可比较。
图像质量与几何质量分开判定；没有几何参考或测量，不能声称2DGS/网格精度合格。
本工具只比较输入的实测报告，不自动生成测量，也不自动证明留出集来源。

三个执行优化开关可分别关闭，不擅自降低照片分辨率、SH阶数或改变增密算法；活跃的
几何损失保持不变。不支持的优化器/外部光栅化版本保留原路径。低精度等可能改变结果
的方案必须是明确选择，并另做质量对照，不冒充无损优化。

## 日本語

既定では品質を優先しますが、速度と品質のトレードオフは明示的に調整できます。
プリセット、解像度、反復回数、高密度化設定、バックエンドは制御項目であり、
保証された「品質の百分率」ではありません。実験ごとに有効な設定と画像寸法を
保存します。学習完了や学習PSNRの上昇だけでは、独立検証や幾何精度を保証しません。

品質比較はデータ、ビュー、解像度、マスク、指標プロトコルが一致する場合に行い、
各指標の差と設定変更を示します。許容差はユーザーが設定します。証拠・しきい値・
独立検証が不足すれば未検証、比較条件が異なれば比較不能です。画像品質と幾何品質は
別に判定し、幾何参照や測定がない場合はメッシュ精度の合格を示しません。
このツールは入力された実測レポートを比較するだけで、測定や検証分割の証明は行いません。

3つの実行最適化は個別に無効化できます。解像度、SH次数、高密度化アルゴリズムや
有効な幾何損失を無断で変更しません。未対応の最適化器や外部描画版は元の処理を
保持します。低精度など結果を変え得る方式は明示的に選び、別の品質比較を必要とします。
