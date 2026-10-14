# Blender transform-handle geometry

Pinned upstream: Blender **v4.5.0**, tag object
`8cb6b388974a817afedf1317ce26f0c75aa5f181`.

This is actual GPL source adaptation, not just a visual reference. The original
GPL-2.0-or-later notices are retained. GSW distributes the native application
under GPL-3.0-or-later, selecting a compatible later GPL version for this port.
The full GPLv3 text is at repository `COPYING` and in native packages.

Sources (UTF-8 upstream file SHA-256):

- [`gpu_immediate_util.cc`](https://github.com/blender/blender/blob/v4.5.0/source/blender/gpu/intern/gpu_immediate_util.cc), cylinder triangle emission:
  `9c52a0fefa0303ebd3a9521ad4e99bc7a972ce435601eb0235031f3ff1359530`.
- [`geom_cube_gizmo.cc`](https://github.com/blender/blender/blob/v4.5.0/source/blender/editors/gizmo_library/geometry/geom_cube_gizmo.cc), cube vertex/index arrays:
  `78512a137ac44e1924a0d794547d2bf98eed3a278e2ea5fda1721e3007ad7054`.
- [`arrow3d_gizmo.cc`](https://github.com/blender/blender/blob/v4.5.0/source/blender/editors/gizmo_library/gizmo_types/arrow3d_gizmo.cc), normal arrow and box proportions:
  `771c4f3afd210adfdfed1057ea67f2aee57b4870e87e685bd6b952c884dd067c`.

Modifications, 2026-10-14: generate Qt value geometry instead of Blender GPU
immediate calls; retain the normal arrow's eight cone facets and cap, omit
degenerate top-zero triangles; use the original cube vertex/index data;
adapt local geometry to GSW's axis orientation and existing fixed world extent.
GSW homogeneous clipping, per-face NDC depth, QPainter presentation and hit
testing form the adapter. Stem ends at the cone base or cube face; it does not
run through a solid head. Per-surface clipping handles oblique projected overlap.

Move/Scale and combined Transform share these shapes. Rotation uses GSW's
existing clipped arcs with front/back hierarchy and thin presentation; no claim
is made that Blender's complete rotation operators, GPU engine or undo system
were copied. GSW constraints, numeric entry, undo, coordinate conventions,
world sizing and scene/worker lifecycle remain its own implementation.

中文：实际移植 Blender 的锥头生成、立方体数据和柄比例，保留 GPL 声明；Qt 绘制、
裁剪与 GSW 操作逻辑由本项目适配，不是整套 Blender 引擎的原样复制。

日本語：Blender の円錐生成・立方体データ・ハンドル比率を実際に移植し、GPL
通知を保持します。Qt 描画・クリッピングと GSW の操作処理を適合させたもので、
Blender のエンジン全体をそのままコピーしたものではありません。
