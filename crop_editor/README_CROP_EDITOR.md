# Gaussian Scene Workbench Web Interface

Local editor for trained 3D Gaussian Splatting models.

## Start

From the repository root:

```bat
training_kit\open_crop_editor.bat
```

Then open:

```text
http://127.0.0.1:7860
```

Do not open `index.html` directly.

## Current Features

- Loads trained scenes from `output/<scene>`.
- Loads `point_cloud.ply` from a selected iteration.
- Displays Gaussian center points with fixed screen-size points.
- Optional approximate soft splat preview.
- Optional real 3DGS preview using `@mkkellogg/gaussian-splats-3d`.
- Optional camera trajectory and camera frustums from `cameras.json`.
- 360-degree trackball view control.
- Rectangle selection.
- Lasso selection.
- Delete selected Gaussians.
- Undo deletion.
- Preview unsaved crops in `Points`, `Soft splats`, and `Real 3DGS`.
- Realtime `Real 3DGS` delete/undo masking without reloading the PLY.
- `Preview Real` manually reloads the high-quality real 3DGS renderer from disk when needed.
- Save cropped model as a new output scene.
- Automatically switches to and reloads the cropped output scene after saving.
- Preserves all Gaussian PLY fields when saving.

## Display Modes

- `Points`: fast point preview; best for selection and editing.
- `Soft splats`: approximate splat preview using color, opacity, and scale.
- `Real 3DGS`: true Gaussian splatting preview through the bundled viewer library.
- `Cameras`: training camera positions and path.

## Notes

- Current selection is screen-space based. It can select hidden Gaussians behind visible ones.
- Postshot-like visible-only editing requires an additional depth or ID picking pass.
- `Real 3DGS` uses a GPU edit mask for unsaved delete/undo operations. It should not
  black-screen reload during ordinary edits after the real renderer has loaded once.
- `Preview Real` forces a full real-renderer reload from disk.
- Only `Save` writes a permanent model into `output/`.
- The saved crop remains a valid 3DGS model directory that SIBR can open.

## Open Cropped Model In SIBR

```bat
cd "%SIBR_ROOT%\bin"
SIBR_gaussianViewer_app.exe -m "%GS_EDITOR_WORKSPACE_ROOT%\output\your_crop_scene"
```
