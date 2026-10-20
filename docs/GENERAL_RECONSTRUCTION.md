# 通用已知相机重建：初始化、深度点云与独立评价

本文说明原生桌面工作流对应的实际 worker 接口，不是模型精度、训练效果或安装验收证明。功能实现、CPU 合同测试、真实 CUDA 数学探针、实际训练与完整续训、独立几何评价、原生界面与安装验证是不同的验收层；不能以预览正常替代后几项。

这条链路保持已知相机的 `pose_policy=locked`，不重跑 SfM、不重新求解相机、不把 3DGS 训练替换为 2DGS。当前几何成果是**非融合的深度点云**：`fusion=none`、`appearance_proxy_not_material_boundary`。它不是三角网格，也不保证反光、透明或遮挡区域的物理材料边界。

## 1. 坐标、单位与数据隔离

所有相似变换采用正尺度、正规旋转：`p_out = s * R * p_in + t`。定义：

- `S = source_to_canonical`：来源坐标到规范坐标，保留尺度来源、约束、独立检核和可逆变换。
- `N = canonical_to_training`：规范坐标到训练数值坐标。数值归一化不等于新的物理尺度测量。
- `p_training = N(S(p_source))`；输出规范坐标用 `N^-1`，输出来源坐标用 `S^-1 ∘ N^-1`。每个变换只应用一次，应用状态、逆变换与原链均保留。

| 坐标框架 | 数值与物理单位 | 深度长度换算到该输出框架 |
| --- | --- | --- |
| `source` | 来源单位；已知时 `meters_per_unit` 为来源坐标每单位的米数 | `1 / (S.s * N.s)` |
| `canonical` | 有有效物理尺度时为米；否则仍为未知单位 | `1 / N.s` |
| `training` | 训练数值单位；已知物理尺度时每单位为 `1 / N.s` 米 | `1` |

`meters_per_unit=null` 就是未知，不能默认成 1 米。已声明或约束求得的尺度不等于经过独立验证的模型精度。`scale_status`、证据、数据身份和 `frame_id` 必须随成果保存。世界原点平移不改变相机 Z 的长度单位；深度阈值只按尺度比换算，不把平移加到深度值上。

相机使用完整逐视图内参 `K`、稳定 `view_id`、图像尺寸及 OpenCV 相机坐标约定。像素边界坐标下，反投影使用 `(u + 0.5, v + 0.5)`，而不是用对称 FoV 猜测主点。训练缩放后的 `training_K` 和实际执行的 float32 W2C／投影矩阵与原始双精度位姿分别记录。

`train`、`val`、`test` 划分属于数据身份。优化输入只能来自 `train`；本深度提取实现也只枚举 `train`，不解码源照片像素，不使用留出集或参考几何进行初始化、优化或融合。身份校验可以对所有图像做字节哈希及文件头尺寸检查；这不等于把测试图作为训练输入。另行进行的留出图像评价不能回灌本流程。

## 2. 环境与命令约定

以下为 PowerShell 示例，命令不会因阅读本文自动执行。先将路径、真实 64 字符十六进制身份值和场景参数替换为当前工程的值。所有新报告、版本和输出目录都应放在独立的 D 盘工作区，不能覆盖原照片、原工程、已有模型或检查点。

```powershell
$Repo = "D:/path/to/GSW-source"
$CpuPython = "D:/Apps/GSW/runtime/envs/gsw_native/python.exe"
$Gs3Python = "D:/Apps/GSW/runtime/envs/gaussian_splatting/python.exe"
$Gs2Python = "D:/Apps/GSW/runtime/envs/two_dgs/python.exe"
Set-Location $Repo
```

解释器位置是示例，以原生设置／运行时配置为准。CPU 工具需要 NumPy、Pillow、plyfile 等现有 worker 依赖；提取必须使用相应后端的现有 CUDA 环境，不能用 CPU Python 冒充真实光栅化。不要为运行示例盲目升级 Torch、CUDA 或安装整套第三方工程。

3DGS 使用通过来源／二进制证明校验的 full-K rasterizer；必要时由运行时配置设置 `GSW_CAMERA_RASTERIZER_ROOT`。2DGS 使用配置的官方来源目录 `GSW_TWO_DGS_SOURCE`／`TWO_DGS_DIR`。提取时 Python、Torch、CUDA、rasterizer wrapper／binary 及 3DGS proof 身份必须匹配实际训练合同。

所有 JSON 文件保存为 UTF-8。三个几何／比较 CLI 的机器输出固定为 UTF-8，不依赖 Windows cp932。`cancel` 加换行通过所属子进程 stdin 请求取消；保持输入管道打开不会阻止正常完成。不要用关闭终端或结束其他训练进程替代取消。

## 3. 检查并保存可复用初始化

入口：

```powershell
& $CpuPython -B native/worker/initialization.py `
  --operation inspect --dataset D:/GSW-work/dataset `
  --request D:/GSW-work/init-request.json --report D:/GSW-work/init-inspect.json
```

`init-request.json` 的随机初始化示例：

```json
{
  "schema": "gsw.initialization_request/1",
  "input_data_identity": "<current-dataset-data_identity-64hex>",
  "backend": "3dgs",
  "resolution": 1,
  "initialization": {
    "kind": "random",
    "frame": "source",
    "bounds": [[-1, -1, 1], [1, 1, 3]],
    "count": 10000,
    "seed": 7,
    "priors": []
  }
}
```

这里的包围盒只是字段示例，**不是任意对象的通用有效范围**。必须根据明确的来源坐标与实际可见范围编辑并检查。随机点先在声明的盒中采样，再变换到规范坐标；显示用规范 AABB 不替代原采样体积。输入框架支持 `source`／`canonical`，不把训练归一化坐标伪装为来源坐标。点数范围为 4～10,000,000，随机种子是 uint32。

`inspect` 只运行 CPU 几何诊断。`gsw.initialization_report/1` 包括实际规范 float64 点身份 `point_sha256`、数量、声明／规范／训练包围范围、变换、训练相机覆盖、内核裁剪约定、假设和错误。缺少随机盒时为 `needs_input`；全部点被某训练相机近裁剪或像幅排除时不能当作 `ready`。稀疏的显示预览不替代完整点集覆盖计算，也不证明表面或物体范围正确。

可选 `--operation suggest` 接受显式 `suggestion`：

- `method="optical_axis_convergence"`，可指定训练 `view_ids` 和 `half_extents`。需要充分秩及正向射线支持；平行、退化或交汇在相机后方时不猜物体位置。
- `method="user_depth_interval"` 使用明确的用户深度区间。它是可编辑的假设，不是从照片恢复的真实表面深度。

建议不会自动采用。光轴交汇残差、相机轨迹范围和可见性均不是模型精度。提供的外部点云／SfM 初始点必须保持 `external_point_cloud`／`sfm_sparse` 来源、相对路径、哈希及先验声明；不能将评价／真值几何标成无先验随机初始化。`ground_truth`、`evaluation_geometry`、`test_geometry` 先验不允许进入这条无评价反馈的初始化接口。

检查通过且用户确认后，创建全新数据版本：

```powershell
& $CpuPython -B native/worker/initialization.py `
  --operation create-version --dataset D:/GSW-work/dataset `
  --expected-identity <current-dataset-data_identity-64hex> `
  --request D:/GSW-work/init-request.json --report D:/GSW-work/init-created.json `
  --output-directory D:/GSW-work/dataset-init-v1
```

新版本复制已验证的关联字节，保存原 manifest、请求／报告哈希、谱系及实际规范 float64 NPZ 初始化产物。生成的随机点文件仍是随机配方的物化结果，不因存在 NPZ 就变成“外部几何先验”。保存重用时重放配方和哈希，不只信任 JSON 中的状态。原数据和旧模型不被改写；原生采用新版本是明确操作。

## 4. 使用新版本实际训练

初始化检查不启动训练。下面的显式 worker 配置演示已知相机锁定路径：

```json
{
  "task": "training",
  "repositoryRoot": "D:/path/to/GSW-source",
  "datasetPath": "D:/GSW-work/dataset-init-v1",
  "cameraDatasetPath": "D:/GSW-work/dataset-init-v1/camera_dataset.json",
  "cameraDatasetIdentity": "<new-dataset-data_identity-64hex>",
  "posePolicy": "locked",
  "runColmap": false,
  "resolutionPolicy": "preserve",
  "backend": "3dgs",
  "quality": "quick",
  "trainingSeed": 42,
  "trainOptions": {"iterations": 1000, "resolution": 1},
  "nativeCheckpoint": true,
  "outputRoot": "D:/GSW-work/output",
  "outputScene": "condition-a",
  "jobStore": "D:/GSW-work/jobs",
  "overwrite": false
}
```

```powershell
& $CpuPython -B native/worker/gsw_worker.py --task training --config D:/GSW-work/train-a.json
```

该命令确实会启动对应后端训练；1000 次迭代只是最小配置示例，不代表质量达标。2DGS 使用另一份配置，将 `backend` 改为 `2dgs` 并使用独立 `outputScene`，不能复用同一结果目录或跨后端恢复状态。初始化诊断的后端／分辨率应与实际任务一致。显式锁定与 `runColmap=true` 冲突时拒绝，而不是悄悄重新求解相机。

实际模型目录保存 `training-camera-contract.json`、已执行的相机矩阵、初始化身份以及选定迭代的 `point_cloud.camera-scale.json`。完整续训须有匹配的原生 Adam／随机数／密度控制等状态，并通过数据、相机、尺度、种子和后端身份检查；对应入口为原配置加 `--resume-training`。复制 PLY、显示预览或重新跑一个阶段都不是完整优化器续训。

## 5. 实际 3DGS／2DGS 深度点云提取

`geometry-request.json`：

```json
{
  "schema": "gsw.gs_geometry_request/1",
  "model_directory": "D:/GSW-work/output/condition-a",
  "dataset_path": "D:/GSW-work/dataset-init-v1",
  "iteration": 1000,
  "output_directory": "D:/GSW-work/geometry-a",
  "coordinate_mode": "canonical",
  "alpha_threshold": 0.5,
  "depth_range": {"min": 0.2, "max": 10, "frame": "training"},
  "pixel_stride": 1,
  "max_points": 50000000,
  "memory_budget_mb": 512
}
```

```powershell
# 3DGS 模型：使用其实际训练 Python／内核环境。
& $Gs3Python -B native/worker/gs_geometry.py `
  --config D:/GSW-work/geometry-request.json --report D:/GSW-work/geometry-a-receipt.json
# 2DGS 模型应使用 $Gs2Python 和独立 request/output/report，不转换训练类型。
```

`output_directory` 和 receipt 必须为不存在的新目标，父目录已存在，并位于源模型／数据集之外。`dataset_path` 必须显式给出并与训练合同匹配，不根据附近文件夹猜测数据集。源 PLY、sidecar、prepared dataset、相机／划分／初始化身份和实际后端身份全部重验；不实例化会改写训练目录的上游 `Scene`。

### 两种深度定义

- **3DGS**：直接调用真实 rasterizer，黑背景下用预计算特征 `[1, z_view, 0]` 获得 `A=Σw_i`、`S=Σ(w_i*z_i)`，再求 `Z=S/A`。定义为 `expected_camera_z_gaussian_center_attribute_alpha_compositing`。这是 EWA alpha 合成的中心属性预期 Z，通过每个像素射线反投影成密集点，不是直接把 Gaussian 均值导出当表面。不会误用 stock 的未归一化逆深度矩，也不经过 RGB `[0,1]` clamp。初期提取明确固定 `antialiasing=false`，不是猜测训练设置。
- **2DGS**：真实 surfel rasterizer 的 `allmap[0]/allmap[1]`，定义为 `expected_camera_z_surfel_ray_intersection_alpha_compositing`。不是自动混用 median depth，也不把 3DGS 模型转训成 2DGS。

两者均为 **camera-Z**，不是沿单位射线的欧氏距离。mask 要求有限、正深度、有限有效 alpha、明确 alpha 阈值和深度区间；无效深度值为 0。alpha 是合成不透明度，不是物理正确性的概率。反射／透明／低覆盖处可能缺失或偏移，不自动补洞。

`depth_range.frame` 可为 `source`／`canonical`／`training`；省略时采用并记录实际训练 clipping。显式区间按一次尺度换算后与训练 clipping 相交，超出或不相交不能悄悄改内核近裁剪。stock 3DGS 中心裁剪约为 `z<=0.2` 训练单位；2DGS 有中心／逐像素近裁剪及用于失真项的 near/far 常量。仅修改投影矩阵的 znear 不等于修改实际内核裁剪行为。

`pixel_stride` 是显式质量／资源选择；默认 1。默认 `max_points=50,000,000`、`memory_budget_mb=512`，CPU 预算用于单视图暂存与分块反投影，不是显存预留承诺。预算不足会报错，不自动增大步长、降低分辨率、抬高 alpha 阈值或删已有模型。

### 固定产物根，不把 receipt 当 manifest

```text
geometry-a/
  manifest.json                   # gsw.gs_geometry/1，唯一相对路径根
  point_cloud.ply                 # float64 XYZ；非 Gaussian PLY、无 face
  point_cloud.camera-scale.json   # gsw.coordinate_export/1
  views/v000000/{depth,mask,alpha}.npy
  views/v000001/{depth,mask,alpha}.npy
```

`manifest.json` 记录视图、完整 K、实际执行矩阵、深度定义、mask、NPY 哈希、原始训练 camera-Z 单位及输出比率、数据／模型／配置／源码身份、完整尺度链和原训练合同。`geometry.coordinate_sidecar` 绑定实际坐标 sidecar 路径与 SHA。PLY 已应用 `training_to_output`；sidecar 使原生端按真实框架／单位读取，不能再重复应用变换。

`--report` 和 stdout 是小型 `gsw.gs_geometry_report/1` receipt，包含 `manifestPath`、`geometryPath`、summary；几何相对路径只能相对 **manifest 所在目录** 解析，不能相对 receipt。失败／取消可保留本次已完成视图的深度证据，但 `geometry=null`，不宣称完整点云成功；旧成果始终保留。重试使用新输出目录，不声称阶段重跑是优化器恢复。

原生作业仍使用 `task="mesh"` 容器，但 `mode="gs_depth"` 输出明确为 `kind="depth_point_cloud"`，返回 `geometryPath`／`manifestPath`，不是 `meshPath`。`cameraDatasetPath` 必须显式传入；`meshOptions` 映射为上述 snake_case 提取参数，不接受旧 TSDF／贴图参数。

原生对话框明确默认 `training`，以便与原训练 Gaussian 的数值框架对照；CLI 默认仍是 `canonical`。现有多对象视口只统一单位和显示原点，尚未统一应用所有已知坐标链的旋转／平移，因此原生选择 `canonical`／`source` 的成果会登记保存，但不自动叠加到训练框架模型上。请在独立评价或同框架工程中使用；不能把已知长度倍率当成完整坐标配准。

## 6. 独立几何评价

`evaluation-request.json` 示例：

```json
{
  "schema": "gsw.geometry_evaluation_request/1",
  "evaluation_frame": {"id": "survey", "unit": "m"},
  "candidate": {
    "manifest_path": "D:/GSW-work/geometry-a/manifest.json",
    "to_evaluation": {
      "s": 1, "R": [[1,0,0],[0,1,0],[0,0,1]], "t": [0,0,0],
      "input_frame": "<exact-geometry.frame_id-from-manifest>", "output_frame": "survey"
    }
  },
  "reference": {
    "path": "D:/GSW-work/independent-reference.ply", "kind": "mesh",
    "sha256": "<actual-reference-file-sha256-64hex>",
    "frame": "survey", "unit": "m",
    "to_evaluation": {
      "s": 1, "R": [[1,0,0],[0,1,0],[0,0,1]], "t": [0,0,0],
      "input_frame": "survey", "output_frame": "survey"
    },
    "provenance": {"role": "independent_reference", "source": "<reference-origin-and-separation-from-model>"}
  },
  "sampling": {"count": 10000, "seed": 19},
  "coverage_threshold": 0.02,
  "quantiles": [0, 0.5, 0.9, 0.95, 0.99, 1],
  "dimensions": [{"id": "known-x-feature", "kind": "axis_extent", "axis": [1,0,0]}]
}
```

```powershell
& $CpuPython -B native/worker/geometry_evaluation.py `
  --config D:/GSW-work/evaluation-request.json --output-directory D:/GSW-work/evaluation-a
```

只有在已知框架确实相同时才能使用示例单位变换。**不自动配准或猜单位**。`to_evaluation.s` 是完整的输入到评价框架数值尺度，必须已经包含单位换算；例如毫米坐标到米框架应明确使用 `s=0.001`，不会再自动乘一次。平移 `t` 用输出评价单位。报告分别保留声明的单位换算比和 `scale_relative_to_unit_conversion`，避免把人为缩放隐藏成原始尺度准确。

候选可用 `manifest_path`、普通 XYZ PLY／三角 PLY，或显式有限 `plane_patch`；描述符类型互斥。raw Gaussian PLY 不被当作表面。面索引必须是整数，显式 `kind` 必须匹配实际数据。候选和参考选同一文件或其文件系统别名时拒绝。`independent_reference` **只是用户来源声明，不等于软件已鉴定参考真实性或完整独立性**；报告记录这一边界，不签发物理精度证书。

评价保留：

- 候选→参考、参考→候选的距离与均值、中位数、RMSE、分位数；距离样本和查询点保存为 `distances.npz` 并绑定 SHA。
- 参考采样中低于阈值的覆盖数、缺失数、覆盖率；单向误差很小不能掩盖缺面。空重建、ROI 无查询、非有限距离分别计数，不能写成零误差。
- 显式轴向 feature extent 的原尺度有符号／绝对／相对偏差。此功能量测用户定义的输入点／网格顶点范围，不自动识别物理对象尺寸；分位范围也必须明确保存。

网格按三角面积均匀重心采样；点云为全部点或固定种子的无放回抽样。有限解析平面可指定 `sampling.reference_grid_shape` 进行单元中心采样，其乘积必须等于 count。可选 ROI 是评价框架下的闭合 AABB，只筛选查询样本，**不裁掉目标几何或隐藏 ROI 内缺失面**；ROI 前后计数均保留。

点目标优先用 SciPy `cKDTree` 精确最近点，未安装时用有界 NumPy 实现。点云最近点距离／覆盖率依赖采样密度，不是连续表面认证。三角目标为 float64 点到三角表面的无符号距离；当前纯 NumPy 路径限制 `查询数 × 目标数 <= 200,000,000`，超限明确拒绝，不静默抽样。输入顶点上限为 10,000,000、请求采样上限为 1,000,000；大网格需要用户明确调整采样或另用已验证加速评价器。

主结果始终是**原始已知框架结果**。可选 `diagnostic_registration` 只能使用显式索引对应和评价框架 region，求 rigid／similarity 的诊断结果，不是隐藏 ICP：保留 6／7 自由度、拟合尺度、锚点残差、前后结果和选择区域。诊断的 scale-fit 不覆盖主结果，也不能把锚点拟合残差或拟合后尺度改名为模型原始精度。

未知物理单位可在双方明确 `unknown` 的共同数值框架下生成 `measured_unknown_units`，不能报告毫米／米误差；未知与已知物理单位混用时拒绝。

## 7. 导出和核对任意比较 recipe

recipe 记录任意命名条件，而不是预设固定 A/B 实验。每个条件都绑定实际规范初始化点身份、数据／相机／划分、分辨率、种子、训练参数、提取参数及仓库关键源码哈希。

```json
{
  "schema": "gsw.reconstruction_comparison_request/1",
  "datasets": [{"id": "object-1", "path": "D:/GSW-work/dataset-init-v1", "data_identity": "<actual-64hex>"}],
  "conditions": [
    {
      "id": "condition-a", "label": "3DGS comparison", "dataset_id": "object-1",
      "training": {"backend": "3dgs", "quality": "quick", "trainingSeed": 42, "trainOptions": {"iterations": 1000, "resolution": 1}},
      "extraction": {"method": "gs_depth", "coordinate_mode": "canonical", "alpha_threshold": 0.5, "pixel_stride": 1, "max_points": 50000000, "memory_budget_mb": 512}
    },
    {
      "id": "condition-b", "label": "2DGS comparison", "dataset_id": "object-1",
      "training": {"backend": "2dgs", "quality": "quick", "trainingSeed": 42, "trainOptions": {"iterations": 1000, "resolution": 1}},
      "extraction": {"method": "gs_depth", "coordinate_mode": "canonical", "alpha_threshold": 0.5, "pixel_stride": 1, "max_points": 50000000, "memory_budget_mb": 512}
    }
  ]
}
```

```powershell
& $CpuPython -B native/worker/reconstruction_comparison.py --operation export `
  --request D:/GSW-work/comparison-request.json --output-directory D:/GSW-work/recipe-a --repository $Repo
& $CpuPython -B native/worker/reconstruction_comparison.py --operation verify `
  --profile D:/GSW-work/recipe-a/profile.json --repository $Repo
```

`export`／`verify` 都**不执行训练、提取或评价**。这里的“重放”是重新读取实际输入、解析当前有效配置并核对完整 portable record，不是按一个保存的 SHA 就信任配置。`profile.json` 是 `gsw.reconstruction_comparison/1`，包含 `comparison_sha256`；标签、条件数、后端和显式提取参数保留，不接受未知控制项或评价数据输入。

数据迁移时可用 `--dataset-paths D:/GSW-work/dataset-map.json`，其内容为 `{ "object-1": "D:/new-location/dataset-init-v1" }`。只重新绑定声明的定位路径；数据／初始化／参数／源码身份仍严格核对，不因 Save As 猜测新输入。源码仅按声明的 `text-lf-v1` 统一 CRLF/LF，其他字节变化不是可忽略更新。

recipe 的仓库源码 pin 不能认证外部 2DGS 来源或已编译环境；`runtime_validation` 明确标记尚未执行／未认证。真实后端身份必须来自实际结果的训练合同：Python、Torch、CUDA、rasterizer wrapper／binary、3DGS proof 等。recipe 导出成功不代表运行时可用、训练结果相同或科学条件等价，`scientific_equivalence_certified=false`。

## 8. 能力矩阵与发布边界

| 能力 | 原生 3DGS | 原生 2DGS | 当前边界 |
| --- | --- | --- | --- |
| 已知 full-K 相机锁定、初始化检查／独立版本 | 对应接口 | 对应接口 | CPU 可见性不是训练或几何精度证明 |
| 真实 rendered camera-Z→mask→点云 | EWA 中心属性预期 Z | surfel 射线交点预期 Z | 非融合 appearance proxy，深度定义不同 |
| 完整优化器暂停／续训 | 后端自身状态 | 后端自身状态 | 必须实际验证数据／相机／尺度／RNG／优化器身份；不可跨后端 |
| 独立几何评价／比较 recipe | 共用接口 | 共用接口 | reference 隔离；无自动主结果配准／精度认证 |
| canonical 输入下的外部 bounded／unbounded TSDF、SuGaR、GS2Mesh、OpenMVS | 未开放 | 未开放 | 外部相机／深度／单位／划分适配与真实验收仍未完成 |

不因新点云能显示，就放开旧外部网格／纹理读者的 canonical 安全门；把 PLY 坐标恢复成 source 也不能补齐相机、depth 或 split 适配。新增深度路径不提供 TSDF、三角重建、贴图、PBR、透明材料恢复或自动补洞。

第三方训练器／rasterizer 的原许可证仍适用，桌面主程序的 GPL 声明不重新授权这些受限研究组件。不要把外部源码或运行库直接拷入 GPL 桌面程序后宣称许可问题已消失；参见 [GENERATION_PIPELINES.md](GENERATION_PIPELINES.md) 和 [native/LICENSE.md](../native/LICENSE.md)。

### 验收证据与安装状态

本文不宣称本轮真实训练、完整续训、全部 GitHub 检查或本地安装已完成。发布前须由交付记录补齐匹配同一提交／源码的证据：两后端实际增密／裁剪与完整状态续训、训练模型深度提取、已知尺度和独立参考评价、取消与文件保护、三语言原生生命周期、构建／安装包及本地程序／快捷方式核验。CPU 测试和控制几何内核探针单独记录，不能替代上述证据。

维护者验收索引：本轮对应 `.planning/2026-10-20-general-reconstruction/`，正式发布证据链接由交付记录补充；未补充前不要将 `release_accepted` 或安装状态描述为已通过。

English: This is an unfused, rendered-depth appearance proxy, not a certified material surface. Known camera/frame/scale/split contracts stay explicit. Original known-frame measurements remain separate from fitted diagnostics. Recipe export/replay starts no jobs and does not certify external runtimes or physical accuracy.

日本語：本機能は融合を行わない描画深度由来の外観プロキシであり、物理的な材質境界や精度を保証しません。既知カメラ・座標・尺度・データ分割の契約を維持し、元の既知座標での評価と診断用の尺度フィットを分離します。レシピの出力・照合だけでは学習を実行せず、外部実行環境や精度の認証にもなりません。
