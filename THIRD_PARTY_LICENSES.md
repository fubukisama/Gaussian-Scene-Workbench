# Third-Party Licenses

Gaussian Scene Workbench is distributed for open-source research, learning, and evaluation use. The repository includes third-party components with their own licenses. The root MIT terms apply only to original Gaussian Scene Workbench code and do not override these third-party licenses.

## SIBR FPSCounter Design Reference

The render-performance meter is a JavaScript adaptation of the rolling frame-time design used by SIBR's `FPSCounter` (Inria GRAPHDECO). No SIBR binaries are bundled. The reference implementation is distributed for non-commercial research and evaluation use: https://gitlab.inria.fr/sibr/sibr_core/-/blob/gaussian_code_release_union/src/core/view/FPSCounter.cpp

## Native Qt 6 Preview

The native executable also embeds Qt base Chinese and Japanese translation catalogs for standard Qt controls. Their upstream sources and license notices are available in the Qt translations module: https://code.qt.io/cgit/qt/qttranslations.git/ . Workbench-specific Chinese, English and Japanese messages are original project translations; CloudCompare, Metashape and Blender are terminology references only.

The `native/` application dynamically links Qt 6.8 Core, Gui, Widgets, OpenGL, OpenGLWidgets, Network, and SVG modules. Open-source Qt modules are available under the GNU Lesser General Public License v3 and/or the alternative licenses stated by each Qt module. The application does not statically link Qt, and deployed Qt DLLs can be replaced by the user.

- Qt licensing: https://www.qt.io/licensing/open-source-lgpl-obligations
- Qt source archives: https://download.qt.io/archive/qt/6.8/6.8.3/submodules/
- GNU LGPL v3: https://www.gnu.org/licenses/lgpl-3.0.html

Local packages built from the Conda Qt environment can also include dynamically linked runtime libraries such as zlib, zstd, PCRE2, double-conversion, libpng, libjpeg-turbo, libtiff, FreeType, WebP, libdeflate, Lerc, and liblzma. Those libraries retain their respective upstream open-source licenses. GitHub Actions builds use the official Qt Windows distribution.

## Native SPZ Codec / 原生 SPZ 编解码 / ネイティブ SPZ コーデック

The native desktop directly compiles the MIT-licensed Niantic/Adobe SPZ source at commit `affd0ecea7fbb4c265ee119475af7ee5b2997482`: https://github.com/nianticlabs/spz . Copyright notices, the upstream license and the exact vendored source subset are retained in `native/third_party/spz/` and distributed in desktop packages under `licenses/spz/`. This is actual codec reuse, not a reimplementation. Its zlib and Zstandard dependencies retain their existing licenses (https://zlib.net/zlib_license.html and https://github.com/facebook/zstd/blob/dev/LICENSE); these notices are also included under `licenses/spz/dependency-licenses/`.

中文：桌面端直接编译复用 Niantic/Adobe 的 MIT 开源 SPZ 编解码源码，固定上述提交；保留源码、版权及许可，安装包内位于 `licenses/spz/`。zlib 与 Zstandard 依赖保留各自许可。

日本語：デスクトップ版は Niantic/Adobe の MIT ライセンス SPZ ソースを上記コミットに固定して直接コンパイルします。ソース・著作権・ライセンスは配布物の `licenses/spz/` に同梱します。zlib と Zstandard はそれぞれのライセンスを維持します。

## Native SH Evaluation / 原生球谐求值 / ネイティブ SH 評価

`native/third_party/gsplat-sh/evaluate_sh.glsl` directly adapts `sh_coeffs_to_color_fast` from gsplat commit `512d366b67073d77ca099ede742683c165dfc23b` under Apache-2.0: https://github.com/nerfstudio-project/gsplat . The Regents of the University of California, Nerfstudio Team/contributors and NVIDIA notices, modification notice and full license are retained and shipped under `licenses/gsplat-sh/`. Only the evaluator is adapted; no CUDA/PyTorch runtime is added. No upstream NOTICE file is present at that revision.

中文：GLSL 球谐求值直接改编 gsplat 的 Apache-2.0 源码，固定上述提交。完整保留版权、修改声明及许可证，并随桌面安装包分发；不增加 CUDA/PyTorch 运行时依赖。

日本語：GLSL の SH 評価は gsplat の Apache-2.0 ソースを上記コミットに固定して直接移植しています。著作権・変更通知・ライセンスを保持し配布物に同梱します。CUDA/PyTorch ランタイム依存は追加しません。

## LichtFeld Studio Reference

LichtFeld Studio is studied as an architecture and workflow reference under GPL-3.0-or-later. No LichtFeld implementation source is copied or linked into the current MIT-licensed native preview. Direct future reuse requires a compatible GPL release and an explicit licensing change: https://github.com/MrNeRF/LichtFeld-Studio

## Key License Constraints

- `gaussian-splatting/`: Gaussian-Splatting License. This license limits use to non-commercial research and evaluation purposes unless separate permission is obtained from the original licensors.
- `gaussian-splatting/submodules/diff-gaussian-rasterization/`: Gaussian-Splatting License.
- `gaussian-splatting/submodules/simple-knn/`: Gaussian-Splatting License.
- `gaussian-splatting/SIBR_viewers/`: Apache License 2.0, except for subdirectories that state a different license.
- `gaussian-splatting/submodules/fused-ssim/`: MIT License.

## Practical Meaning

You may use this repository for research, learning, experiments, and evaluation, subject to the included licenses.

Do not assume the full repository is available for commercial use under MIT. Commercial use of the bundled Gaussian Splatting components requires permission from the original licensors.

## Included License Files

- `LICENSE`
- `gaussian-splatting/LICENSE.md`
- `gaussian-splatting/submodules/diff-gaussian-rasterization/LICENSE.md`
- `gaussian-splatting/submodules/simple-knn/LICENSE.md`
- `gaussian-splatting/SIBR_viewers/LICENSE.md`
- `gaussian-splatting/submodules/fused-ssim/LICENSE`
