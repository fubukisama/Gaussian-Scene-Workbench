# Central reference-axis visual design

The scene origin has a compact, screen-sized marker: bevel-highlighted shafts,
two-tone arrowheads, upright outlined XYZ letters, and a small neutral origin
collar. Long grid axes are subdued so the marker carries the visual emphasis.
The existing X-red, Y-blue, Z-green convention is preserved.

The design uses the distinction between grid guides and readable navigation
markers seen in [Blender's viewport overlays](https://docs.blender.org/manual/en/4.4/editors/3dview/display/overlays.html)
and [Unity's gizmo display options](https://docs.unity3d.com/Manual/GizmosMenu.html).
This is an original implementation; it does not copy their code or assets.

## Rendering behavior

- One common world length is calculated from the camera projection and refined
  so the longest projected axis occupies about 76 logical pixels at normal UI
  scale. Real foreshortening is preserved instead of stretching all axes to
  the same apparent length.
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
  0.02 to 1,000,000, maintaining the screen footprint and finite scene depth.
- Native framebuffer checks compare the default view, 24 zoom-out steps, and
  the same distant orthographic view. On the desktop OpenGL run, the central
  green marker occupied 424 classified pixels in all three captures.
- The solid-box mesh fixture fully occludes the compact marker. Its check now
  expects zero visible green-marker pixels rather than the old long Z line
  projecting out of the box; the unoccluded zoom check verifies visibility.
- Visual review used same-camera, same-density captures from the installed
  previous build and the new build. The revised arrow faces are solid and
  antialiased, lettering is legible, and no large background panel is added.

Local review artifacts are generated under
`native/build-unity-gizmo/axis-art-qa/`: `central-axis-before-after.png`,
`central-axis-detail.png`, `central-axis-far-detail.png`, and full-frame
`gsw-reference-axis-ortho.png`.
