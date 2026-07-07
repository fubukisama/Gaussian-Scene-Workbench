# 3DGS Editor v0.1.10

## Changes

- Renders the pivot center sphere and rings in a separate overlay scene after the model render pass.
- Clears only the depth buffer before drawing the center gizmo so mesh, splat, and textured views cannot occlude it.
- Keeps the existing pivot position tracking and screen-stable size behavior.

## Notes

- Restart the editor after updating so the new render path is loaded.
