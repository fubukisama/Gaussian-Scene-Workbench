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
  `viewmove_apply`, lines 1020–1052, and `axis_set_view`, especially lines
  955–960 for automatic projection selection; copyright 2023 Blender Authors.
  SHA-256: `3537751c447fc748c0978ab87df9ddb892ed5042322968a379a5dd26e7c745ed`.
- [`view3d_utils.cc`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/editors/space_view3d/view3d_utils.cc#L534),
  `ED_view3d_persp_ensure`, lines 534–559; copyright 2008 Blender Authors.
  SHA-256: `472d4ed1d305d5fcea10bc0d3a74af7ecdf9a3d02d0ec6b89a766d3a1192252c`.

The companion Qt navigation-gizmo adapters in `native/src/NavigationGizmo.cpp`
and `native/src/BlenderNavigationPresentation.h` also adapt:

- [`view3d_gizmo_navigate_type.cc`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/editors/space_view3d/view3d_gizmo_navigate_type.cc#L53),
  `gizmo_axis_draw`, lines 53–287, and `gizmo_axis_test_select`, lines 289–340;
  copyright 2023 Blender Authors.
  SHA-256: `6c1e71395e46bd8dbf6da5bced22f0bdfe3e68a460a74a8207e1be0f242604eb`.
- [`view3d_gizmo_navigate.cc`](https://github.com/blender/blender/blob/8cb6b388974a817afedf1317ce26f0c75aa5f181/source/blender/editors/space_view3d/view3d_gizmo_navigate.cc#L258),
  `WIDGETGROUP_navigate_setup`, signed-axis mapping and click/drag dispatch,
  lines 258–275; copyright 2023 Blender Authors.
  SHA-256: `2cac9cfc0ac73f31a26f8ff31c362a84f8550778784a72e313fd7ff4a5da570e`.

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

## Navigation-gizmo adaptation, 2026-10-24

The Qt adapter now follows Blender's default Auto Perspective rules: selecting
a signed axis enters its orthographic view; starting an orbit from that axis
view restores perspective. Panning and zooming preserve the projection.
Manually selecting orthographic projection in an oblique user view does not
make every later orbit force perspective. GSW supplies the camera-state update,
framing preservation and event lifecycle around the adapted predicates.

The gizmo has no central projection icon or projection hit target. Projection
has its own mini button below the camera button. Six projected axis ends retain
Blender's depth ordering and near/far radius variation. An axis aligned with the
line of sight keeps its near disc at the center and hides only the far disc;
clicking that center disc chooses the opposite axis view. Inside the gizmo's
circular area, selection uses Blender's nearest projected-axis rule, without a
disc-distance cutoff. Axis presses are deferred until release so dragging from
an axis or the center orbits instead of also snapping the view. Qt replaces
Blender's window-manager dispatch, GPU drawing and font API; semantic mini-button
artwork remains GSW's separately attributed icon set.

中文：实际移植 Blender 的鼠标锚定缩放与投影平移算法，并适配 Qt。缩放锚点位于
相机目标深度平面，不宣称读取模型表面深度；模型、工程与训练数据保持不变。
轴向导航同时移植默认自动透视规则：点击轴端进入正交视图，从轴向视图开始旋转
时回到透视；平移和缩放保持投影。斜角下手动启用的正交视图旋转时仍保持正交。
删除中心投影按钮，独立投影按钮位于相机按钮下方。沿视线的轴保留近端中心圆点，
点击可查看反向；圆形导航区域内按最近投影轴端选择，拖动时旋转视图。

日本語：Blender のカーソル位置を基準とするズームと投影に応じたパンのアルゴリズム
を実際に移植し、Qt に適合させています。ズームの基準はカメラターゲットの深度平面
であり、モデル表面の深度取得を意味しません。モデル・プロジェクト・学習状態は変更
しません。
軸ナビゲーションには既定の自動透視切り替えも移植します。軸端のクリックで軸に
沿った平行投影となり、そこから回転を始めると透視投影に戻ります。パンとズームは
投影を維持し、斜めのユーザービューで手動選択した平行投影も回転時に維持します。
中央の投影ボタンは削除し、独立した投影ボタンをカメラボタンの下に配置します。
視線方向の軸は手前の円を中央に残し、クリックで反対側を表示します。円形領域内は
最も近い投影軸端を選び、ドラッグはビューの回転として扱います。
