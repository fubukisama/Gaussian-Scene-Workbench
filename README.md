<div lang="en">

<p align="center">
  <img src="native/assets/gsw-app-icon.png" width="88" alt="Gaussian Scene Workbench icon">
</p>

<h1 align="center" lang="en">Gaussian Scene Workbench</h1>

<p align="center">
  <strong>Reconstruct, train and edit Gaussian scenes in one native workspace.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/native_preview-0.3.8-2563eb?style=flat-square" alt="Native preview 0.3.8">
  <img src="https://img.shields.io/badge/platform-Windows_x64-334155?style=flat-square" alt="Windows x64">
  <a href="native/LICENSE.md"><img src="https://img.shields.io/badge/desktop_license-GPL--3.0--or--later-16a34a?style=flat-square" alt="Original native desktop: GPL-3.0-or-later"></a>
</p>

<p align="center">
  <strong lang="en">English</strong> · <a href="README.zh-CN.md" lang="zh-CN">简体中文</a> · <a href="README.ja.md" lang="ja">日本語</a>
</p>

<p align="center">
  <a href="README_RELEASE.md"><strong>Get the Windows preview</strong></a> ·
  <a href="docs/README.md">Documentation</a> ·
  <a href="native/README.md">Build from source</a> ·
  <a href="https://github.com/fubukisama/Gaussian-Scene-Workbench/issues">Report a bug</a>
</p>

---

GSW brings **Gaussian Splatting, photogrammetry, point clouds and meshes** into a Qt / C++ desktop application. Start with photos, video or known cameras—or open a model you already have.

## One workspace, six essentials

| Build and train | Inspect and deliver |
| --- | --- |
| **Reconstruct**<br>Import photos or video, link datasets and run COLMAP. | **Explore**<br>View Gaussians, 2DGS surfels, point clouds and textured meshes. |
| **Train**<br>Run 3DGS / 2DGS experiments with progress, safe pause and compatible full-state resume. | **Edit**<br>Select, transform and crop supported models, with undo and original-file protection. |
| **Keep coordinates traceable**<br>Preserve locked cameras, units, scale constraints and data splits. | **Extract and evaluate**<br>Export PLY / SPZ, use optional mesh routes and compare geometry against independent references. |

## Start in three steps

1. **Install the preview.** Follow the [download guide](README_RELEASE.md) for the available installer and its prerequisites.
2. **Bring in a scene.** Add photos / video, link a dataset, or import an existing PLY / SPZ model.
3. **Choose your workflow.** Train, inspect, edit or export. [Find the right guide →](docs/README.md)

> **Preview, not a bundled training environment.** Reconstruction and CUDA training need compatible external runtimes. Full-state resume is limited to compatible native 3DGS / 2DGS checkpoints. Rendered depth products are appearance proxies, not certified physical surfaces or accuracy measurements.

<details>
<summary><strong>See the native viewport</strong></summary>

![Native viewport displaying four independently loaded synthetic layers](docs/images/native-viewport-fixture.png)

Actual native viewport capture from an independent synthetic regression scene. It illustrates mixed-model display, not reconstruction quality. [Image provenance](docs/images/README.md).

</details>

## Go deeper

[Training & resume](docs/TRAINING_RESUME.md) · [Known cameras & scale](docs/CAMERA_SCALE_P0.md) · [Geometry & evaluation](docs/GENERAL_RECONSTRUCTION.md) · [Supported pipelines](docs/GENERATION_PIPELINES.md) · [Display & navigation](docs/VIEWPORT_DISPLAY.md)

## License

The original native desktop is **GPL-3.0-or-later**; other original code and third-party components retain their own licenses. Graphdeco research backends have separate non-commercial restrictions—GPL does not grant commercial rights to every backend.

[License guide](docs/LICENSING.md#english) · [Native scope](native/LICENSE.md) · [Repository license](LICENSE) · [Third-party notices](THIRD_PARTY_LICENSES.md)

</div>
