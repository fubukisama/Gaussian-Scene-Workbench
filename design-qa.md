# Orientation Gizmo Design QA

Source visual truth: `C:\Users\ztx\AppData\Local\Temp\codex-clipboard-0eae3d25-8b60-415d-ae83-4ded4607ca24.png`

Rendered implementation evidence:

- Full orthographic frame: `C:\Users\ztx\AppData\Local\Temp\gsw-infinite-grid-smoke.png`
- Focused orthographic crop: `C:\Users\ztx\AppData\Local\Temp\gsw-unity-gizmo-ortho-crop.png`
- Normalized side-by-side comparison: `C:\Users\ztx\AppData\Local\Temp\gsw-unity-gizmo-ortho-comparison.png`
- Focused oblique perspective crop: `C:\Users\ztx\AppData\Local\Temp\gsw-unity-gizmo-oblique-crop.png`

Viewport and normalization:

- Source: 213 x 182 pixels, Unity axis-aligned orthographic state.
- Implementation full frame: 2027 x 1136 device pixels, native Qt/OpenGL viewport.
- Implementation focused crop: 243 x 205 device pixels.
- Comparison canvas: source normalized with Lanczos to 240 x 205 and implementation retained at 243 x 205. CSS size and browser device scale do not apply to this native desktop control.
- Matched state: axis-aligned orthographic view with the projection label visible. The implementation intentionally remains Z-up, so its green vertical label is `Z` rather than Unity's Y-up `Y`.

## Full-view comparison evidence

The control remains anchored to the lower-right viewport corner without colliding with the separate camera, pan, or zoom controls. Its footprint is materially smaller than the reported build, and the projection label stays below the direction handles.

## Focused comparison evidence

The axis-aligned comparison matches Unity's hierarchy: compact light center face, outward conical direction handles, neutral rear handles, axis labels beyond the positive tips, and a separate `≡ Iso` label. The oblique crop verifies the state that cannot be seen in the axis-aligned Unity reference: the center becomes a camera-projected three-face cube and the cones expose distinct light and shadow faces.

## Fidelity surfaces

- Fonts and typography: native system UI text remains crisp at device density. Axis labels are bold and compact; projection text is smaller and separated. The exact Unity font is intentionally not redistributed.
- Spacing and layout rhythm: the center cube is limited to 30% of the gizmo radius. Cone bases and hit regions remain outside the cube, and near-view-aligned axes collapse before controls crowd each other.
- Colors and visual tokens: front X/Y/Z directions retain the application's red/blue/green Z-up convention; rear directions use neutral gray. Face shading and shadows provide depth without adding a large background plate.
- Image quality and asset fidelity: the widget is rendered as resolution-independent camera-projected geometry. No rasterized Unity assets or Reference-Only source files are copied.
- Copy and content: `≡ Persp` and `≡ Iso` expose projection state; axis labels reflect the application's coordinate convention.

## Comparison history

1. Initial P1: the 54%-radius flat center square covered foreshortened circular axis handles. Fixed by reducing the cube to 30%, enforcing clearance, and matching hit regions to visible controls.
2. Second P1: replacing circles with single-color triangles still looked flat and did not communicate camera orientation. Fixed by projecting an actual cube from the camera matrix, drawing visible faces by depth, splitting every cone into light and dark faces, adding shaft/cone shadows, and fading rear directions.
3. Post-fix evidence: both focused crops show the compact, non-overlapping result; native regression and framebuffer smoke tests pass.

## Findings

No actionable P0, P1, or P2 mismatch remains. The operating-system font and the required Z-up axis mapping are accepted product constraints rather than design drift.

## Follow-up polish

- P3: tune face brightness after user review on the primary monitor if its gamma makes the gray rear cones appear too bright.

final result: passed
