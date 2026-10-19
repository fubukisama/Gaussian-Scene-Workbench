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

Use an empty VAO, draw `GL_TRIANGLES` with three vertices, no vertex buffer. The
OpenGL context must support GLSL 1.30 (GSW's OpenGL 3.3 context does). Supply the
actual camera matrices for the same frame as the model renderer.

| Uniform | Type | Meaning |
|---|---|---|
| `inverseProjection` | `mat4` | Inverse of the actual projection including lens/aspect correction; **not** inverse combined view-projection. Its model-dependent clip planes do not clip the analytic reference plane. |
| `inverseViewRotation` | `mat3` | View direction to world direction; upper-left 3 by 3 of inverse view, translation removed. Must be orthonormal, with no scale/shear. |
| `planeBasis` | `mat3` | Columns are orthonormal world vectors **U, V, N**, where N is the plane normal. Either handedness is valid; transpose is its inverse. |
| `cameraPlanePosition` | `vec3` | `(dot(camera-origin,U), dot(camera-origin,V), dot(camera-origin,N))`; `origin` is the persistent scene/reference grid origin, never the selected model's bottom. |
| `gridSteps[8]` | `float` array | Positive ascending spacings in the same local world units as the camera/reference. Use eight powers of ten spanning the visible resolution, respecting GSW's configured minimum physical step. Do not feed the old snapped 1-2-5 major-step sequence. |
| `gridPhase[8]` | `vec2` array | For each step: CPU double `std::remainder(cameraU, step)` and `std::remainder(cameraV, step)`, converted to float only after reduction. Same U/V/origin as `cameraPlanePosition`. |
| `gridDistance` | `float` | Positive distance at which perspective distance fade begins; fade reaches zero at twice this distance. Blender uses clip-end divided by two; GSW supplies a reference/camera-derived range independent of selected-model clipping. Orthographic views do not use this distance fade. |
| `gridLineSize` | `float` | Blender's additional half-width `max(physicalPixelScale - 1, 0) / 2`; zero gives its nominal one-physical-pixel line. |
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

The module is validated through the integrated rendered-viewport seam, not
private shader-string/LOD implementation tests. Both projections, all reference
planes, roll, close/far scales, custom backgrounds, transparent framebuffer
alpha, model occlusion and navigation/editing remain caller acceptance gates.

中文：实际移植 Blender 的网格线过滤、多级渐变与远处淡出；保留本软件的背景配置、
坐标、导航和模型渲染，不宣称复制了整个 Blender 引擎。

日本語：Blender のグリッド線フィルター、多段階ブレンド、遠方フェードを実際に
移植しています。背景設定・座標・操作・モデル描画は本ソフトのものを保持し、
Blender エンジン全体をコピーしたものではありません。
