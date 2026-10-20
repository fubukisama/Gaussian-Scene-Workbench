# P1：定尺度、参考检核与可追溯批量训练

更新：2026-10-20。本文是当前原生实现的用户指南、协议和验收边界，不是本次发行版已交付、远端 CI 全绿或实物模型精度合格的声明。最终提交、原生安装包、三语窗口和完整 CI 以交付验收记录为准。P0 的相机、像素坐标、初始化及划分协议见 [CAMERA_SCALE_P0.md](CAMERA_SCALE_P0.md)。

## 1. 先区分四件事

| 记录 | 说明 | 不能推出的结论 |
|---|---|---|
| 来源声明 `source_declared` | 来源或用户明确提供了单位 | 相机、尺度或实物模型已经独立检核 |
| 约束求得 `constraint_derived` | 由所列距离、基线或控制点求得正尺度／相似变换 | 拟合残差就是模型几何精度 |
| 独立检核 `independently_checked` | 明确声明独立、没有参与拟合的所列参考约束通过检核 | 整个重建模型、所有表面或全部相机都有同等精度 |
| 模型几何精度 | 需要独立模型／实物参考、配准与任务指标的另外验证 | 不能由上述标签、预览、坐标往返或合成数据的零残差代替 |

P1 报告固定记录 `scope=scale_and_reference_constraint_agreement` 和 `model_geometry_accuracy=not_measured`。`independent_of_fit=true` 是用户对参考来源的声明，加上程序可验证的角色与身份隔离，不是测量设备认证。参考测量自身的不确定度、点号及取点方法仍需保留。

手工输入源点时，程序不替你证明该坐标实际落在正确的实物标志上。教程中的检查值是生成数据，仅用于检验软件数学与流程，不能变成你的数据的实物精度证书。

## 2. 原生操作流程

### 相机数据集

1. 先导入已知相机数据集，检查来源、完整 K、图像尺寸、`train/val/test`、单位及初始化。锁定输入不自动重跑 COLMAP。
2. 打开“工作流 → 相机数据集定尺度与对齐…”。坐标必须来自原始源框架，而非视口局部显示偏移或已经归一化的训练坐标。
3. 选择“仅定尺度”或完整相似对齐，分别填写拟合与检核约束；明确填写拟合冲突容差和检核容差。
4. 点击“估计并独立检核”。检查逐约束残差、参考不确定度、退化诊断、独立性范围及警告。修改参数会使旧估计失效；不会拿检核值重新拟合。
5. 另行点击“创建并使用新数据版本”。失败的独立检核阻止创建；没有检核或检核不充分只能保留为约束求得。原图、原版本及旧检查点不修改。
6. 使用新版本启动新任务。相机、尺度或约束合同改变时，不能把旧优化器检查点直接套到新版本上。

完整对齐会显式改变规范框架中所有相机中心和方向，但不重新求解相机：原始 `source_c2w_opencv` 与 K 保留；同一个全局正 Sim3 应用于原始相机和几何，再单独记录训练数值归一化。它不是逐相机位姿优化。

### 已有模型

选中 PLY 模型，打开“工作流 → 当前模型定尺度…”。模型输入身份绑定 PLY、关联纹理及相邻尺度 sidecar 的内容，不是把 PLY 文件 SHA 随意填成数据集身份。没有相机来源的模型不提供相机基线约束，也不会伪造相机或训练合同。

源坐标取原始 PLY 文件，不包含当前场景对象的临时编辑变换、显示局部化或观察视角。若要定尺度的是已编辑后的几何，应先按现有导出能力另存新模型，再对该文件坐标建立约束。

“创建新模型版本／变换记录”与估计分开。原模型保留；工程登记新的版本目录。保存、重开与 SaveAs 保留成果引用和文件，打开工程不会启动训练或重新应用变换。合法但丢失的目录保留为 `missing`；被替换成链接的目录是 `unsafe`，不会当作可用成果跟随读取。

## 3. 约束请求 `gsw.scale_constraints/1`

三个可编辑模板：

- [距离定尺度](examples/camera-scale-p1/constraints-distance.json)
- [相机基线](examples/camera-scale-p1/constraints-baseline.json)
- [控制点 Sim3](examples/camera-scale-p1/constraints-control-sim3.json)

模板的身份、点坐标、长度、误差和来源都是示例，必须按实际输入替换。不是只替换哈希就能得到有效的实物约束。

| 字段 | 要求 |
|---|---|
| `input_data_identity` | 使用当前检查接口返回的身份；输入变化则旧请求失效 |
| `input_frame` | 当前仅 `source`；不能混入训练坐标或视口坐标 |
| `mode` | `scale_only` 或 `sim3` |
| `fit_tolerance_m` | 显式、有限且非负，以米为单位；用于拒绝拟合冲突，不是精度声明 |
| `id` / `reference_id` | 非空稳定标识；观测和几何选择不能跨拟合／检核角色重复使用 |
| `role` | `fit` 或 `check`；检查值不参与中心化、旋转、尺度、权重或阈值选择 |
| `reference_unit` | `m`、`cm`、`mm`；内部统一换算为米 |
| `weight` | 正的相对权重，默认 1 |
| `reference_sigma` | 可选的正有限参考不确定度，单位同 `reference_unit`；未知时省略，不填 0 或 null |
| `provenance` | 保留来源、`measurement_id`、测量类型及不确定度说明等 |

提供 sigma 时，有效权重为 `weight / sigma_m²`；未提供时只使用相对权重并记为不确定度未知，不把拟合残差反算成实物精度或零参考误差。

### 三类几何

| 类型 | 源数据 | 参考数据 |
|---|---|---|
| `distance` | `source_a`、`source_b`，原始源坐标中的两个三维点 | 正的 `reference_distance` |
| `baseline` | `view_a`、`view_b`，规范数据集的稳定 view ID | 两个相机光学中心之间的已知距离，不是相机机身尺寸 |
| `control` | `source_xyz` | `reference_xyz`，以 `reference_unit` 表达的局部笛卡尔坐标 |

基线只从原始源位姿取相机中心，不从规范化后的显示中心猜测。拟合可以显式引用 val/test 相机的校准几何，报告会记录这一点；对应图像仍不参与训练，但同一几何不能再冒称独立留出检核。

### 两种求解模式

`scale_only` 求正尺度 `s`，保持 `R=I, t=0`。一条有效距离／基线即可定尺度；不由此推断方向、位置或 CRS。控制点至少两个，通过同角色内的无序点对距离贡献尺度；点对相互相关，其调和权重总和归一回原控制行的有效权重总和，不假装增加独立观测数量。尺度检核若只给一个控制点，会保持未验证。

`sim3` 求：

```text
p_metric = s R p_source + t
s > 0, RᵀR = I, det(R) = +1
```

至少三个非共线拟合控制点；距离／基线可以共同参与尺度目标：

```text
Σ_control w ||s R x + t − y||² + Σ_distance w (s d − L)²
```

不是先拟合控制点，再把距离仅作为展示项。实现拒绝重合、共线／秩不足、非有限值、负尺度、可观测的全秩反射及超出声明容差的冲突。不自动删除离群点。平面控制可以求 proper 3D 旋转，但不能证明离面手性；需要分布合理的离面检核，不能由平面零残差推断整体正确。

相同／反向的距离端点、相机对、控制点及稳定观测标识不能通过改名跨角色复用；把一条拟合距离改写为两个检核控制点也不形成独立信息。

### 独立检核状态

检核行须另有 `independent_of_fit=true`、非空 `provenance.source`、`provenance.measurement_id` 和显式 `tolerance_m`，才可判定通过／失败。

| 状态 | 行为 |
|---|---|
| `not_provided` | 没有检查约束；不是通过 |
| `unverified` | 缺少独立来源、阈值或充分几何；不升级成独立检核 |
| `pass` | 仅所列、声明独立且隔离的参考约束满足阈值 |
| `fail` | 保留失败结果，不重拟合；阻止创建新的定尺度版本 |

用户阈值与浮点数值裕量分别保存；原始残差不被静默四舍五入成 0。参考／检核数据一旦改变，旧结果身份与续训合同不能继续沿用。

## 4. 报告、版本与恢复

解报告 `gsw.scale_solution/1` 保存原始请求、规范化请求、各自 SHA256、求解器版本、输入身份、正 Sim3 及逆变换、逐行残差、不确定度、秩／分布诊断、检查状态和范围。报告的 `solution_sha256` 只排除它自身字段；没有影响重放身份的运行时时间戳。

支持的 NumPy／LAPACK 版本可能出现几个 ULP 的 SVD 数值差异。重放先检查原始内容与自身哈希，再只允许明确记录的派生浮点裕量；输入、角色、权重、不确定度、阈值、离散诊断和 pass/fail 保持精确检查。成功重放保留原解和原摘要，不悄悄改写身份或把失败变通过。

数据集新版本保留 `camera_dataset.json`、`camera_dataset_info.json`、原始输入清单归档、请求、报告、来源文件／图像的独立副本及谱系。`scale.derivation` 进入数据身份、训练合同、完整检查点和 PLY sidecar。只有可重放验证的 GSW 内部派生记录取得 `gsw_replay_verified`；导入一个声称 independently_checked 的 JSON 不会自动取得这一资格。

若校正了原有单位倍率，新记录可使用 `source_unit=source_units`，同时在谱系保留原声明单位和旧变换，避免把校正后的数值仍误标为原 cm/mm 的标准换算。不要靠修改单位标签改变几何。

已有模型版本保存 `constraints.json`、`solution.json`、`transform.json`、`model-scale-version.json` 和 `source/` 来源副本。源模型、纹理、sidecar 与输入描述分别绑定哈希；支持的烘焙结果另有 `scaled.ply` 和对应 sidecar。

工程成果 registry 仅持久保存 `{kind,path}` 相对目录，不把“目录可用”当成验证通过。SaveAs／数据迁移不改报告、约束、计划或检查点原字节。批量计划的受管数据引用在执行时按新工程位置重定位并重新核验身份；外部引用仍明确指向外部路径，不猜测同名替代文件。

资源保护是显式拒绝，不是隐藏抽样：约束窗口最多 4096 行；scale-only 控制点成对报告最多 100000 对；请求和辅助元数据／报告有 16 MiB 上限；派生谱系深度有 8 代保护。Sim3 的线性控制点拟合不受成对展开上限，但仍受整体元数据限制。这些不是模型显示预算或坐标范围限制。

### 命令行：先检查，再估计，再创建

使用已配置的 GSW 辅助 Python 和实际安装根目录；以下路径只是用户工作目录示例。报告父目录要存在，所有输出名必须是新的，且数据集报告／新版本不能写回输入数据集。

```powershell
$Python = '<已配置的辅助 Python 绝对路径>'
$Install = 'D:/Apps/GSW/native/<实际安装版本目录>'
$Helper = Join-Path $Install 'native/worker/scale_constraints.py'
$Dataset = 'D:/GSW-Example/Project.files/datasets/original'
$ReportRoot = 'D:/GSW-Example/reports/new-run'
New-Item -ItemType Directory -Path $ReportRoot -Force | Out-Null

& $Python $Helper --operation inspect --dataset $Dataset --report "$ReportRoot/input.json"
if ($LASTEXITCODE -ne 0) { throw '输入检查未通过' }
$Identity = (Get-Content -Raw -LiteralPath "$ReportRoot/input.json" | ConvertFrom-Json).data_identity
```

把模板中的点／长度／来源改成实际观测，再填入 `$Identity` 并另存为 `request.json`。模型入口用 `--model <PLY>` 替代 `--dataset`；其身份必须来自检查报告，不能只取 PLY 文件哈希。

```powershell
& $Python $Helper --operation solve --dataset $Dataset --expected-identity $Identity `
  --request "$ReportRoot/request.json" --report "$ReportRoot/estimate.json"

# 人工检查结果后，才明确创建一个新目录；不会启动训练。
& $Python $Helper --operation create-version --dataset $Dataset --expected-identity $Identity `
  --request "$ReportRoot/request.json" --report "$ReportRoot/created.json" `
  --output-directory 'D:/GSW-Example/Project.files/datasets/scaled-version'
```

`solve` 与 `create-version` 分别重新准入／重放。报告名不能复用覆盖；失败先看 typed code，不通过改文件名或改标签绕过身份、阈值和反射保护。发布前取消／失败清理本次 staging；已经提交的公开新版本不会因迟到的取消被偷偷删除。Python 接口见 `native/backend/scale_constraints.py` 和 `native/worker/scale_constraints.py`。

## 5. 模型变换与导出能力

| 对象／操作 | 当前行为 |
|---|---|
| PLY 点云／三角网格正 Sim3 | 另存 XYZ；已识别法向随 R 转换；面拓扑、UV 与纹理受保护复制 |
| 3DGS 正尺度／平移 | 中心与三个 log-scale 同步；不改变 SH、不透明度或方向 |
| 2DGS 正尺度／平移 | 中心与两个 surfel log-scale 同步，不硬套三维尺度数组 |
| 已识别高斯协方差／旋转 | 按正 Sim3 转换，并遵守参数约定；不是只改 XYZ 或单位标签 |
| 旋转且没有方向性 SH 的受支持模型 | 支持对应几何／法向／高斯方向变换 |
| 旋转且含 `f_rest_*` 方向性 SH | 新版本为 `transform_manifest`，保留源模型和变换，不生成假称正确烘焙的旋转 PLY |
| 非均匀缩放、剪切、镜像、CRS 重投影 | 不在本正 Sim3 能力内，不静默拟合为支持 |

`transform_manifest` 是未烘焙的变换记录，不是新的训练结果或完整状态恢复。已有 Gaussian 训练输出若恢复到一个需要 SH 旋转的源框架，原有 typed 拒绝仍有效；保留训练／规范框架与清单，不能靠解除导出限制冒充支持。

只有明确识别的几何／向量／高斯字段会按约定转换。任意自定义属性的字节保留，不表示其未知向量／张量语义已经适配。局部 Sim3 也不建立 CRS：新输出不能把旧经纬度／投影标签继续当作有效重投影声明，旧声明仅作来源记录。

## 6. 更多来源：明确版本，不猜协议

### 摄影测量软件

Metashape 的官方导出菜单支持 COLMAP 相机格式；GSW 走已有的 COLMAP text/bin 适配器，检查实际相机模型、图像对应和单位，不直接打开 `.psx/.psz` 工程，也不自动读取其控制点／精度结论。必须提供当前支持的针孔、已去畸变图像及一致内参，不能把导出的畸变模型强标成零畸变。官方说明：[Metashape 2.3 用户手册](https://www.agisoft.com/pdf/metashape-pro_2_3_en.pdf)、[导出接口的 pinhole 选项](https://www.agisoft.com/pdf/metashape_python_api_2_2_3.pdf)。

这证明有文档化的交换格式入口，不代表所有 Metashape 版本／设置组合、原生工程或其他摄影测量软件工程均已验收。

COLMAP 交换文件本身不表达 GSW 的完整 train/val/test 选择规则。需要留出集时，应在规范相机清单中明确 split 并重新准入，不依赖 `test/` 目录名猜测，也不能修改已固定身份的清单后继续保留旧 identity。

### 单相机轨迹

[轨迹清单示例](examples/camera-scale-p1/trajectory_dataset.json) 与 [CSV 示例](examples/camera-scale-p1/poses.csv) 必须放在真实图像根目录并改成实际数据；示例不附图像。也可把清单的 format/path 改为 `gsw_pose_json_v1`／[poses.json](examples/camera-scale-p1/poses.json)。

| 格式 | 明确要求 |
|---|---|
| `tum` | 原始文本 `timestamp tx ty tz qx qy qz qw`；明确 `c2w`、`opencv`、`source_unit` 和四元数舍入容差 |
| `gsw_pose_csv_v1` | 严格表头 `timestamp_s,tx,ty,tz,qx,qy,qz,qw`；显式 c2w/w2c、OpenCV/Blender 约定 |
| `gsw_pose_json_v1` | `gsw.pose_trajectory/1`；每行仅一个与声明一致的命名位姿字段 |

清单 schema 为 `gsw.trajectory_dataset/1`。每幅图明确 path、十进制时间字符串和 split；可选稳定 view ID、单 sensor ID 及 calibration ID。一个或多个 calibration 必须提供 K、宽高、像素约定、受支持投影、明确 `undistorted=true` 和零畸变；不能从 fr1/fr2 文件名套焦距，不能在不知道畸变时改一个标签绕过检查。

时间采用 Decimal 精确比较，原拼写留作来源；JSON 中请用字符串或整数，不用已经丢失十进制精度的二进制 float。`association.mode` 是 `exact` 或 `nearest_one_to_one`，`max_difference_s` 与 `missing=reject|drop` 必须显式给出。唯一最近匹配才接受；并列或两图抢同一最近位姿直接拒绝，不贪心改配更远相机。drop 仅针对未匹配图并完整记录，不隐藏缺文件、坏 K、错误尺寸或重复身份。

轨迹仅单相机／单传感器，当前不支持 rig 外参、多时钟标定、时间偏移、插值、外推、滚动快门、漂移校正或受约束位姿优化。未知单位保持任意单位。TUM 的格式语义和数据许可来自[官方格式说明](https://cvg.cit.tum.de/data/datasets/rgbd-dataset/file_formats)与[官方数据页面](https://cvg.cit.tum.de/data/datasets/rgbd-dataset)。仓库只保留可追溯的三个实际轨迹行作格式夹具，见[来源／CC BY 4.0 记录](../native/backend/tests/fixtures/tum-fr1-xyz-first3/SOURCE.md)；配套测试图和 K 是另行生成的，不冒充 TUM 实际影像投影或几何精度验证。

## 7. 通用批量训练

“工作流 → 批量训练…”先编辑引用与参数，再“预览并核验数据身份 → 创建持久计划 → 启动所选批量计划”。打开／保存计划不会训练，继续也要单独请求。当前只接受已经准入、身份已固定的锁定相机数据集，以及 `3dgs`／`2dgs`。其他后端明确拒绝，不生成看似有效但参数未生效的任务。

[批量请求模板](examples/camera-scale-p1/training-batch-request.json) 的数据集路径与身份必须替换。其两后端 × 两 seed × 两试验编号产生八个串行任务；条件名称、数据集数量和可支持参数组合不是固定材料实验。

`repeat` 是独立试验编号轴：`[1,2]` 表示两次相同 seed 的试验，`[2]` 只是编号 2 的一次，不自动改 seed。`trainingSeed` 单独控制真实 Python／NumPy／Torch 训练随机序列；数据集声明的初始化 seed 不被批量计划覆盖。相同 seed 不承诺跨 GPU／库版本逐位相同结果。

| 参数类别 | 当前受支持字段 |
|---|---|
| 共同轴 | `backend`、`quality`、`trainingSeed`、`repeat` |
| `trainOptions.*` 共同参数 | iterations、resolution、densify_grad_threshold、densification_interval、densify_until_iter |
| 2DGS 专用 | depth_ratio |
| 3DGS 专用 | optimizer_type、antialiasing、exposure_compensation |

`initialization_policy=dataset_declared`、`evaluation_policy=declared_splits`，不重分测试图，不根据某个试件改初始化。参数需满足后端范围与兼容性，未知参数或把 2DGS 参数交给 3DGS 会拒绝。`declared_splits` 不新增独立评价证据；仍应检查源数据集的 `independent_test` 和跨 split 重复内容警告。

`performanceOptions` 是显式共享配置；质量优先可用 `growth_mode=pause`。`run_policy.on_failure` 为 stop 或 continue；continue 仅允许处理后续任务，已有失败仍保留且整体不标成功。最多 256 个明确展开任务，超过拒绝、不抽样。

持久目录为 `<workspace>/.gsw/training-batches/<id>/`，包含 `plan.json`、原始 configs、原子更新的 `state.json`、执行配置／日志、每任务输出及可用检查点凭据。计划固定输入身份、参数／seed／repeat、划分与训练源代码指纹。运行／继续会重新核验；完成项先验输出哈希，活动项只有可用完整优化器检查点才能继续。

批次之间的暂停只有“尚未开始下一项”的安全边界，不冒充一个优化器检查点；活动训练的暂停则通过原生完整状态路径保存。PLY 预览、打开报告或 Warm Start 均不算完整恢复。显式取消不自动续训，旧检查点不被改写成兼容新 seed／新尺度。

```powershell
$BatchHelper = Join-Path $Install 'native/worker/training_batch.py'
$Workspace = 'D:/GSW-Example/Project.files'
& $Python $BatchHelper --operation preview --repository $Install --workspace $Workspace `
  --request "$ReportRoot/batch-request.json" --report "$ReportRoot/batch-preview.json"
& $Python $BatchHelper --operation create-plan --repository $Install --workspace $Workspace `
  --request "$ReportRoot/batch-request.json" --report "$ReportRoot/batch-created.json"

# 从 create-plan 返回的 planPath 读取；下列两步会实际启动／继续训练，不能自动串在预览之后。
$Plan = '<明确选择的 planPath>'
& $Python $BatchHelper --operation run --plan $Plan
# 需要继续时另行选择，只有核验通过才启动：
# & $Python $BatchHelper --operation resume --plan $Plan
```

## 8. 路线逐项验收与保留项

以下区分本地已取得的行为证据和仍待交付的发行门槛；不是把计划中的能力写成全通过。

| 路线要求 | 当前实现／验收缝 | 状态与边界 |
|---|---|---|
| 距离、相机基线、控制点定尺度 | `test_scale_constraints.py`；原始源位姿取基线、单位／权重／不确定度、混合目标 | 本地 CPU 通过；不自动证明手工点位或参考实物真值 |
| 尺度与完整对齐分开 | scale-only 保持 R/t；Sim3 检查 rank、proper R、反射、冲突与百万原点 | 本地数值测试通过；平面手性限制明确保留 |
| 拟合与独立检核分离 | 改检查值不改变拟合解；角色／几何复用拒绝；fail 不重拟合 | 本地回归通过；生成的独立角色夹具不是实体测量证据 |
| 约束／变换／检核持久化 | 请求、解、派生谱系、哈希绑定，保存／重开／SaveAs，registry missing/unsafe | Python 事务与原生公开接口已本地验收；完整发行再核对对应源码／安装边界 |
| 两后端真实训练及完整续训 | 3DGS、2DGS 各实际 1000 步；暂停／完整状态恢复；固定校准、保留派生记录 | 本地 GPU 记录通过；COLMAP 启动为 0，val/test 训练采样排除，改 seed／测试图被拒绝，seed 改变真实采样；不宣称长周期收敛质量或实物精度 |
| 模型定尺度与高斯感知输出 | 原 PLY 保留、几何／log-scale／协方差、sidecar、纹理、原生测量 | 原生模型读取／拒绝检查与 CPU 版本测试通过；含 SH 的旋转保存未烘焙清单 |
| 更多摄影测量来源 | Metashape 官方 COLMAP 交换入口进入现有适配器 | 受格式／模型／去畸变条件约束；未实现原生 Metashape 工程解析，未声称所有软件版本实测 |
| 轨迹 CSV／JSON 与时间关联 | versioned TUM／GSW CSV／GSW JSON；Decimal exact/nearest、tie/reuse/drop、双 K、源归档、真实 worker 取消 | 本地两 Python 3.8 运行环境通过；实际小 TUM 轨迹仅格式证据，单传感器限定 |
| 受约束位姿优化／rig 外参 | 当前锁定相机不进入优化器，没有 rig 状态模型 | 未实现：需要分别固定自由度／尺度锚点、保存前后参数、建立后端及独立验收，不能仅加一个开关 |
| 通用批量条件／seed／初始化／评估 | 版本化请求、可变数据集条件、参数轴、串行任务、显式继续和来源身份 | 本地实际 3DGS＋2DGS 串行批次完成；读取真实 Adam／RNG 检查点、SaveAs 后续训、旧工作区保持暂停及校验成果后跳过均通过；原生窗口执行仍以最终远端记录为准 |
| 三语、旧流程、原生安装与远端 CI | 本地原生测试及 zh_CN/en_US/ja_JP 同步实现；完整发行构建／安装包／匹配提交检查 | 最终门槛须由交付记录确认；本文不声称已更新快捷方式或全部 CI 成功 |

本地验证记录截至本文编写时包含：核心／轨迹／P0 组合 67/67；原子文件／批次／种子／计划／检查点／规范初始化／相机数据组合在 QA 及两实际 Python 3.8 环境各 86/86；模型 CLI 各 13/13；原生模型检查 27 项。完整旧 worker 回归为 351 项，其中 51 项既有可选／独立 GPU 门槛未在该 CPU 命令内执行，另有本节所列真实 GPU 实训；不能把跳过当通过。Workspace 新增 registry 与真实模型纹理加载均通过，两个旧可选外部真实夹具未配置。数值与行为结果必须与对应源码一起使用，不能把这些有限测试扩张成所有环境或模型的精度证明。

目前 canonical 相机成果进入 TSDF、OpenMVS、SuGaR、GS2Mesh 等后续管线仍受既有显式能力门槛约束；缺少受验证的 full-K、深度／法向、split、尺度适配时拒绝，不以 P1 定尺度通过替代这些适配。非线性相机、复杂畸变、滚动快门、CRS、非均匀仿射及完整 SH 旋转烘焙同样没有被本次顺带宣称完成。

正式物理模型质量需要另外准备没有参与拟合的扫描／实物参考、明确配准和可解释的几何指标。拟合 RMSE、参考约束通过、合成往返误差、PSNR／SSIM 与物理表面精度是不同结论，应分别保存和报告。

本轮[生成小样本真实训练记录](validation/camera-scale-p1-synthetic-training.json)保留结果哈希、相机／划分保护、实际种子与批量迁移验证及匹配源码指纹；不包含原照片／工程路径，也不将它作为实体模型精度或原生窗口验收的替代品。

## 9. 实现与复查入口

批量计划以明确的 `source_hash_mode=text-lf-v1` 固定执行源码：仅把 Windows CRLF 与 Git 对应源码中的 LF 视为相同换行，其余字节改变均使身份变化。这与完整检查点已有的源码哈希规则一致，不会忽略代码、参数或数据变更。它不提供跨不同源码版本的优化器迁移；旧版本安装和检查点仍保留原样，不能用 PLY 代替完整状态恢复。

元数据与初始化采用同目录、独占创建且名称不长于目标文件的临时文件；冲突重试与失败清理不覆盖目标或其他文件。这避免合法 Windows 目标路径因附加长 UUID 临时后缀而失败，但不表示支持操作系统本身拒绝的超长目标路径。

- 纯求解／重放：`native/backend/scale_constraints.py`。
- 新数据版本及 CLI：`native/worker/scale_constraints.py`。
- 已有模型：`native/backend/model_scale_version.py`；原生 sidecar／测量在 `SceneCoordinates`、`PlyPointCloudLoader`、`ModelExport`。
- 轨迹：`native/backend/trajectory_dataset.py` 与共享 `camera_dataset` 准入。
- 批量：`native/backend/training_batch_plan.py`、`native/worker/training_batch.py`、`native/worker/training_seed.py`。
- 原生持久化：`WorkspaceDocument::workflowArtifacts/addWorkflowArtifact`；原生窗口为 `ScaleConstraintsDialog`、`TrainingBatchDialog`。
- 数学来源：[Umeyama 正相似对齐论文](https://web.stanford.edu/class/cs273/refs/umeyama.pdf)、[NumPy SVD 文档](https://numpy.org/doc/stable/reference/generated/numpy.linalg.svd.html)。实现的混合尺度目标、权重与拒绝策略以上述协议和测试为准，不把算法名称当作精度认证。

所有例子都是独立、可编辑的参数化协议输入；没有任何特定材料、试件尺寸、视点数量、本机用户名或历史训练检查点作为产品默认值。
