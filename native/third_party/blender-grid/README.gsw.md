# Blender viewport-grid shader adaptation

Pinned upstream: **Blender v4.5.0**, commit
`8cb6b388974a817afedf1317ce26f0c75aa5f181`. The official GitHub tag API was
checked: this tag resolves directly to that commit, not an annotated tag object.

This is actual GPL source adaptation: the triangular-wave line filter,
disc-prefiltered `LINE_STEP`, eight-step selection, cubic three-level blending,
quartic viewing-angle fade and perspective-distance fade originate in the
retained Blender source. It is **not** a
copy of Blender's whole viewport, compositor, model renderer or UI. The original
GPL-2.0-or-later notices are retained; GSW distributes its combined native
application under GPL-3.0-or-later by choosing the compatible later GPL version.
See the repository and installed package `COPYING` for the full GPLv3 text.

## Retained upstream reference source

These files are byte-for-byte upstream source downloaded from the fixed commit;
they are reference material, **not** compiled with Blender's build system.

| File | Original path at the pinned commit | Upstream SHA-256 |
|---|---|---|
| `upstream/overlay_grid_frag.glsl` | [`source/blender/draw/engines/overlay/shaders/overlay_grid_frag.glsl`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/draw/engines/overlay/shaders/overlay_grid_frag.glsl) | `eeb0b2c7bc10e593ae3ed9979fda1f629bd210b8321afd2ff0d00973f6471589` |
| `upstream/overlay_grid_vert.glsl` | [`source/blender/draw/engines/overlay/shaders/overlay_grid_vert.glsl`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/draw/engines/overlay/shaders/overlay_grid_vert.glsl) | `437f98b489f2316976c2e8e88539e78da2d3e27a8fe6fc380967441f9f204c10` |
| `upstream/overlay_grid.hh` | [`source/blender/draw/engines/overlay/overlay_grid.hh`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/draw/engines/overlay/overlay_grid.hh) | `8e2cace82e0a8efa260fa4cf60f3a6f81ed5705811bbf8e6a0dfe64c2088f050` |
| `upstream/overlay_shader_shared.hh` | [`source/blender/draw/engines/overlay/overlay_shader_shared.hh`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/draw/engines/overlay/overlay_shader_shared.hh) | `c3959d0d5c742e916cfeae9c679d7ab1e6f65adf5986399133a041c094bc90f6` |

The fragment source supplies the line/LOD/fade algorithm. The shared header
supplies `DISC_RADIUS` and `LINE_STEP` (lines 466-471). The host source supplies
the default eight decimal scales, pixel-width policy and plane/projection
choices. The vertex source documents the upstream precision strategy that the
camera-relative fullscreen adapter replaces. No additional Blender dependencies
or engine build are required.

### Controlled step hierarchy source, 2026-10-27

`BlenderGridSteps.cpp` additionally adapts the unitless subdivision loop from
[`view3d_grid_steps_ex` in `source/blender/editors/space_view3d/view3d_draw.cc`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/editors/space_view3d/view3d_draw.cc#L796-L844)
at the same pinned commit. SHA-256 of the complete upstream file is
`6a6d5edb9021116cab87b6a9e2299c39b0cf4b950c3ccf50e78eb854b6fd4b48`.
The source's 2008 Blender Authors and GPL-2.0-or-later notice, plus GSW's 2026
adaptation notice, are preserved in the new helper. This is a direct source
adaptation, not an integration of Blender scene objects or its unit engine.

The original loop emits `grid_scale * subdiv` and multiplies `subdiv` by the
requested subdivision count for each of eight levels. GSW accepts an explicit
already-converted base spacing, uses a double multiplier rather than an integer
that can overflow, omits Blender's axis-view `N^3` predivision, and rejects the
complete result on invalid or unrepresentable values. It does not choose a
physical unit, guess object scale, clamp spacing or change source coordinates.

## GSW changes, 2026-10-19

- Replace Blender GPU create-info macros, UBOs and library aliases with explicit
  GLSL 1.30 uniforms and Qt `QByteArray` shader entry points.
- Replace finite camera-projected grid triangles with a single fullscreen
  triangle and analytic camera-relative ray/plane intersection. View rotation
  and inverse projection are kept separate so a large world translation never
  enters homogeneous unprojection.
- Preserve Blender's periodic triangular-wave function, splitting its modulo
  into camera-relative position plus a CPU-computed per-scale phase. This avoids
  adding a large absolute camera coordinate before a small periodic remainder.
- Use the upstream-documented more accurate maximum of both projected
  derivatives for LOD. The same plane-relative grazing-angle fade applies to
  XY/XZ/YZ planes, including rolled orthographic views.
- Keep true source-origin axes separate from periodic grid phase. Adapt source
  axis colors through uniforms, without changing GSW's coordinate conventions.
- Use the current projection for ray construction, but deliberately do not apply
  model-dependent near/far clipping or Blender's orthographic clip-depth fade.
  GSW's far range is fitted to displayed models, so that would make the fixed
  reference plane disappear during orthographic panning or selection changes.
  Orthographic rays start on the eye's XY plane; only forward intersections are
  visible. Perspective horizon/distance fade remains continuous. The shader
  draws only its grid/axes coverage; it does not set, sample, replace or tint the
  background.
- Omit Blender's post-scene depth-texture/in-front soft-occlusion path. GSW draws
  this grid before its scene, with depth writes disabled; subsequently drawn
  opaque model pixels cover it through the existing model renderer. This does
  not claim Blender's separate overlay/in-front engine was embedded.

## Public integration contract

`gsw::blender_grid::vertexShader()` and `fragmentShader()` return shader source.
There are no UI strings, scene mutations, worker calls, camera controls, global
programs or mutable module state. `StepCount` is eight.

`BlenderGridSteps.h` is standard C++ only and owns the same public `StepCount`.
The shader header includes it, so existing shader callers retain that API.
`subdivisionSteps(double baseSpacing, int subdivisions)` returns
`std::optional<std::array<float, StepCount>>`. It accepts finite positive bases
and integer subdivisions of at least one. Every output must fit the positive
**normal** float range `[std::numeric_limits<float>::min(),
std::numeric_limits<float>::max()]`; smaller subnormals are rejected because
they can be flushed to zero by a GPU. N > 1 produces strictly increasing
levels; N == 1 repeats the base for fixed-mode major-only display. Failure is
`std::nullopt`, never a silently clamped or rescaled grid. The caller owns unit
conversion, settings validation, automatic/manual mode and error presentation.

Use an empty VAO, draw `GL_TRIANGLES` with three vertices, no vertex buffer. The
OpenGL context must support GLSL 1.30 (GSW's OpenGL 3.3 context does). Supply the
actual camera matrices for the same frame as the model renderer.

| Uniform | Type | Meaning |
|---|---|---|
| `inverseProjection` | `mat4` | Inverse of the actual projection including lens/aspect correction; **not** inverse combined view-projection. Its model-dependent clip planes do not clip the analytic reference plane. |
| `inverseViewRotation` | `mat3` | View direction to world direction; upper-left 3 by 3 of inverse view, translation removed. Must be orthonormal, with no scale/shear. |
| `planeBasis` | `mat3` | Columns are orthonormal world vectors **U, V, N**, where N is the plane normal. Either handedness is valid; transpose is its inverse. |
| `cameraPlanePosition` | `vec3` | `(dot(camera-origin,U), dot(camera-origin,V), dot(camera-origin,N))`; `origin` is the persistent scene/reference grid origin, never the selected model's bottom. |
| `gridSteps[8]` | `float` array | Spacings in the same local world units as the camera/reference. Auto mode uses positive ascending powers of ten spanning the visible resolution; do not feed the retired snapped 1-2-5 major-step sequence. Fixed mode uses index 0 as the exact minor spacing and index 1 as the exact major spacing, with all phases generated for those exact values. N == 1 has equal index 0/1 and draws only the major grid. |
| `gridPhase[8]` | `vec2` array | For each step: CPU double `std::remainder(cameraU, step)` and `std::remainder(cameraV, step)`, converted to float only after reduction. Same U/V/origin as `cameraPlanePosition`. |
| `gridDistance` | `float` | Positive distance at which perspective distance fade begins; fade reaches zero at twice this distance. Blender uses clip-end divided by two; GSW supplies a reference/camera-derived range independent of selected-model clipping. Orthographic views do not use this distance fade. |
| `gridLineSize` | `float` | Blender's additional half-width `max(physicalPixelScale - 1, 0) / 2`; zero gives its nominal one-physical-pixel line. |
| `fixedGrid` | `int` | Defaults to 0: auto LOD. Nonzero: fixed minor/major levels only, with no substitution of a coarser spacing. The host must set this explicitly. |
| `gridDensity` | `float` | Defaults to 1. The host must set a finite positive value. Auto LOD divides its original resolution by this value, so larger values admit denser lines without changing their world coordinates. Fixed mode ignores density and uses a physical-pixel fade guard instead. |
| `gridSubdivisions` | `int` | Defaults to 10; the host must set the admitted count. Fixed mode with 1 is major-only; greater counts use indices 0 and 1 as minor/major. This uniform never changes the supplied spacing or model scale. |
| `isOrthographic` | `int` | Zero for perspective, nonzero for orthographic. |
| `showGrid`, `showAxes` | `int` | Zero disables that contribution, nonzero enables it. |
| `gridColor`, `gridEmphasisColor` | `vec4` | Straight RGB and coverage opacity, supplied by GSW's current appearance settings/theme. No hard-coded background. |
| `axisUColor`, `axisVColor` | `vec4` | Straight colors of the source axes along U and V, including their maximum coverage opacity. |

For a point `P` on the plane, its plane coordinates are
`transpose(planeBasis) * (P - origin)`. The ray intersection internally uses
`transpose(planeBasis) * (P - camera)`, adding only the reduced phase for periodic
lines. `cameraPlanePosition.xy` is used only by the nonperiodic true-origin
axes. Phase reduction cannot recover precision already discarded by the
caller's stored camera/reference data; it prevents an additional large-coordinate
periodic cancellation in the shader, not a change to the source geometry format.

Draw before all scene/model passes with `GL_DEPTH_TEST` disabled and depth writes
disabled. Blend straight RGB using source alpha and one-minus-source-alpha.
Keep destination backdrop alpha opaque: use separate alpha factors `GL_ONE` and
`GL_ONE_MINUS_SRC_ALPHA`, or explicitly mask alpha writes while drawing the
grid. Do **not** use source alpha for both alpha factors, which would make Qt's
premultiplied framebuffer presentation corrupt light-background colors. Restore
all GL state expected by subsequent model passes. No shader writes depth.

In fixed mode, both line families reuse Blender's `get_grid`/`LINE_STEP`
coverage. A level fades from absent to fully visible between two and three
physical pixels in its denser projected direction. Zooming never replaces the
configured spacing with another value: an over-dense fixed grid can fade out.
Minor and major covered alpha are combined with `max`, with major colour taking
priority at its coverage, rather than summing coincident opacity. Axes, reference
origin, camera-relative intersection, per-level double-reduced host phase,
straight-alpha blending, background and perspective/angle fades are unchanged.

The module is validated through the integrated rendered-viewport seam, not
private shader-string/LOD implementation tests. Both projections, all reference
planes, roll, close/far scales, custom backgrounds, transparent framebuffer
alpha, model occlusion and navigation/editing remain caller acceptance gates.

中文：实际移植 Blender 的网格线过滤、多级渐变与远处淡出；保留本软件的背景配置、
坐标、导航和模型渲染，不宣称复制了整个 Blender 引擎。
新增受控步长沿用固定版本 Blender 的无单位细分循环；只接收调用者已换算的显示
步长，不声明物理单位或缩放模型。固定模式在过密时淡出，不擅自改成更粗的间距。

日本語：Blender のグリッド線フィルター、多段階ブレンド、遠方フェードを実際に
移植しています。背景設定・座標・操作・モデル描画は本ソフトのものを保持し、
Blender エンジン全体をコピーしたものではありません。
制御可能な間隔の生成には固定版 Blender の単位なし分割ループを使用します。
呼び出し側で換算済みの表示間隔だけを受け取り、物理単位の宣言やモデルの拡縮は
行いません。固定モードでは密すぎる線をフェードし、別の間隔へ変更しません。
