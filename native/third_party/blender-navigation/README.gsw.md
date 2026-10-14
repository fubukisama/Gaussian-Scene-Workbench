# Blender view-navigation adaptation

Pinned upstream: Blender **v4.5.0**, commit
`8cb6b388974a817afedf1317ce26f0c75aa5f181`.

This module adapts actual Blender algorithms, not its complete viewport,
operator, GPU, preferences or undo engine. The original **GPL-2.0-or-later**
copyright/license notices are preserved in both source files. GSW's native
application is GPL-3.0-or-later; the compatible later version is selected when
distributing the combined application. See repository/package `COPYING`.

## Sources

UTF-8 original file SHA-256, retrieved at the pinned commit:

- [`view3d_navigate_view_zoom.cc`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/editors/space_view3d/view3d_navigate_view_zoom.cc#L113),
  `view_zoom_to_window_xy_3d`, lines 113–150; copyright 2023 Blender Authors.
  SHA-256: `f2765d53d5e869052aabaa8610096a8eb581fdb0115ca8e92f460cd30ee3c1b3`.
- [`view3d_project.cc`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/editors/space_view3d/view3d_project.cc#L674),
  `ED_view3d_win_to_delta`, lines 674–689; copyright 2008 Blender Authors.
  SHA-256: `5790878381b63df81888a253811b8b25451ee7aa05c0eb503367f8956916e5f5`.
- [`view3d_navigate.cc`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/editors/space_view3d/view3d_navigate.cc#L1020),
  `viewmove_apply`, lines 1020–1052; copyright 2023 Blender Authors.
  SHA-256: `3537751c447fc748c0978ab87df9ddb892ed5042322968a379a5dd26e7c745ed`.

## Modifications, 2026-10-14

- Replace Blender data structures and vector functions with Qt value types.
- Adapt Blender's negated `ofs` to GSW's positive camera target and its lower-left
  window coordinates to top-left logical Qt pixels.
- Retain zoom's `target + dvec - dvec * factor` anchor compensation. Compute its
  before/after offsets from the actual projection matrices, rather than assuming
  the requested factor survived distance limits or the minimum orthographic
  extent. This is necessary for GSW's existing near-zoom orthographic floor.
- Express Blender's `zfac`/inverse-projection pixel delta through inverse-matrix
  direction mapping scaled by the target's clip W. Keeping the inverses separate
  avoids near/far homogeneous-depth cancellation. The actual camera lens, focal aspect,
  viewport dimensions, projection mode and roll determine the result; no fixed
  `distance * 0.0018` pan sensitivity remains in this module.
- Return an optional finite target. Invalid, singular or non-finite cameras and
  viewport inputs are rejected without writing caller state.

The zoom interface requires both frames to keep the same camera orientation and
target before applying its returned target. It preserves the point beneath the
cursor on the **camera-target depth plane**. It does **not** claim surface-depth
anchoring, auto-depth navigation, or any additional GPU picking. Panning follows
the pointer exactly on the same plane. No model geometry, source coordinates,
transforms, training state or project records are modified.

`BlenderViewNavigationTests` exercises the public helpers through actual camera
projection: independent known numeric cases, both projection modes, portrait and
short-wide windows, rolled/opposite/oblique views, corrected camera focal aspect,
actual distance/orthographic limits, invalid inputs and unchanged input values.
It uses QCoreApplication/Qt value math only, with no desktop window.

中文：实际移植 Blender 的鼠标锚定缩放与投影平移算法，并适配 Qt。缩放锚点位于
相机目标深度平面，不宣称读取模型表面深度；模型、工程与训练数据保持不变。

日本語：Blender のカーソル位置を基準とするズームと投影に応じたパンのアルゴリズム
を実際に移植し、Qt に適合させています。ズームの基準はカメラターゲットの深度平面
であり、モデル表面の深度取得を意味しません。モデル・プロジェクト・学習状態は変更
しません。
