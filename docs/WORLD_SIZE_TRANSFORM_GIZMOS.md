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
