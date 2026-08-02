# Orientation Gizmo Ergonomics Design QA

Source visual truth:

- Unity interaction reference: `C:\Users\ztx\AppData\Local\Temp\codex-clipboard-0eae3d25-8b60-415d-ae83-4ded4607ca24.png`
- Reported compact-control defect: `C:\Users\ztx\AppData\Local\Temp\codex-clipboard-7b204b72-a7d5-458a-b5dd-45c3a7a220cd.png`

Rendered implementation evidence:

- Full native frame: `C:\Users\ztx\AppData\Local\Temp\gsw-unity-orientation-smoke.png`
- Focused implementation crop: `C:\Users\ztx\AppData\Local\Temp\gsw-unity-gizmo-ergonomic-crop.png`
- Normalized before/after comparison: `C:\Users\ztx\AppData\Local\Temp\gsw-gizmo-ergonomic-before-after.png`
- Normalized Unity/implementation comparison: `C:\Users\ztx\AppData\Local\Temp\gsw-gizmo-ergonomic-unity-comparison.png`

Viewport and normalization:

- Reported defect: 234 x 207 pixels, oblique perspective state with the center control idle.
- Unity reference: 213 x 182 pixels, axis-aligned orthographic state.
- Implementation full frame: 2027 x 1136 device pixels, native Qt/OpenGL viewport.
- Implementation focused crop: 243 x 205 device pixels, oblique perspective state with the center projection target hovered.
- Before/after comparison: both sides normalized to 243 x 205 with Lanczos scaling.
- Unity comparison: reference normalized to 240 x 205 and implementation retained at 243 x 205.
- CSS size and browser device scale do not apply to this native desktop control.

## Full-view comparison evidence

The full native frame keeps the existing lower-right allocation and its separate camera, pan, and zoom controls. The outer layout radius is unchanged from the preceding build; no enlarged background plate or globally scaled widget was introduced.

## Focused comparison evidence

The before/after composite shows that the center cube is now visually legible while the six direction controls use the existing outer-ring whitespace. Hover exposes a circular acquisition target around the cube. The target is independent of visual geometry, so its clickable diameter is 34-42 px while the cube remains 20-25 px. Cone tips are positioned at 90% of the existing layout radius and their bases must remain outside the circular center target plus a four-pixel safety gap. The circular target leaves its diagonal corners available for free orbit instead of turning the whole middle area into a blocking square.

The Unity comparison retains the intended hierarchy: dimensional center, outward conical directions, neutral rear directions, external axis labels, and a separate projection label. The implementation intentionally remains Z-up, so green vertical is `Z` rather than Unity's Y-up `Y`.

## Fidelity surfaces

- Fonts and typography: native system UI text remains crisp at device density. Axis labels are bold and stay outside the cones; projection text remains smaller and visually separate. The exact Unity font is not redistributed.
- Spacing and layout rhythm: the outer widget allocation is unchanged. Internal spacing now separates the center acquisition target, cone bases, cone tips, and labels without globally enlarging the control.
- Colors and visual tokens: X/Y/Z retain the application's red/blue/green Z-up convention; rear directions remain neutral gray. The hover ring uses a low-opacity cool neutral and disappears when the pointer leaves.
- Image quality and asset fidelity: the cube and direction cones remain resolution-independent camera-projected geometry with face shading and antialiasing. No Unity raster assets or source files are copied.
- Copy and content: `≡ Persp` and `≡ Iso` expose projection state; axis labels match the application's coordinate convention.

## Comparison history

1. Earlier P1: a flat, oversized center square hid foreshortened handles. Fixed by projecting a real compact cube and using cone heads with depth-aware collapse.
2. Reported P1: the compact cube had only a roughly 14-20 px click target and all direction controls clustered near it, making the center difficult to acquire and causing wrong-axis clicks.
3. First attempted fix was rejected before handoff because globally increasing the radius would consume scene space.
4. Revised fix: restored the previous outer radius, increased only the visual cube to 20-25 px, added an independent 34-42 px circular target, moved cones into existing outer-ring whitespace, and enforced target/cone clearance. Diagonal target corners remain free-orbit space.
5. Post-fix evidence: the normalized composites show a readable center and separated arrows without a larger permanent footprint. All 25 native tests and 42 Python worker tests pass, including center-target, free-orbit-corner, cone-clearance, and viewport-bound assertions.

## Findings

No actionable P0, P1, or P2 mismatch remains. The persistent Z-up mapping, operating-system font, and hover-only target ring are accepted product constraints rather than design drift.

## Follow-up polish

- P3: after hands-on use, the hover-ring opacity can be tuned without changing geometry or hit arbitration.

final result: passed
