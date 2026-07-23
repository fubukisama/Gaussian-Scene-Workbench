# Navigation Gizmo Design QA

**Source visual truth**

- Full capture: `E:\Gaussian-Scene-Workbench-Dev\.tools\design-qa\blender-4.5-navigation-reference.png`
- Focus crop: `E:\Gaussian-Scene-Workbench-Dev\.tools\design-qa\blender-navigation-focus.png`
- Source: locally installed Blender 4.5.0, default perspective viewport.

**Rendered implementation**

- Full capture: `E:\Gaussian-Scene-Workbench-Dev\.tools\design-qa\workbench-navigation-implementation-final.png`
- Focus crop: `E:\Gaussian-Scene-Workbench-Dev\.tools\design-qa\workbench-navigation-focus-final.png`
- Combined comparison: `E:\Gaussian-Scene-Workbench-Dev\.tools\design-qa\navigation-gizmo-comparison-final.png`
- Build: packaged native Qt application launched from the installed desktop target.

**Viewport and normalization**

- Blender capture: 1707 x 1019 logical pixels at device scale 1.
- Workbench capture: 1573 x 952 logical pixels at device scale 1; automatic UI scale 90%.
- Blender focus crop: 157 x 240 pixels.
- Workbench focus crop: 144 x 252 pixels.
- Both focus crops were aspect-fit into 240 x 294 panels and combined into a 480 x 294 comparison.
- State: dark perspective viewport, no active hover, no loaded Workbench scene.

**Intentional product constraints**

- The user requested the Workbench control in the lower-right corner. Blender places it in the upper-right, so the navigation button stack is mirrored upward while preserving Blender's axis-outward order: zoom, pan, camera, projection.
- Workbench uses Y-up scene coordinates; Blender uses Z-up. Axis positions therefore follow each application's real coordinate system rather than forcing a misleading label arrangement.
- Blender's implementation is GPL-2.0-or-later while this repository is MIT. The Workbench implementation reproduces the documented visual and interaction behavior without copying Blender's code or binary icon assets.

**Full-view comparison evidence**

- The control remains inside the viewport at the requested lower-right anchor.
- It does not overlap the status badge, dock panels, or persistent viewport controls.
- Positive and negative axis handles remain readable against the grid at the default camera angle.
- The four navigation buttons form a stable vertical stack and preserve the same relative order as Blender.

**Focused-region comparison evidence**

- Axis handles use Blender-style circular caps, bold axis labels, saturated X/Y/Z colors, dimmed rear handles, and depth-dependent scale/color.
- Axis stems terminate beneath the circular caps and use rounded strokes.
- Zoom, pan, camera, and projection affordances are present. The camera icon is intentionally dim when the current scene has no usable camera.
- The Workbench component is slightly denser because it must fit above a lower-right anchor; adaptive scaling prevents clipping at 150% UI scale and at the 420 x 280 minimum viewport.

**Required fidelity surfaces**

- Fonts and typography: bold axis letters scale with the application font and remain centered; negative labels appear only on highlight, matching Blender's low-noise default.
- Spacing and layout rhythm: circular handles, stems, control gaps, and edge margins scale together; minimum-size and 150% scale tests keep the complete group in bounds.
- Colors and visual tokens: X red, Y green, and Z blue preserve Blender's semantic palette; rear axes mix toward the viewport background and active states use white/blue emphasis.
- Image and icon quality: all marks are resolution-independent native Qt rendering with antialiasing; no raster scaling artifacts are visible.
- Copy and content: the component has no persistent visible copy beyond X/Y/Z; Chinese tooltips explain orbit, snap, zoom, pan, camera, and projection actions.

**Findings**

- No actionable P0, P1, or P2 visual differences remain.
- P3: Qt's line joins differ subtly from Blender's GPU glyph rendering at small sizes. This does not affect recognition or interaction.

**Comparison history**

1. Initial comparison found a P1 completeness gap: the Workbench stack had zoom, pan, and projection controls but omitted Blender's camera-view control.
2. Added a camera-view toggle, disabled-state treatment when no scene camera exists, and user-view restoration.
3. Rebuilt, repackaged, and captured the final installed implementation. The focused comparison confirms all four navigation controls are present in Blender-relative order.
4. The earlier responsive test also found the top control exceeding the 420 x 280 viewport at 150% font scale. The entire component now scales to the available height; the regression test passes.

**Implementation checklist**

- [x] Depth-sorted six-axis navigation handles.
- [x] Click-to-snap for positive and negative X/Y/Z.
- [x] Drag-to-orbit interaction.
- [x] Zoom and pan drag controls.
- [x] Scene camera/user view toggle.
- [x] Perspective/orthographic toggle.
- [x] DPI and minimum-viewport adaptation.
- [x] Packaged-install visual verification.

final result: passed
