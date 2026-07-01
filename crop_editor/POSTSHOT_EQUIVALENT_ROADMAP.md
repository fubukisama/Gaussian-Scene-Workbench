# 3DGS Editor Roadmap

This project aims for Postshot-like local 3DGS workflow coverage without copying proprietary UI or implementation.

## Current

- Load trained scenes from `output/<scene>`.
- Edit Gaussian PLY by original Gaussian index.
- Rect/lasso selection.
- Delete selected Gaussians.
- Save cropped model as a new scene.
- Show point preview.
- Show approximate soft splats.
- Show camera positions from `cameras.json`.
- Show real 3DGS rendering through `@mkkellogg/gaussian-splats-3d`.

## Phase 1: Editor Core

- True 3DGS preview inside the editor.
- Selection overlays on top of true splat render.
- Visible-only selection using depth buffer / splat index buffer.
- Undo/redo stack with named operations.
- Keep selected / delete selected.
- Crop box gizmo with move/rotate/scale.
- Plane slicing and height clipping.
- Brush select / erase.
- Save-as and overwrite-safe project handling.

## Phase 2: Training Workflow

- New scene wizard.
- Image/video import.
- Video frame extraction.
- COLMAP tracking controls.
- Training presets for low VRAM / quality / detail.
- Live training log and progress.
- Resume / stop / continue training.
- Dataset quality report: blur, duplicate frames, low-match images.

## Phase 3: Masking And ROI

- Per-image mask import.
- Treat alpha as mask.
- Background removal mask pipeline.
- ROI training from crop box / focus area.
- Re-train or refine inside selected region.

## Phase 4: Viewer And Rendering

- Camera path editor.
- Keyframe animation.
- Export screenshots and videos.
- FOV/focal length controls.
- Orthographic camera.
- Measurement tools.
- Lighting/background/display controls.

## Phase 5: Export And Interop

- Export PLY / SPLAT / KSPLAT / compressed formats.
- SIBR launch button.
- Postshot-compatible import where public formats allow it.
- Unreal/Unity/Blender handoff helpers.

## Technical Notes

- The authoritative editable data is still the Gaussian PLY vertex table.
- Any edit must preserve all fields, including `f_dc_*`, `f_rest_*`, `opacity`, `scale_*`, and `rot_*`.
- Full Postshot-like visual selection requires an ID/depth picking pass. Screen-space lasso without visibility testing will select hidden Gaussians behind the visible surface.
- True rendering uses `@mkkellogg/gaussian-splats-3d` as the first renderer backend. A custom renderer can replace it later if selection buffers or deeper editor integration require it.
