<div lang="zh-CN">

<p align="center">
  <img src="native/assets/gsw-app-icon.png" width="88" alt="Gaussian Splatting Workbench 图标">
</p>

<h1 align="center" lang="en">Gaussian Splatting Workbench</h1>

<p align="center">
  <strong>从照片与已知相机，到可编辑的高斯场景。</strong>
</p>

<p align="center">
  <a href="README_RELEASE.md#下载预览版" title="获取原生预览版"><img src="https://img.shields.io/badge/native_preview-development-2563eb?style=flat-square" alt="原生预览版开发通道"></a>
  <a href="README_RELEASE.md#使用前确认" title="Windows 平台与运行环境要求"><img src="https://img.shields.io/badge/platform-Windows_x64-334155?style=flat-square" alt="Windows x64"></a>
  <a href="native/LICENSE.md"><img src="https://img.shields.io/badge/desktop_license-GPL--3.0--or--later-16a34a?style=flat-square" alt="原创原生主程序：GPL-3.0-or-later"></a>
</p>

<p align="center">
  <a href="README.md" lang="en">English</a> · <strong lang="zh-CN">简体中文</strong> · <a href="README.ja.md" lang="ja">日本語</a>
</p>

<p align="center">
  <a href="README_RELEASE.md#简体中文"><strong>获取 Windows 预览版</strong></a> ·
  <a href="docs/README.md">文档</a> ·
  <a href="native/README.md">源码构建</a> ·
  <a href="https://github.com/fubukisama/Gaussian-Scene-Workbench/issues">反馈问题</a>
</p>

---

<strong><span lang="en">Gaussian Splatting Workbench (GSW)</span></strong> 在 Windows 原生桌面中连接摄影测量、3DGS / 2DGS 训练、多模型编辑与可追溯导出。可以从照片或视频开始，保留外部求解的相机，也可以直接打开已有模型。

[为什么选择 GSW](#为什么选择-gsw) · [选择你的起点](#选择你的起点) · [功能一览](#功能一览) · [开始使用](#开始使用) · [文档](#文档)

## 原生视口

![原生视口显示独立的高斯、网格与点云测试图层](docs/images/native-viewport-fixture.png)

<sub>来自合成混合模型回归场景的实际视口截图（2026-10-27）。这是较早的画面，仅展示图层显示，不代表当前 UI 验收或重建质量。[图片来源](docs/images/README.md)。</sub>

## 为什么选择 GSW

### 把整个实验连接起来

在一个工程中关联数据集、重建、训练任务和成果。查看进度，安全暂停兼容的训练，比较不同实验而不覆盖旧模型。持久批量任务可独立于观察窗口继续运行。

### 相机与尺度有据可查

导入已知相机的完整内参、位姿和数据划分；锁定相机流程不会静默重跑 SfM。保留初始化、坐标变换、尺度约束与独立检核记录；未知单位仍保持未知。

### 保留原件，查看与编辑新结果

在同一视口中处理高斯模型、2DGS surfel、点云和网格。选择对象或受支持的点集，进行可撤销的变换与裁剪，再另存导出结果。视口编辑不会重写源文件或训练检查点。

## 适合谁

- **重建研究者：**一起记录输入、实际生效的设置、相机策略与评价来源。
- **场景编辑者：**查看多个成果，调整受支持模型，准备符合目标格式要求的导出。
- **已知相机工作流：**使用标定、合成或外部重建的相机数据，不把每种输入都重新当作 SfM 问题处理。

GSW 面向通用场景：小物体、室内和建筑是不同的应用流程，不是写死的实验模板。

## 选择你的起点

### 照片或视频

添加媒体或关联数据集，通过 COLMAP 重建相机，再训练 3DGS 或 2DGS。从稀疏重建、初始化到训练预览和最终结果，按阶段查看进度。[导入与重建](docs/WORKSPACE_DROP.md) · [训练与完整状态续训](docs/TRAINING_RESUME.md)

### 已知相机

检查相机约定、单位和训练／验证／测试划分，在训练前选择并查看初始化。通过受支持的导出保留锁定相机与已记录的变换链。[相机与初始化](docs/CAMERA_SCALE_P0.md) · [已知距离、基线与控制点](docs/CAMERA_SCALE_P1.md)

### 已有模型

导入受支持的 PLY / SPZ 文件，比较独立图层，调整可见性和变换，无须重新训练即可导出。[多模型工作流](docs/MULTI_SCENE_IMPORT.md) · [坐标与单位](docs/SCENE_COORDINATES.md)

## 功能一览

| 工作流 | GSW 提供的能力 | 使用前确认 |
| --- | --- | --- |
| 查看与编辑 | 高斯／2DGS、点云和网格图层；选择、变换、裁剪与撤销 | 源属性支持情况，以及各格式的编辑／导出限制 |
| 重建 | 照片／视频导入、数据集关联和 COLMAP 任务 | 兼容的外部 COLMAP 与运行环境 |
| 训练与续训 | 原生 3DGS / 2DGS 任务、进度、安全暂停和兼容的完整状态续训 | 后端与输入身份匹配；PLY 热启动不等于完整状态续训 |
| 几何提取 | 渲染深度点云，以及可选的 TSDF、SuGaR / GS2Mesh 路线 | 各方法的相机、深度、运行环境和尺度要求；并非所有路线都支持所有输入 |
| 测量与比较 | 配对图像的 PSNR / SSIM 与独立几何报告，含后台／CLI 工具 | 有效参考数据、配准、单位、数据划分与可比较的测量设置 |

高斯模型可通过 PLY / SPZ 交付。点云与网格导出有各自的格式和属性规则；支持一种格式，不等于每种模型表示都具有相同支持。[完整管线矩阵](docs/GENERATION_PIPELINES.md) · [几何路线与限制](docs/MESH_GENERATION.md) · [高斯导出保真](docs/GAUSSIAN_PLY_BAKING.md)

## 开始使用

1. **获取已验证的 Windows x64 预览版。** 按[下载与安装指南](README_RELEASE.md#简体中文)操作，或[从源码构建原生程序](native/README.md)。源码 ZIP 不是安装器。
2. **选择查看或计算。** 打开受支持的已有模型不需要训练环境；重建与训练使用单独配置的运行环境。CUDA 训练需要兼容的 NVIDIA 硬件和后端依赖。[运行环境配置](docs/WINDOWS_NATIVE_RUNTIME.md)
3. **先从小工程开始。** 导入模型、添加媒体，或验证已知相机数据集。长时间运行前，确认实际生效的设置与输出位置。

原生界面支持**简体中文、英语和日语**，切换语言即时生效。内存与显存需求取决于图像分辨率、模型大小和所选方法，不提供统一的显存或质量保证。

> **预览版边界：**兼容的完整状态续训仅适用于原生 3DGS / 2DGS 后端。渲染深度几何是外观代理，不是经过认证的材料边界。任务完成、较低的拟合残差或正常预览，都不能证明独立几何精度。

## 文档

- **训练与实验：**[暂停／续训](docs/TRAINING_RESUME.md) · [运行控制](docs/RUNTIME_DECOUPLING.md) · [质量测量](docs/TRAINING_QUALITY.md) · [持久批量任务](docs/RECORDED_PLY_AND_BATCH.md)
- **相机与几何：**[已知相机](docs/CAMERA_SCALE_P0.md) · [尺度约束](docs/CAMERA_SCALE_P1.md) · [初始化、深度与评价](docs/GENERAL_RECONSTRUCTION.md)
- **桌面与交付：**[显示／导航](docs/VIEWPORT_DISPLAY.md) · [大模型预算](docs/RESOURCE_BUDGETS.md) · [更新／回退](docs/NATIVE_UPDATES.md) · [源码构建](native/README.md)

[浏览全部指南 →](docs/README.md)

## 反馈与开发

反馈可复现的问题时，请附上版本、相关设置，以及尽可能小的非私密样例。不要在公开 Issue 中上传私人照片、检查点或凭据。[问题追踪](https://github.com/fubukisama/Gaussian-Scene-Workbench/issues) · [原生 CI 与诊断](docs/NATIVE_CI.md)

## 许可

原创原生桌面主程序采用 **GPL-3.0-or-later**；范围外原创代码与第三方组件保留各自许可。Graphdeco 研究后端另有非商业限制，主程序 GPL 不等于所有后端均可商用。

[许可说明](docs/LICENSING.md#简体中文) · [原生许可范围](native/LICENSE.md) · [仓库许可](LICENSE) · [第三方声明](THIRD_PARTY_LICENSES.md)

</div>
