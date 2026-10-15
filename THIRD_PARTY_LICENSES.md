# Third-Party Licenses

## Optional native training accelerators

The independent combined L1/SSIM extension in `gaussian-splatting/gsw_loss_cuda/`
is newly derived from Rahul Goel's MIT fused-ssim commit
`1272e21a282342e89537159e4bad508b19b34157`, retaining its original copyright and
MIT license. GSW additions fuse the L1 reduction/gradient and add bounded
workspaces and numerical/source/ABI gates. It is not a relicensing of the
private GPL LichtFeld prototype. See its `README.gsw.md` and complete `LICENSE`.

The optional Faster-GS CUDA package is fetched separately at Apache-2.0 commit
`c60ea269b7cf3ebda527d640b6fdd31462f91b41`. Original GSW adapter/control helpers
outside the GPL desktop main-program scope carry MIT notices. Neither extension
nor the main-program GPL grant overrides the research/evaluation restrictions
on Graphdeco/2DGS trainers. Prebuilt experimental CUDA libraries and private
prototype artifacts are not bundled in native staging.

中文：发布版联合损失重新基于MIT fused-ssim原代码，并保留原版权及许可；不是给
GPL私用原型换许可证。Faster-GS另按Apache-2.0下载。研究训练后端的原有限制不变。

日本語：公開の融合損失はMIT fused-ssim原コードから新たに派生し、元の著作権と
ライセンスを保持します。GPL私用原型のライセンス変更ではありません。
Faster-GSは別途Apache-2.0で取得し、研究用学習バックエンドの制限は維持します。

Gaussian Scene Workbench contains components under several licenses. From the 2026-10-14 licensing change, the original native desktop main program is GPL-3.0-or-later within the scope in [native/LICENSE.md](native/LICENSE.md). Earlier MIT grants and original code outside that scope retain the root MIT terms. Neither grant overrides third-party licenses or grants commercial rights to restricted research components. The project's research purpose is not an additional use restriction on its GPL or MIT code.

## Blender Transform Geometry / Blender 变换几何 / Blender 変換形状

The native desktop directly adapts the cone-generation routine from Blender
v4.5.0 `source/blender/gpu/intern/gpu_immediate_util.cc` and cube vertex/index
data from `source/blender/editors/gizmo_library/geometry/geom_cube_gizmo.cc`.
The retained subset and Qt adapter are in `native/third_party/blender-gizmo/`.
This is actual source/geometry reuse, not merely a visual reference. See that
directory's provenance and change records for the exact upstream files and
their fixed revision. Upstream Blender Authors copyright and
GPL-2.0-or-later notices are preserved; GPL version 3 is selected for the
combined desktop program. The complete GPL version 3 text is in [COPYING](COPYING).

The port supplies cone/cube geometry and presentation through the native Qt
adapter; it does not embed Blender's application, windowing engine, complete
operator system or all transform interaction code. GSW retains its own
axis/plane dragging, numeric input, constraint, undo and project context.
Blender branding and application icons are not included. Official source:
<https://github.com/blender/blender/tree/v4.5.0>.

中文：桌面端实际移植 Blender v4.5.0 的圆锥生成及立方体顶点/索引子集，
保留上游版权、GPL-2.0-or-later 声明、固定来源和修改说明，与主程序按 GPL
第 3 版组合。移植限于几何与原生 Qt 展示适配，不等于嵌入完整 Blender 操作器、
窗口系统或全部交互代码；拖动、约束、数字输入、撤销和工程上下文仍由 GSW 处理。

日本語：Blender v4.5.0 の円錐生成と立方体の頂点・インデックスを実際に移植し、
元の著作権、GPL-2.0-or-later 通知、固定した出典と変更記録を保持します。
主プログラムとの組合せには GPL 第 3 版を適用します。形状と Qt 表示の移植であり、
Blender 全体の操作器・ウィンドウ・全操作コードを組み込むものではありません。
ドラッグ、制約、数値入力、取り消しとプロジェクトの状態は GSW が処理します。

## Native Qt 6 Preview

Native 2DGS training: `native/worker/two_dgs_train.py` adapts the actual official hbb1/2d-gaussian-splatting `train.py`, commit `f3e3b9fa67bbd1c75e05167ff37391d8dab2a678`: https://github.com/hbb1/2d-gaussian-splatting . Original Inria GRAPHDECO notices are retained. The complete upstream Gaussian-Splatting research/evaluation license is shipped at `native/worker/licenses/2dgs-LICENSE.md`. Changes add native state control, telemetry and observation snapshots. `native/worker/training_density_control.py` adapts the final densification/pruning orchestration from that model and the vendored Graphdeco `scene/gaussian_model.py`, under their existing research/evaluation licenses. It adds a robust geometry-aware size limit, reset-recovery grace and bounded final pruning. Upstream clone/split and optimizer surgery remain in their original modules. No commercial-use permission is implied.

中文：2DGS 训练入口和原生密度控制编排基于上述官方源码，保留版权及完整研究/评估用途许可证，不以 MIT 重新授权，不授予商用许可。修改包括原生状态控制、监视、快照集成与过度裁剪保护。

日本語：2DGS の学習入口と原生密度制御は上記公式ソースに基づき、著作権と研究・評価用途のライセンスを保持します。MIT への変更や商用利用許諾はありません。原生の状態制御、監視、スナップショット統合と過剰な枝刈りの抑制を追加しています。

The native executable also embeds Qt base Chinese and Japanese translation catalogs for standard Qt controls. Their upstream sources and license notices are available in the Qt translations module: https://code.qt.io/cgit/qt/qttranslations.git/ . Workbench-specific Chinese, English and Japanese messages are original project translations; CloudCompare, Metashape and Blender are terminology references only.

The native desktop main program dynamically links Qt 6.8 Core, Concurrent, Gui, Widgets, OpenGL, OpenGLWidgets and SVG modules. Open-source Qt modules are available under the GNU Lesser General Public License v3 and/or the alternative licenses stated by each Qt module. The application does not statically link Qt, and deployed Qt DLLs can be replaced by the user. Distributors must preserve the applicable Qt notices and satisfy the source and other obligations of their chosen Qt licenses; the main program's GPL notice does not replace those obligations.

- Qt licensing: https://www.qt.io/licensing/open-source-lgpl-obligations
- Qt source archives: https://download.qt.io/archive/qt/6.8/6.8.3/submodules/
- GNU LGPL v3: https://www.gnu.org/licenses/lgpl-3.0.html

Local packages built from the Conda Qt environment can also include dynamically linked runtime libraries such as zlib, zstd, PCRE2, double-conversion, libpng, libjpeg-turbo, libtiff, FreeType, WebP, libdeflate, Lerc, and liblzma. Those libraries retain their respective upstream open-source licenses. GitHub Actions builds use the official Qt Windows distribution.

## Native Desktop Icons / 原生桌面图标 / ネイティブデスクトップのアイコン

The desktop semantic icons directly use Lucide SVG assets pinned to commit `70562c1ee1c4fdcf736fe97bc893fb8511927934`: https://github.com/lucide-icons/lucide . Original artwork and the full upstream ISC license, including the MIT notice for Feather-derived icons, are retained in `native/third_party/lucide/`; desktop packages ship the notices in `licenses/lucide/`. Runtime palette substitution does not alter the vendored artwork. Blender, CloudCompare and Metashape icon assets and branding are not bundled. The separately licensed Blender geometry port is described above; CloudCompare and Metashape remain behavior references.

中文：桌面语义图标直接复用上述固定版本的 Lucide SVG，保留原始图形、ISC 许可和 Feather 衍生图标的 MIT 声明，随安装包分发。不分发 Blender、CloudCompare、Metashape 的应用图标或品牌标识；单独许可的 Blender 几何移植见上方，后两者仍仅作为行为参考。

日本語：デスクトップの操作アイコンは上記コミットの Lucide SVG を直接使用し、元の図形・ISC ライセンス・Feather 派生図形の MIT 通知を配布物に同梱します。Blender・CloudCompare・Metashape のアプリアイコンとブランド記号は同梱しません。別ライセンスの Blender 形状移植は上記、後者二つは操作の参考のみです。

## Native SPZ Codec / 原生 SPZ 编解码 / ネイティブ SPZ コーデック

The native desktop directly compiles the MIT-licensed Niantic/Adobe SPZ source at commit `affd0ecea7fbb4c265ee119475af7ee5b2997482`: https://github.com/nianticlabs/spz . Copyright notices, the upstream license and the exact vendored source subset are retained in `native/third_party/spz/` and distributed in desktop packages under `licenses/spz/`. This is actual codec reuse, not a reimplementation. Its zlib and Zstandard dependencies retain their existing licenses (https://zlib.net/zlib_license.html and https://github.com/facebook/zstd/blob/dev/LICENSE); these notices are also included under `licenses/spz/dependency-licenses/`.

中文：桌面端直接编译复用 Niantic/Adobe 的 MIT 开源 SPZ 编解码源码，固定上述提交；保留源码、版权及许可，安装包内位于 `licenses/spz/`。zlib 与 Zstandard 依赖保留各自许可。

日本語：デスクトップ版は Niantic/Adobe の MIT ライセンス SPZ ソースを上記コミットに固定して直接コンパイルします。ソース・著作権・ライセンスは配布物の `licenses/spz/` に同梱します。zlib と Zstandard はそれぞれのライセンスを維持します。

## Native 2DGS Surface Projection / 原生 2DGS 表面投影 / ネイティブ 2DGS サーフェル投影

The GPL desktop embeds the original `native/src/shaders/SurfelProjection.glsl`
implementation for native 2DGS projection. Its geometric calculations are
independently implemented; the previous restricted derivative is neither
embedded nor loaded by the GPL desktop. This changes the display-code
licensing boundary, not the research licenses or optimizer-state capabilities
of separate training components.

The former `native/third_party/2dgs-surfel/evaluate_surfel.glsl` is preserved
as separately licensed historical material. It directly adapted routines
from hbb1/diff-surfel-rasterization `cuda_rasterizer/forward.cu`, commit
`e0ed0207b3e0669960cfad70852200a4a5847f61`:
<https://github.com/hbb1/diff-surfel-rasterization>.
Original Inria GRAPHDECO notices and the full research/evaluation license
remain in its directory. It has not been relabeled GPL or granted commercial
rights. It must not be linked or embedded in the GPL main program without
appropriate separate permission from the relevant rights holders.

中文：GPL 桌面端使用独立实现的 SurfelProjection.glsl，不嵌入或加载旧研究许可衍生 shader。旧文件保留原版权及完整许可，不改标为 GPL，不授予商用权，也不在未经适当另行许可时与 GPL 主程序链接或嵌入。独立训练后端的研究许可和完整状态续训能力不受此显示代码替换影响。

日本語：GPL デスクトップは独自実装の SurfelProjection.glsl を使い、従来の研究ライセンス派生 shader を組み込んだり読み込んだりしません。旧ファイルは著作権と全ライセンスを維持し、GPL への変更や商用許諾はありません。権利者の適切な別許諾なしに GPL 主プログラムへリンク・組込みはできません。独立学習バックエンドの研究ライセンスと完全状態再開の機能は変更しません。

## Native SH Evaluation / 原生球谐求值 / ネイティブ SH 評価

`native/third_party/gsplat-sh/evaluate_sh.glsl` directly adapts `sh_coeffs_to_color_fast` from gsplat commit `512d366b67073d77ca099ede742683c165dfc23b` under Apache-2.0: https://github.com/nerfstudio-project/gsplat . The Regents of the University of California, Nerfstudio Team/contributors and NVIDIA notices, modification notice and full license are retained and shipped under `licenses/gsplat-sh/`. Only the evaluator is adapted; no CUDA/PyTorch runtime is added. No upstream NOTICE file is present at that revision.

中文：GLSL 球谐求值直接改编 gsplat 的 Apache-2.0 源码，固定上述提交。完整保留版权、修改声明及许可证，并随桌面安装包分发；不增加 CUDA/PyTorch 运行时依赖。

日本語：GLSL の SH 評価は gsplat の Apache-2.0 ソースを上記コミットに固定して直接移植しています。著作権・変更通知・ライセンスを保持し配布物に同梱します。CUDA/PyTorch ランタイム依存は追加しません。

## LichtFeld Studio Reference

The native desktop now directly adapts LichtFeld Studio's whole-page byte-budget
and at-least-one upload policy from `lod_page_cache.cpp`, pinned to
`a8d2feafbe0abd91211e20f46b7793b8839b2922`, in
`native/third_party/lichtfeld-streaming/`. Original 2026 LichtFeld Studio Authors
copyright and GPL-3.0-or-later notices, original-file hash and modification
record are retained and packaged. The adapter handles variable page sizes,
attempt/time bounds and overflow-safe accounting in GSW's existing point/mesh
queues. No LichtFeld CUDA/Vulkan runtime, NVIDIA library or training engine is
linked by this CPU policy port. Other LichtFeld code remains a reference unless
its own reviewed adaptation is explicitly documented.

The native camera also adapts Blender v4.5.0 cursor zoom and projected pan in
`native/third_party/blender-navigation/`. Pinned version, full source hashes,
2008/2023 Blender notices, GPL-2.0-or-later and Qt modifications are retained
there and packaged; the combined main program selects GPL version 3.

中文：实际移植 LichtFeld 的整页上传预算及 Blender 的光标缩放／投影平移，保留
固定来源、原作者版权、GPL 与修改说明。不链接它们的完整渲染器／训练器，不改变
第三方训练许可。源码与说明随主程序分发。

日本語：LichtFeld のページ転送予算と Blender のカーソルズーム／投影パンを
実際に移植し、固定した出典・元著作権・GPL・変更通知を保持して配布します。
各ソフトのレンダラー／学習器全体をリンクせず、第三者の学習ライセンスを
変更しません。

## Compute Component and Distribution Boundary

The desktop launches compute components as external processes and exchanges
task information, files and supported GPU-preview frames. Their source and
licenses remain separate from the original desktop main program. A process
boundary is not, by itself, a legal determination that every integration or
distribution is an aggregate. Changes to dependencies, data interchange or
packaging require reassessing that boundary. These notices record audited
components and intended boundaries, not a blanket legal guarantee or
commercial authorization for all bundled training code.

中文：桌面与计算组件通过外部进程、任务信息、文件及受支持的 GPU 预览帧协作，
保留独立源码和许可；“不同进程”本身不保证每种集成或分发在法律上属于独立聚合。
依赖、数据交换或包装变化须重新核查，不把 GPL 主程序误解为全部训练代码的商用授权。

日本語：デスクトップと計算コンポーネントは外部プロセス、タスク情報、ファイル、
対応する GPU プレビューフレームで連携し、ソースとライセンスを分けます。
別プロセスという事実だけで、すべての組合せ・配布が法的に独立した集合と
保証されるわけではありません。依存・通信・包装変更は再確認が必要であり、
GPL 主プログラムを全学習コードの商用許諾と解釈しないでください。

## Key License Constraints

- `gaussian-splatting/`: Gaussian-Splatting License. This license limits use to non-commercial research and evaluation purposes unless separate permission is obtained from the original licensors.
- `gaussian-splatting/submodules/diff-gaussian-rasterization/`: Gaussian-Splatting License.
- `gaussian-splatting/submodules/simple-knn/`: Gaussian-Splatting License.
- `gaussian-splatting/SIBR_viewers/`: Apache License 2.0, except for subdirectories that state a different license.
- `gaussian-splatting/submodules/fused-ssim/`: MIT License.

## Practical Meaning

You may use this repository for research, learning, experiments, and evaluation, subject to the included licenses.

Do not assume the full repository or every bundled component is available for commercial use under the main program's GPL or the remaining MIT grants. Commercial use of restricted Gaussian Splatting components requires permission from the original licensors. Binary distributions of the GPL main program must also meet the corresponding-source and notice requirements described in `native/LICENSE.md` and `COPYING`.

## Included License Files

- `LICENSE`
- `COPYING`
- `native/LICENSE.md`
- `native/third_party/blender-gizmo/` provenance and upstream notices
- `gaussian-splatting/LICENSE.md`
- `gaussian-splatting/submodules/diff-gaussian-rasterization/LICENSE.md`
- `gaussian-splatting/submodules/simple-knn/LICENSE.md`
- `gaussian-splatting/SIBR_viewers/LICENSE.md`
- `gaussian-splatting/submodules/fused-ssim/LICENSE`
