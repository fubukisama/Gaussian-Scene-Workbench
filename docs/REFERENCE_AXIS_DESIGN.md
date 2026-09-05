# Central reference-axis visual design

The scene origin has a compact, distance-responsive marker: bevel-highlighted shafts,
two-tone arrowheads, upright outlined XYZ letters, and a small neutral origin
collar. Long grid axes are subdued so the marker carries the visual emphasis.
The existing X-red, Y-blue, Z-green convention is preserved.

The design uses the distinction between grid guides and readable navigation
markers seen in [Blender's viewport overlays](https://docs.blender.org/manual/en/4.4/editors/3dview/display/overlays.html)
and [Unity's gizmo display options](https://docs.unity3d.com/Manual/GizmosMenu.html).
This is an original implementation; it does not copy their code or assets.

## Rendering behavior

- The reference length is 30% of the source scene radius, independent of camera
  distance and adaptive grid spacing. Projection turns this into a screen length;
  a smooth response approaches 28 logical pixels at extreme distance and 124
  close up (at normal UI scale, also limited to 22% of the smaller viewport side).
  It visibly shrinks during zoom-out without abrupt hard-clamp plateaus or jumps
  when the grid changes level. This replaces the previous fixed 76-pixel footprint.
- One common world length is refined for perspective so real foreshortening is
  preserved instead of stretching all axes to the same apparent length. Shafts,
  arrowheads, letters, and the collar also scale moderately with distance, with
  a legibility floor for their thickness and lettering.
- Shafts, bevels, arrow faces, letters, and the origin collar are triangles.
  Their width is independent of the driver's supported `glLineWidth` range.
- Projected vertices retain scene depth. The marker writes depth before model
  rendering, so points, meshes, and Gaussian compositing can cover rear parts.
  Coplanar decoration layers are drawn in order to avoid depth stripes.
- A view-aligned arrow collapses before its tip and letter crowd the origin.
  Offscreen or behind-camera origins do not produce floating labels.
- The marker is an orientation aid, not a ruler or a transform handle. Its
  apparent length is decorative; numeric size still comes from the grid and
  the imported model's source units. No source data or coordinates are changed.

## Verification

- Geometry checks cover perspective and orthographic views from distances
  0.02 to 1,000,000: monotonically decreasing size, meaningful ordinary-zoom
  changes, bounded visibility, and finite scene depth. Changing scene units
  and camera distance together preserves the appearance.
- Native framebuffer checks compare the default view, 6 and 24 zoom-out steps,
  and the same distant orthographic view. On the desktop OpenGL run, the central
  green marker occupied 260, 179, 128, and 128 classified pixels respectively.
- The solid-box mesh fixture fully occludes the compact marker. Its check now
  expects zero visible green-marker pixels rather than the old long Z line
  projecting out of the box; the unoccluded zoom check verifies visibility.
- Visual review uses same-density near/middle/far captures. The revised arrow
  faces remain solid and antialiased, lettering is legible, and no large
  background panel is added. The center marker is no longer screen-size invariant.

Local review artifacts are generated under
`native/build-unity-gizmo/axis-distance-qa/`:
`central-axis-distance-comparison.png`, `gsw-unity-orientation-smoke.png`,
`gsw-reference-axis-mid.png`, `gsw-reference-axis-far.png`, and
`gsw-reference-axis-ortho.png`.
