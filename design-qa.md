# Viewport compact HUD design QA

**Evidence**

- Source visual truth: `C:\Users\ISHIDA~1\AppData\Local\Temp\codex-clipboard-3262f797-ee9d-4870-9aff-71b6ee9c8c1c.png`
- Rendered implementation: `C:\Users\Ishida_Lab\AppData\Local\Temp\gsw-reference-axes-smoke.png`
- Combined comparison: `C:\Users\Ishida_Lab\AppData\Local\Temp\gsw-overlay-design-qa-comparison.png`
- Source pixels: 381 x 689. Implementation pixels: 1350 x 708.
- Comparison viewport: the implementation was height-normalized to 689 px and its leftmost 381 px were compared with the supplied left-edge viewport crop. Native Qt rendering was captured at device density 1.
- State: both captures show a loaded PLY scene in the dark 3D viewport. Scene contents differ, but the compared HUD placement and information hierarchy are equivalent.

**Findings**

- No actionable P0, P1, or P2 differences remain for the requested compact redesign.
- Fonts and typography: the overlay uses the existing application typeface at one step smaller, with a two-level title/stat hierarchy and readable elision.
- Spacing and layout rhythm: the top-left card drops from three rows to two and uses tighter margins; the two bottom-left rows are consolidated into one 22 px minimum-height status pill.
- Colors and visual tokens: existing neutral and teal status colors are preserved with slightly lighter panel opacity, so the redesign remains consistent with the viewport theme.
- Image quality and asset fidelity: no image or icon assets are involved; native text and panel rendering remain sharp in the framebuffer capture.
- Copy and content: all original information remains available with shorter labels (`网格`, `视距`, `精度`, renderer, CPU), and narrow views elide instead of wrapping over the scene.

**Focused region comparison**

- The combined comparison focuses on the left-edge HUD, where all requested changes occur. The navigation gizmo and right-side interaction controls were unchanged, so an additional focused region was not needed.

**Comparison history**

- Pass 1: the rendered implementation reduced the top block by one text row and the bottom block by one full status row. No P0/P1/P2 issue was found, so no corrective visual iteration was required.

**Implementation Checklist**

- [x] Merge project and scene identity into one title row.
- [x] Preserve scene statistics in a second accent row.
- [x] Merge grid, distance, precision, renderer, and CPU metrics into one compact status row.
- [x] Verify truncation, spacing, color hierarchy, and framebuffer rendering.

**Follow-up Polish**

- P3: if future status fields make the bottom row substantially longer, consider a user-controlled detailed/compact HUD toggle instead of adding another persistent row.

final result: passed
