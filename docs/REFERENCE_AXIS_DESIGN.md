# Central reference-axis visual design

The scene origin has fixed-world-length axes: bevel-highlighted shafts,
two-tone arrowheads, upright outlined XYZ letters, and a small neutral origin
collar. Long grid axes are subdued so the marker carries the visual emphasis.
The existing X-red, Y-blue, Z-green convention is preserved.

The design uses the distinction between grid guides and readable navigation
markers seen in [Blender's viewport overlays](https://docs.blender.org/manual/en/4.4/editors/3dview/display/overlays.html)
and [Unity's gizmo display options](https://docs.unity3d.com/Manual/GizmosMenu.html).
This is an original implementation; it does not copy their code or assets.

## Rendering behavior

- The reference length is 30% of the source scene radius, independent of camera
  distance, FOV, UI scale, viewport size, and adaptive grid spacing. Each true
  endpoint is exactly `origin + axis * referenceLength`; projection alone sets
  its screen position. There is no minimum or maximum pixel length, saturation
  curve, or viewport-fitting correction. The previous bounded scaling approach
  was incorrect because it suppressed genuine length changes during close zoom.
- In perspective, a transverse shaft doubles its projected length when camera
  distance halves. Oblique shafts keep their real perspective foreshortening.
  In orthographic projection the length follows the orthographic view extent;
  changing camera distance alone does not change projected length.
- Only stroke width, arrowhead artwork, and labels use readable screen sizes.
  Crowded far-away tips/letters are omitted without stretching the shaft, and
  the origin collar shrinks with tiny axes. At extreme distance the real marker
  can become subpixel; the separate corner navigation gizmo remains available.
- Homogeneous frustum clipping preserves visible shafts when an endpoint leaves
  the screen or crosses the near plane, even if the origin is offscreen. A clipped
  endpoint does not receive a fake arrowhead or a label pinned to the viewport edge.
- Shafts, bevels, arrow faces, letters, and the origin collar are triangles.
  Their width is independent of the driver's supported `glLineWidth` range.
- Projected vertices retain scene depth. The marker writes depth before model
  rendering, so points, meshes, and Gaussian compositing can cover rear parts.
  Coplanar decoration layers are drawn in order to avoid depth stripes.
- A view-aligned arrow collapses before its tip and letter crowd the origin.
  Offscreen or behind-camera endpoints do not produce floating labels.
- The axes are not transform handles. Their lengths use the scene's source units;
  the numeric grid remains the labeled scale reference. No source data or
  coordinates are changed.

## Verification

- Geometry checks verify emitted arrow-tip vertices against exact world endpoint
  projections at camera distances 12, 6, and 3, across UI densities: projected
  lengths are 1:2:4, including lengths exceeding the previous size caps.
- Tests also cover near-plane/screen-edge clipping, offscreen origins with visible
  shafts, no far-distance pixel-length floor, fixed-extent orthographic behavior,
  finite depth from 0.02 to 1,000,000, and equivalent mm/m scene representations.
- Native framebuffer checks zoom in 4 steps and then another 4, as well as zooming
  out 6 and 24 steps and switching distant projection. On desktop OpenGL the green
  marker occupies 336, 651, and 1339 classified pixels during the zoom-in sequence;
  the 6-step zoom-out yields 161 and both extreme-distance views yield 0.
- The solid-box mesh fixture fully occludes the compact marker. Its check now
  expects zero visible green-marker pixels rather than the old long Z line
  projecting out of the box; the unoccluded zoom check verifies visibility.
- Visual review uses same-density default/close/closer captures. The arrow faces
  remain solid and antialiased; endpoints keep moving outwards as the camera
  approaches instead of saturating at a small fraction of the viewport.

Local review artifacts are generated under
`native/build-unity-gizmo/axis-world-length-qa/`:
`central-axis-physical-zoom.png`, `gsw-unity-orientation-smoke.png`,
`gsw-reference-axis-close.png`, `gsw-reference-axis-closer.png`,
`gsw-reference-axis-mid.png`, `gsw-reference-axis-far.png`, and
`gsw-reference-axis-ortho.png`.
