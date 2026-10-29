<div lang="zh-CN">

<p align="center">
  <img src="native/assets/gsw-app-icon.png" width="88" alt="Gaussian Scene Workbench 图标">
</p>

<h1 align="center" lang="en">Gaussian Scene Workbench</h1>

<p align="center">
  <strong>从照片到高斯场景：在一个原生工作台里完成重建、训练与编辑。</strong>
</p>

<p align="center">
  <a href="README_RELEASE.md#下载预览版" title="获取原生预览版"><img src="https://img.shields.io/badge/native_preview-0.3.8-2563eb?style=flat-square" alt="原生预览版 0.3.8"></a>
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

**高斯场景研究工作台（GSW）**把高斯泼溅、摄影测量、点云和网格工作流放进 Qt / C++ 原生桌面。可以从照片、视频、已知相机开始，也可以直接打开已有模型。

## 一个工作台，六项核心能力

| 重建与训练 | 查看与交付 |
| --- | --- |
| **重建场景**<br>导入照片或视频、关联数据集，并运行 COLMAP。 | **查看模型**<br>统一显示高斯、2DGS surfel、点云和纹理网格。 |
| **管理训练**<br>运行 3DGS / 2DGS 实验，查看进度，安全暂停并从兼容的完整状态续训。 | **编辑场景**<br>选择、变换和裁剪受支持模型，保留撤销与原始文件。 |
| **追溯坐标**<br>保留锁定相机、单位、尺度约束和训练／检核数据划分。 | **提取与评价**<br>导出 PLY / SPZ、使用可选网格管线，并与独立参考数据比较几何。 |

## 三步开始

1. **安装预览版。** 按[下载指南](README_RELEASE.md#简体中文)获取可用安装器，并确认所需环境。
2. **加入场景。** 添加照片／视频、关联数据集，或导入已有 PLY / SPZ 模型。
3. **选择工作流。** 训练、查看、编辑或导出。[查找对应指南 →](docs/README.md)

> **当前为预览版，不捆绑完整训练环境。** 重建和 CUDA 训练需要兼容的外部运行库。完整状态续训仅适用于兼容的原生 3DGS / 2DGS 检查点；渲染深度成果是外观代理，不代表真实材料边界或模型精度认证。

<details>
<summary><strong>查看原生视口</strong></summary>

![原生视口同时显示四个独立加载的合成图层](docs/images/native-viewport-fixture.png)

来自独立合成回归场景的实际原生视口截图，仅展示混合模型显示，不是重建质量样例。[图片来源](docs/images/README.md)。

</details>

## 进一步了解

[训练与续训](docs/TRAINING_RESUME.md) · [已知相机与尺度](docs/CAMERA_SCALE_P0.md) · [几何与独立评价](docs/GENERAL_RECONSTRUCTION.md) · [支持的管线](docs/GENERATION_PIPELINES.md) · [显示与导航](docs/VIEWPORT_DISPLAY.md)

## 许可

原创原生桌面主程序采用 **GPL-3.0-or-later**；范围外原创代码与第三方组件保留各自许可。Graphdeco 研究后端另有非商业限制，主程序 GPL 不等于所有后端均可商用。

[许可说明](docs/LICENSING.md#简体中文) · [原生许可范围](native/LICENSE.md) · [仓库许可](LICENSE) · [第三方声明](THIRD_PARTY_LICENSES.md)

</div>
