# World-size model transform gizmos

Move, Rotate, Scale and combined Transform now use a stable world-space radius:
`sourceSceneRadius * 0.3` for standalone tools, `sourceSceneRadius * 0.5` for
the combined tool. This reference follows the loaded source bounds, not camera
distance, adaptive grid spacing, font size, or the transform currently being
edited. Scaling the object does not feed back into drag sensitivity.

The world radius is projected through the actual view/projection matrix. There
is no minimum, maximum, or softened screen radius. Perspective distance halving
doubles the camera-facing parts; halving orthographic extent does the same.
Axis endpoints and plane corners are projected in 3D, including foreshortening.
Arrowheads and scale blocks use their endpoint depth; centre balls, trackball
bounds and the uniform-scale diamond share the same world-size reference.
Stroke widths, text and toolbar/navigation UI retain readable screen dimensions.

Drawing and picking consume the same layout. Pixel hit tolerance is small and
shrinks for tiny handles; circular and diamond centres are not picked as large
invisible rectangles. Rotation rings are clipped segment by segment in clip
space, so crossing the near plane cannot remove an entire ring or create a false
closing edge. Keyboard rotation and trackball sensitivity use the projected
radius captured at gesture start. The previous edge-on axis-rotation fallback,
precision, snapping, numeric entry, cancellation and undo/redo remain intact.

## Validation

`ModelInteractionTests` covers perspective distances 32, 16, 8, 4 and 2; multiple
orthographic extents; all four modes; oblique 3D endpoints and plane handles;
shrinking hit regions; and rotation rings crossing the near plane. Initial tests
failed against fixed-pixel sizing before the implementation changed.

The reference-axes GUI smoke uses actual wheel events and hover descriptions to
measure the outer rotation target at four distances in both projection modes,
for standalone Rotate and combined Transform. Its screenshot series is named
`gsw-model-gizmo-{persp,ortho}-{rotate,combined}-{0,1,2,3}.png` in the test temp
directory. Existing real-input horizontal Z rotation, modifiers and undo/redo
checks run in the same smoke test.

Very distant handles may become subpixel, and very close handles can extend
outside the viewport: these are intentional consequences of genuine world size.
Use Find Model / `F` to frame the object, or the existing keyboard transforms.
The bottom-right orientation widget is navigation UI and remains screen-sized.

## Spatial presentation upgrade / 空间层次升级 / 空間表現の改善

中文：移动、旋转、缩放与综合变换共用新的布局／绘制模块。中心的可见直径从 `20/24 × shapeScale` 缩为 `8/10 × shapeScale`，约减少 58–60% 的直径和 83–84% 的遮挡面积，并改为空心轮廓；原有拾取半径与少量容差保留，但不会给远处的微小操作器添加固定像素热区。整体世界半径、轴端位置、旋转半径、透视／正交缩放规律与 XYZ 颜色约定不变。

English: Move, Rotate, Scale and combined Transform share the new layout/painter. The visible center diameter changes from `20/24 × shapeScale` to `8/10 × shapeScale`, reducing diameter by about 58–60% and covered area by about 83–84%, with an unfilled outline. Existing hit radius and small tolerance remain; distant tiny controls do not acquire fixed-pixel hot regions. World radius, axis endpoints, rotation radius, perspective/orthographic scaling and XYZ color convention are unchanged.

日本語：移動・回転・拡大縮小・総合変換は、新しいレイアウト／描画モジュールを共有します。中心の表示直径は `20/24 × shapeScale` から `8/10 × shapeScale` へ縮小し、直径を約 58–60%、遮蔽面積を約 83–84% 減らして、塗りつぶしのない輪郭にします。従来の選択半径と小さな許容幅を維持し、遠くの微小な操作器には固定ピクセルの選択領域を追加しません。世界半径、軸端、回転半径、透視／平行投影の拡縮則と XYZ の色の対応は変更しません。

中文：旋转环保留裁剪后的真实 NDC 深度，前弧清晰、后弧轻淡；穿越枢轴深度平面的线段拆分，彩色线段与实心柄按由远到近绘制，不按轴编号互相压盖。连续底描边避免逐线段描边形成锯齿节。移动柄为世界空间的 8 分面锥头，缩放柄为局部立方体，可见面经齐次裁剪并用相机左上方光照分出明暗。平面柄保持轻填充，轨迹球填充只在悬停／操作时出现。绘制与拾取使用同一几何；同种重合手柄优先选中更近者，连可分辨的微小深度差也不因固定深度容差而忽略。这里的深度是操作器自身层次，不改模型材质或场景深度算法。

English: Rotation arcs retain clipped NDC depth: near arcs are clear and far arcs restrained. Edges crossing the pivot-depth plane are split; coloured strokes and solid grips paint far-to-near, not in XYZ order. A connected underlay avoids dark teeth caused by per-edge halos. Move heads are world-space eight-facet cones; scale grips are local cubes with homogeneously clipped faces and camera-relative upper-left lighting. Plane grips have light fills; trackball fill appears only during hover/interaction. Picking uses the same geometry and prioritizes nearer overlapping grips of the same kind, including small resolvable depth differences. This is gizmo depth, not a change to model materials or scene-depth rendering.

日本語：回転弧はクリップ後の NDC 深度を保持し、近い弧を明確に、遠い弧を控えめに描きます。枢軸の深度を跨ぐ線分を分割し、色付き線分と立体ハンドルを軸番号ではなく遠方から手前へ描画します。連続した下描きで、線分ごとの輪郭が生む細かな暗い節を防ぎます。移動先端は世界空間の 8 面の円錐、拡大縮小ハンドルはローカル立方体で、可視面を同次座標でクリップし、カメラ左上の照明で明暗を分けます。平面ハンドルは薄く塗り、トラックボールの塗りはホバー／操作時だけ表示します。選択は同じ形状を使い、同種の重なるハンドルは小さくても識別できる深度差に従って近い方を優先します。操作器自体の深度表現であり、モデル材質やシーンの深度描画は変更しません。

### Acceptance and references / 验收与参考 / 検証と参考

`transform_gizmo_visual` paints directly into `QImage`, without a window. It
checks hollow centers, front/back contrast, depth-reversing intersections,
lit cube faces and all four tools in both themes/projection types. With
`GSW_SMOKE_OUTPUT_DIR` it exports sixteen controlled sample frames for review;
these are not screenshots of the user's project. Geometry tests retain all
existing zoom, clipping, constraints and interaction assertions.

- [Blender 4.5 transform gizmo](https://github.com/blender/blender/blob/v4.5.0/source/blender/editors/transform/transform_gizmo_3d.cc) and [dial implementation](https://github.com/blender/blender/blob/v4.5.0/source/blender/editors/gizmo_library/gizmo_types/dial3d_gizmo.cc): compact unfilled move center, tool-specific display and front-hemisphere rotation hierarchy. GSW retains faint rear arcs rather than copying Blender's default clipping behavior.
- [CloudCompare interactive transformation](https://www.cloudcompare.org/doc/wiki/index.php/Interactive_Transformation_Tool): object transformation and axis constraints; its navigation pivot is not treated as the same editing tool.
- [Metashape 2.3 official manual](https://www.agisoft.com/pdf/metashape-pro_2_3_en.pdf): separate transformation tools and trackball sizing, not a published rendering algorithm.

The earlier presentation implementation studied Blender/CloudCompare without
copying their code. On 2026-10-14, after explicit GPL licensing approval,
GSW additionally ports Blender 4.5 cone generation and cube geometry into the
Qt adapter at `native/third_party/blender-gizmo/`. Copyright, source hashes,
modifications and GPL notices are retained there. Metashape remains a
closed-source behavior reference. Original models, project coordinates and
generation backends are unchanged.

### Arrow junction correction / 箭头接合修复 / 矢印の接合修正

中文：移动、缩放与综合工具采用 Blender 的细锥头／小方块比例，杆线统一为
1.5 逻辑像素，无粗黑描边和重渐变。杆线在锥底或方块接合面终止，不穿入实体。
斜视重叠时按每个表面的局部 NDC 深度遮挡杆线，而不是用整根杆的平均深度；
前方外部杆线保留，后方杆线隐藏。三种独立工具及综合工具统一为轻量轮廓。
整体世界半径、轴端位置、坐标颜色和变换语义保持不变，极近视角仍可自然放大。

English: Move, Scale and combined Transform use Blender's slender cone/compact
box proportions. Stems are 1.5 logical pixels with no heavy black border or
gradient. They terminate at the cone base or cube junction, never inside the
solid. Oblique overlaps compare local NDC surface depth, not whole-stem mean
depth: nearer exterior stems remain visible, farther stems are hidden. All
standalone tools and combined Transform use the restrained presentation family.
World radius, endpoints, axis colours and transform semantics are unchanged;
extreme close views still naturally enlarge world-sized geometry.

日本語：移動・拡大縮小・総合変換は Blender の細い円錐／小型ブロックの比率を
使用します。軸線は 1.5 論理ピクセルで、太い黒枠や強いグラデーションは使い
ません。円錐の底面やブロック接合面で終わり、立体内部へ伸ばしません。
斜めの重なりでは線全体の平均ではなく表面位置の NDC 深度で遮蔽し、手前の
外部軸線は表示、奥は非表示にします。単独３ツールと総合変換の表現を統一し、
世界半径・軸端・色対応・変換処理は維持します。極近距離では自然に拡大します。

The QImage regressions first reproduced 120 intruding pixels in a close
oblique cone. The corrected slender geometry has zero intrusion among 741
opaque side samples. A separate crossing test rejects both whole-line average
draw orders and requires physically near stems to remain visible. Reference
proportion tests cover standalone and combined move/scale heads; existing
world-size, zoom, constraints and picking tests remain enabled.
