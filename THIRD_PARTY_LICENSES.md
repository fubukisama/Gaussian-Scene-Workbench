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
