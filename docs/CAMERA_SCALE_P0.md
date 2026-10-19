# 相机、尺度与训练划分：P0 协议和能力边界

更新：2026-10-19。本文件记录实现接口与验收边界，不是“所有测试已通过”或几何精度证书。真实 GPU、原生窗口、完整恢复、安装包及匹配提交的最终结果必须与本轮验收记录一起交付；预览正常不能替代这些结果。

## 三个独立维度

| 维度 | P0 记录 | 不表示什么 |
|---|---|---|
| 相机来源 | 原生/外部 SfM、合成来源、人工协议；适配器和来源文件哈希 | 来源名称不证明相机精度 |
| 位姿策略 | `locked` 或 `solve` | 锁定输入失败不允许自动改为求解 |
| 尺度状态 | `arbitrary`、`source_declared`、`constraint_derived`、`independently_checked` | 归一化、焦距、模型外观不产生公制依据 |

`constraint_derived` / `independently_checked` 在 P0 中只能保存外部输入的结构化证据，标记 `verification_origin=external_claim`。P0 不执行 P1 的距离、基线、控制点拟合，也不把来源声称转写成 GSW 独立实测精度。

## 输入适配器矩阵

| 输入 | P0 状态 | 语义和限制 |
|---|---|---|
| Blender/NeRF `transforms_train.json` | 已实现解析与规范化 | 另读可选的 `transforms_val.json`、`transforms_test.json`，不合并测试图进训练；允许嵌套路径和不同尺寸/内参 |
| COLMAP text / binary | 已实现解析与规范化 | `cameras`、`images` 必需；`points3D` 可选，空点云可转为明确记录的随机初始化；二进制优先 |
| 通用 JSON | 已实现 `gsw.camera_dataset/1` | 明确每视图 K、位姿、split、单位和变换链；不执行 JSON 内说明字符串 |
| 研究导出 `conventions + cameras[]` | 独立 `research_json` 适配器 | 读取声明的米制相机矩阵和 K，检查并存 Blender/OpenCV 位姿一致；不读取实验名义尺寸或求值字符串公式 |
| 摄影测量原生工程、任意 SLAM/轨迹格式 | 未实现 | 需要 P1 的具体格式/版本、时间和坐标关联验收，不能因 JSON/CSV 扩展名相同就宣称支持 |
| rig、滚动快门、复杂畸变、CRS 重投影 | 未实现 | P2 或独立适配器能力；不能把经纬度直接当米 |

P0 投影准入限于已去畸变的 `PINHOLE` / `SIMPLE_PINHOLE`。非零 skew、非零/未知畸变、不支持的投影、NaN、无效旋转、路径逃逸、缺图、尺寸冲突、单位冲突均拒绝。没有经过验证的自动去畸变/重采样路线，因而不会悄悄改图或改标定来“修好”输入。

COLMAP 文件格式与位姿定义依据其[官方格式说明](https://colmap.github.io/format.html)。读取器没有调用 COLMAP 转换或重建程序。

## 规范数据集

托管数据根包含 `camera_dataset.json`。图片保持原相对层级，托管文件是独立字节副本，不与原图硬链接；不同子目录同名文件不会按 stem 合并。

| 字段 | 含义 |
|---|---|
| `schema` | `gsw.camera_dataset/1` |
| `source` | 来源类别、适配器版本、世界/相机轴约定、原始标定文件路径和 SHA-256 |
| `pose_policy` | `locked` / `solve`，不是目录布局猜测结果 |
| `scale` | 状态、`source_unit`、可空的 `meters_per_unit`、`canonical_unit`、结构化 evidence |
| `views[]` | 稳定 `view_id`、相对 `image_path`、真实宽高、内容 SHA-256、完整 K、源/规范位姿、split，可选时间/传感器标识 |
| `splits` | 互不重叠的 train / val / test 视图 ID 集合 |
| `transforms` | `source_to_canonical` 和 `canonical_to_training` 的正尺度 Sim(3)、逆变换、帧名、应用状态及证据 |
| `initialization` | 随机 / 外部 PLY / SfM 稀疏点、坐标框架、bounds、数量、seed、文件哈希和先验类型 |
| `data_identity` | 图像内容、相机、split、策略、尺度、变换、初始化等规范内容的身份哈希；不是仅按文件名计算 |

规范 pose 使用 float64 数值、行优先 `c2w_opencv`，相机局部轴为右、下、前。`source_c2w_opencv` 单独保留源坐标数值；重新读取时不再次执行单位换算或翻轴。并存的来源位姿必须一致，百万量级原点不会把平移比较容差放大成整单位的允许偏差。

规范像素坐标为 `pixel_border`，左上像素中心为 `(0.5, 0.5)`。视图同时保存 `source_K`、`source_pixel_coordinates` 和规范 `K`：

| 来源约定 | 到规范 K 的转换 |
|---|---|
| COLMAP / 显式 `pixel_border` | 保留 K |
| 通用 JSON 默认 `opencv_integer` | `cx += 0.5`，`cy += 0.5` |
| 标准 NeRF 风格 Blender transforms 默认 integer ray 约定 | 同上，并记录源约定；可明确声明 `pixel_border` |
| 研究 JSON | 按 `conventions.pixels` 的明确声明，不按文件名猜 |

这不是焦距变化或相机重估。后端光栅像素中心、偏心主点和分辨率缩放必须使用同一约定；仅在列表里显示 K 不算完整训练投影验收。

`camera_dataset_info.json` 是原生 UI 的只读摘要，不是训练合同替代品。`source/metadata_manifest.json` 另记录原始选择、逐视图路径映射、复制模式和原文件哈希。摘要中 `dataVersion` 是显式版本或内容身份，不混用适配器版本。

## 公开 CPU 接口

实现位于 `native/backend/camera_dataset.py`：

```python
dataset = load_camera_dataset(source, adapter="auto", options=None, cancelled=None)
dataset = validate_camera_dataset(dataset, image_root, verify_images=True)
write_camera_dataset(dataset, destination_manifest)
points_and_colors = read_initialization_points(dataset, image_root)
```

`camera_dataset_root()` 独立返回引用根；绝对运行时路径不需进入图像/相机内容身份。重新定位整个托管副本后，内容未变时身份保持；图片字节、split、相机策略或变换链改变则不能冒充同一版本。

`native/backend/camera_admission.py` 提供：

```python
admission = admit_locked_dataset(source, expected_identity=None, run_colmap=False)
new_version = prepare_solve_dataset_version(source, fresh_destination,
                                            expected_identity=None, cancelled=None)
```

准入错误使用 `CameraDatasetError.code/detail`。接口不会在缺图、不支持投影、身份变化或空稀疏点的分支偷偷启动 SfM。

## 操作路线

### 已知相机，保持来源尺度

1. 选择“导入已知相机数据集”和实际适配器。来源没有公制声明时保留任意单位；用户声明单位必须作为明确 evidence 保存。
2. 核对来源、锁定策略、train / val / test 数量、源单位和初始化先验。不要把评价网格、深度、法向混入“无几何先验”初始化。
3. 默认保持分辨率。需要折中时明确选择手动缩放，实际训练宽高和缩放后的完整 K 会写进合同；没有悄悄降至 1600 像素的自动分支。
4. 无稀疏点可采用随机初始化，但相机轨迹不等于物体深度。覆盖退化、初始化不可见或范围不确定时应补充粗范围/合法点云，不能承诺任意场景自动初始化成功。
5. 启动后检查实际训练合同和 sampled view ID，而不是只看预览是否正常。

### 明确重新求解

从规范已知相机输入切换 `solve` 时创建新版本：仅 train 图像被独立复制到新版本的 `input`；val/test 图像字节不复制到 SfM 输入。原标定保留为来源记录，新自由 SfM 的尺度重新标为任意单位，不能继续沿用原合成/标定公制声明。

预检只显示这个计划，不先复制图片或运行 SfM。实际新版本仍须经过原有 COLMAP 质量门槛。新建版本与完整状态续训不是同一操作。

### 恢复

完整状态续训包含高斯、优化器、增密累积、随机状态、采样状态以及相机/尺度合同。必须验证数据身份、split、实际分辨率、策略、尺度和变换链；缺少原 prepared 数据时不能重新猜一个归一化继续旧状态。

完整状态续训禁止同时覆盖结果、重跑 COLMAP 或混用没有相机尺度身份的旧 checkpoint。PLY 再初始化、PLY warm start、只恢复预览均不是完整优化器状态恢复。

## 训练坐标和范围

源坐标、规范坐标、训练坐标分开保存。明确的 m/cm/mm 来源可先转为规范米；未知尺度仍保持任意单位。训练使用基于训练相机覆盖和合法初始化范围的可逆平移/正均匀缩放；不依赖试件尺寸、固定边长或照片数量。

随机初始化和数值归一化只使用 train 相机/声明范围/合法初始化，不使用 val/test 的几何范围，不读取评价网格、深度或法向。

现有 Graphdeco / surfel GPU 核有固定正深度裁剪 `view_z > 0.2`。P0 将相机、初始化、学习率/增密范围移到一致训练框架，并记录逐视图初始化覆盖及实际裁剪合同；这不是已经将 CUDA 内核改成任意近裁剪值。若几何落在该近端阈值内仍会被剔除。不能只修改 Python 的 `znear` 标签便宣称内核能力已改变。

## 输出、测量与单位

原始训练框架 PLY 保留。相邻的同名 `.camera-scale.json` 包含完整合同，并用 `ply_sha256` 绑定实际 PLY；不从父目录名或模型外观推断尺度。侧车损坏、文件哈希不符或单位/链矛盾时原生加载失败，不静默改成“米”。

对于规范坐标为米、`p_train = s * R * p_canonical + t`：

```text
每训练坐标单位米数 = 1 / s
源坐标恢复后的每坐标单位米数 = meters_per_unit
规范米制恢复后的每坐标单位米数 = 1
```

原生测量、尺寸显示、坐标报告、JSON 保存/重开和多模型单位比例共用这个物理比率，不把任意训练单位假标成 `1 m`。几何缓存不能覆盖本次哈希验证的尺度来源。没有尺度依据时显示 `u`，不能导出成隐含米制 GLB。

| 变换 | P0 能力 |
|---|---|
| 正均匀缩放 + 平移 | 新文件恢复 XYZ、高斯 log-scale / 协方差等对应量，保留不透明度、方向、SH |
| 3DGS / 2DGS | 分别处理三维高斯与二维 surfel 尺度维度，不把三尺度数组强套给 surfel |
| 世界旋转且只有 DC 外观 | 需同时正确变换高斯方向/协方差，按实际能力测试 |
| 世界旋转且有方向相关 SH | 尚未验证完整 SH 旋转时拒绝烘焙；保留训练框架和变换清单 |
| 非均匀缩放、剪切、镜像、CRS 重投影 | 不属于本接口的正尺度相似变换，不冒充已支持的恢复 |

恢复结果另存 PLY 和 `gsw.coordinate_export/1` 元数据，明确 `output_frame`、输出单位、已烘焙变换、输入/输出哈希。重新打开这种文件不能再次应用原 inverse。保留训练框架的复制/裁剪也要更新新 PLY 的哈希和操作记录。

模型几何的绝对精度还需独立参考。完美合成投影误差、坐标往返误差和尺度链一致性，都不等于实物测量精度。

## 后端对照

| 管线 | 已知相机训练接入 | 尺度与测量边界 |
|---|---|---|
| 3DGS | 共享规范数据/锁定/实际 K/分辨率/split/恢复路线已实现；最终核验以真实 GPU 记录为准 | 实际生成 PLY 与相机尺度合同需成对验证 |
| 2DGS | 同等共享路线，使用 surfel 后端实现与其完整状态恢复 | 二维尺度、深度/法向投影实现需独立验证，不能用 3DGS 单例替代 |
| 有界 / 无界 TSDF | **规范相机训练产物不准进入当前外部 TSDF 网格管线** | 现有外部 `Scene` / 深度读取器没有消费完整规范 K、训练框架与来源框架变换及 split；已安装运行库或 2DGS 训练成功不能替代该能力。普通 legacy 2DGS 网格流程及原 SfM 质量保护保留 |
| OpenMVS 贴图 | **规范相机训练产物不准借旧相机转换路线烘焙贴图** | 旧贴图路径未建立规范逐图相机与模型坐标映射；规范产物在上游网格启动前被拒绝，不会先生成几何再借偶然存在的 sparse 补相机。普通受支持的 legacy 照片贴图流程保留 |
| SuGaR | **规范相机训练产物不准进入当前外部细化 / 网格 / 贴图管线** | 外部细化读取器未接规范完整 K、尺度变换和划分；基础 3DGS 的验收不为它背书。旧版受支持的 SuGaR 原生作业仍保留原门槛 |
| GS2Mesh | **规范相机训练产物不准进入当前外部立体深度 / 融合管线** | 外部深度与融合读取器未核验规范相机、深度 / 法向约定、来源尺度及 split；不能以可读 PLY 代表已适配。旧版受支持的 GS2Mesh 原生作业保留 |

该拒绝在原生网格入口和 `native.worker.mesh_generation.run` 两处生效。后台作业只检查所选模型目录的 `training-camera-contract.json` 和所选迭代 PLY 旁的 `.camera-scale.json` 两个 GSW 保留标记；标记损坏或不可读也停止，不能把损坏元数据当成使用旧读取器的许可。后台返回机器错误码 `camera_dataset_mesh_backend_unavailable`，保存失败记录，且在任何外部环境准备、`Scene` 读取、稀疏质量读取、网格或贴图命令前拒绝。不会重跑 SfM、修复 sparse 或删除已有模型 / 已完成结果。

即便某个高斯 PLY 已恢复为来源 / 米制坐标，也没有因此补齐这些后端的相机、深度、法向或划分适配；只要所选模型根仍带训练合同或所选 PLY 带来源侧车，仍明确拒绝。不会扫描其他迭代，普通 `.prj` 不会触发该规范相机限制。对 legacy 输入原有流程不作全局禁用；退化 SfM 仍按原有质量门槛拒绝。

后续解除能力限制必须同时实现并验收：逐图完整 K 和投影、深度 / 法向坐标约定、训练到来源尺度链、train / val / test 隔离，以及真实生成网格和贴图的跨后端验证。单独安装依赖、恢复 PLY 的 XYZ 或看到预览，均不能解除此门槛。`generation_capabilities.py` 中 3DGS / 2DGS 使用 `explicit_pose_policy`（`solve → shared_colmap_viability`、`locked → canonical_locked_v1`）；下游管线明确为 `canonical_camera_input=false` 并保留技术原因及待验收项。

## 验收证据与未完成门槛

本轮建立的公开测试 seam：

- CPU 协议读取、源像素/K、投影、Sim(3) 往返、大原点、单位冲突、嵌套重名、split 泄漏、初始化和复制身份。
- 真实 owned worker CLI 导入、控制管道保持打开仍能退出、取消/失败回滚及源文件哈希不变。
- 训练 job 准入和外部执行 seam：锁定不启动 COLMAP；求解保留质量保护；不兼容完整恢复组合在排队前拒绝。
- 原生 GUILESS 核心：真实 PLY 加载、物理长度、JSON/坐标报告重开、缓存尺度刷新、未知尺度拒绝隐含米制 GLB、真实 QProcess Python 坐标恢复与 log-scale 验证。
- 独立 GPU 短程训练和真实窗口测试仍是必要门槛，不因以上 CPU 通过而省略。

可重复执行的测试模块：

```powershell
python -B -m unittest native.backend.tests.test_camera_dataset `
  native.worker.test_camera_import native.worker.test_camera_admission `
  native.backend.tests.test_camera_training_review native.worker.test_canonical_training
```

真实增密与完整状态恢复的独立短程工具为 `scripts/verify_camera_density_resume.py`。默认执行 3DGS / 2DGS × clone / split 四组生成夹具；`--plan-only` 只准备 CPU 数据，不能算 GPU 验收。实际 GPU 模式使用有界短日程触发增密，并通过只读 Python profile 记录真实统计、增密与恢复调用，核对模型、Adam、统计量、随机状态、相机与待采样栈。测试参数只属于独立验收命令，不改变产品默认训练日程；该结果不宣称重建几何质量或默认长程训练已被全面验证。

原生核心目标为 `gsw_model_export_tests`。选择真实后端环境的 `GSW_CAMERA_SCALE_EXPORT_PYTHON` 和仓库根 `GSW_CAMERA_SCALE_EXPORT_ROOT` 后，`recordedCameraCoordinatesUseRealNativeExportProcess` 执行真实 CPU 子进程，而不是用字符串检查代替导出。

阶段性证据（待最终匹配提交与安装版重新核对，不能当作最终交付全绿）：

- [本轮计划与验收门槛](../.planning/2026-10-19-camera-scale-p0/task_plan.md)。
- 原生核心日志：`D:/Codex/qa-camera-p0/native-model-export-final.txt`，当时 31 通过、0 失败、1 个既存可选真实 SPZ 输入用例未选择。
- 直接 GPU 多尺度/原点矩阵：`D:/Apps/GSW/validation/camera-scale-p0-oct19/gpu-matrix-2/RESULTS.json`。其源码身份和后续原生 job / 安装候选身份必须在最终报告对齐；开发中修改后的旧结果不能替代重新验证。
- 冻结训练实现的最终 GPU 来源 / 尺度矩阵为 `gpu-final-2/RESULTS.json`（14 组）；真实原生 worker、手动分辨率与原生测量导出分别见 `worker-final-2`、`manual-worker-final` 和 `native-final-3dgs.txt` / `native-final-2dgs.txt`。路径均在本轮 D 盘验收根目录。实际增密补验为 `density-final-1/RESULTS.json`：四组均记录 3 次真实增密、两次完整状态暂停 / 恢复和 train-only 增密统计；补足此前关闭增密的短程运行范围，不替代最终安装版和真实窗口门槛。
- 匹配旧版真实窗口像素问题：`baseline-native-windows-failures.log` 与同提交截图。点云例的原 opaque 颜色检测漏掉 MSAA/网格背景混合后的真实边缘像素，产生 172/181 的错误宽比；修复只校正点覆盖检测，原 ROI、像素数、4% 物理宽比和状态保留门槛不变。修复后的真实窗口结果仍须重新验收。

最终交付报告还必须填写：完整 3DGS / 2DGS 原生 worker GPU 日志、实际采样/检查点/输出与单位验证、三语言真实窗口与旧流程回归、匹配提交 CI、生产 Setup 的哈希/对应源码及安装后快捷方式目标。未完成前不得写成“P0 已全部验收”或“安装版已完成”。
