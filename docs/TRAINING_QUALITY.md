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

## Measure existing image pairs

The comparison window now has an explicit image-measurement entry. Choose
reference/render folders to pair exact filenames (including extension and case),
or use an advanced JSON manifest for different names and optional masks. Folder
pairing is nonrecursive, excludes hidden files and rejects links/missing pairs;
every reference must match. Refresh the pairs after changing folder contents.
The preview table shows at most512pairs, but the measurement processes all pairs.

This CPU-only producer accepts PNG/JPEG RGB8 images and optional grayscale8 masks.
It never starts a model renderer or trainer, resizes, registers, crops, removes
Gaussians or silently skips a failed view. Nonidentity EXIF orientation, alpha,
palette/16-bit PNG and mismatched sizes are rejected. Encoded RGB values are
divided by255 without ICC conversion or a claim that they are linear/sRGB
colorimetry. Mask values greater than zero select pixels; SSIM accepts only
complete11×11valid mask windows. At least one such window is required.

PSNR is computed from pooled valid-channel MSE, not an average of per-view dB.
SSIM uses normalized11×11Gaussian weights, sigma1.5, population covariance,
K1=.01/K2=.03, valid windows and a channel mean; the aggregate is weighted by
valid-window counts. This is a fixed evaluation protocol, not the training-loss
SSIM boundary convention. It is an independent implementation of the formulas in
[Wang et al.(2004)](https://ece.uwaterloo.ca/~z70wang/publications/ssim.pdf).

Example manifest; paths resolve against the original manifest's directory:

```json
{"version":1,"split":"unverified","views":[{"id":"view-001","reference":"reference/001.png","render":"renders/001.png","mask":"masks/001.png"}],"settings":{"backend":"2dgs","iteration":30000}}
```

Omit `mask` when not used. `split` may be `unverified` or explicitly
`training_fit`; paired images alone cannot prove `held_out`, which this producer
rejects. Existing externally produced held-out measurement reports remain
supported by the comparison tool, subject to their producer's provenance.

```text
python scripts/measure_image_quality.py --manifest pairs.json --output new-measurement.json
```

The report records every view's PSNR/SSIM/MSE, dimensions, effective counts and
source/render/mask hashes. Reference content, ordered view IDs, dimensions,
masks and the protocol define comparison identity; candidate pixels do not.
The protocol includes NumPy/Pillow and reported codec build versions. Use the
same CPU measurement runtime for comparable3DGS/2DGS/mesh-render experiments;
it need not be the training runtime. Decoder-version changes are not silently
treated as equivalent. The CLI's optional `--base-dir` preserves original path
resolution when a GUI snapshots the manifest.

Only explicit **Use as Baseline/Candidate** publishes a fresh application-owned
JSON and fills the comparison window. Closing the measurement window afterward
does not remove an adopted report. Reports live in the stable application
workspace base, outside disposable untitled-project children; they are not
automatically associated with an experiment or included in its backup. The
saved path is shown explicitly. Every adoption revalidates the original bytes;
an externally deleted/edited report is freshly republished without overwriting
the external change. Input hashing and decoding use the same bounded, owned
temporary snapshot, including during concurrent path replacement.
Cancel/close stops only its owned CPU helper;
no training or original file is modified. Independent validation remains
unverified, and no LPIPS/depth/normal/mesh accuracy is fabricated.

Resource limits are4096explicit pairs,64MiPixels per image,512MiB per file and
16MiB manifest/report. Tile computations bound additional numerical arrays;
the image decoder may still hold the decoded images. Exceeding a limit is an
explicit error, never automatic downsampling. The native helper has a10minute
timeout. CLI progress is JSONL on stderr; exit0=success,2=invalid/error,
3=cancelled. Existing output files are refused.

## Numerical and recovery boundary

Execution optimizations preserve the data, objective and state semantics; they
do not promise bitwise CUDA trajectories or identical final quality. Aggressive
3DGS growth remains an open repeatability gate on both the older and current
runtime. A bounded frozen-cotangent investigation found a gradient divergence
in observed training, but four replays were stable; opaque saved context and
observer effects remain unresolved. This is not a proven atomic-reduction cause
or a completed stability fix. No density threshold, random sequence or quality
assertion was changed to make that gate pass.

Opt-in CUDA acceptance now retains failed synthetic fixtures, child commands,
logs and `acceptance-status.json` under `.planning/checkpoint-test-evidence`.
`GSW_CHECKPOINT_TEST_EVIDENCE_ROOT` selects a writable evidence location and
also retains successful fixtures. It does not resume an original experiment.

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

图像实测入口在 CPU 上直接计算已有参考图／渲染图的逐视图与汇总 PSNR、SSIM。
可以按同名文件夹完整配对，也可使用高级 JSON 清单；不会自动缩图、配准、裁边或
过滤模型。参考图必须全部匹配，目录变化后可刷新配对；表格仅展示前512项，实测
仍处理全部。只支持 PNG/JPEG RGB8 与可选灰度8位遮罩，拒绝尺寸不符、alpha、
16位／调色板 PNG 和非正向 EXIF。编码 RGB 值直接除以255，不转换 ICC 色彩。
遮罩非零像素有效；SSIM 只使用完整有效的11×11窗口。

PSNR 来自全部有效通道的池化 MSE；SSIM 使用11×11、sigma1.5的归一高斯权重、
总体协方差与有效窗口加权汇总，不等同于训练损失的边界约定。报告保留每幅图的
指标、尺寸、有效数量与输入哈希。协议含测量依赖与编解码构建版本，跨3DGS、2DGS
和网格渲染图对照时应使用同一 CPU 测量环境，不要求训练环境相同。

没有留出集证明时明确保持“未验证”，不能仅勾选选项就变成独立验证。只有主动
选择“用作基准／候选”才保存全新报告并填入对照，关闭子窗口后已采用的报告仍在。
取消不影响训练或原图；不会补造 LPIPS 或几何精度。资源超限会明确报错，不自动降级。
已采用报告放在应用长期工作区基目录，独立于未命名工程的临时子目录；不自动绑定
实验或纳入工程备份，界面显示实际路径。重复采用会核验原始字节，外部修改或删除后
另存全新报告，不覆盖外部更改。输入哈希与解码来自同一份有界只读快照，避免并发
路径替换导致测量内容与哈希不一致。
这些执行优化不保证 CUDA 逐步位级一致；极端3DGS增密的重复一致性仍未通过，
已保留诊断证据，没有改变原增密算法或放宽断言。续训验收失败的合成样本与子进程
日志现在自动保留，原工程、照片和优化器检查点不受影响。

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

画像測定は既存の参照画像／描画画像について、各ビューと集計のPSNR・SSIMをCPUで
計算します。同名ファイルのフォルダー、または詳細JSONリストで対応を指定します。
全参照画像の対応を要求し、内容変更後は更新できます。表の表示は512件までですが、
測定は全件が対象です。リサイズ、位置合わせ、切り抜き、モデルの間引きはしません。
PNG/JPEGのRGB8と任意のグレースケール8ビットマスクのみを扱い、寸法不一致、alpha、
16ビット／パレットPNG、非正方向EXIFは拒否します。符号化RGBを255で割り、ICC変換は
しません。マスクは非ゼロが有効で、SSIMは11×11全域が有効な窓のみを使用します。

PSNRは有効チャンネルのプールMSE、SSIMは11×11・sigma1.5の正規化ガウス重み、
母共分散と有効窓数による集計です。学習損失の境界処理と同一ではありません。
報告にはビュー別指標、寸法、有効数、入力ハッシュを保存します。測定依存関係と
コーデックのビルド版もプロトコルに含めます。3DGS・2DGS・メッシュ描画の比較には
同じCPU測定環境を使い、学習環境を統一する必要はありません。

ホールドアウトの根拠がなければ未検証のままです。明示的に基準／候補として採用
した時だけ新しい報告を保存し、子ウィンドウ終了後も保持します。キャンセルは
学習や元画像に影響せず、LPIPSや幾何精度を生成したことにはしません。資源上限は
エラーとして示し、自動で品質を下げません。
採用した報告はアプリの永続作業領域の基底に保存し、未保存プロジェクトの一時
サブフォルダーとは分離します。実験やプロジェクトバックアップへの自動登録は
せず、保存先を表示します。再採用前に元バイト列を確認し、外部編集・削除後は
新しい報告を作り、外部変更を上書きしません。ハッシュと復号は同じ上限付きの
読み取り専用スナップショットを使い、同時パス置換による内容の食い違いを防ぎます。
実行最適化はCUDAのビット単位の軌跡
一致を保証せず、極端な3DGS高密度化の再現性は未合格です。密度アルゴリズムや
合格条件は変更せず、失敗した合成再開テストの入力・子プロセスログを保持します。
