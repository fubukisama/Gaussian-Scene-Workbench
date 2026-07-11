# Native Feature Parity

This matrix is the release gate for replacing the legacy desktop application. A visible control is not considered implemented until its state, failure behavior, and automated or repeatable acceptance check exist.

Status: `available`, `partial`, `missing`, `hidden`.

| Workflow | Legacy capability | Native status | Native acceptance gate |
| --- | --- | --- | --- |
| Project | Open/save scene-oriented work | partial | Portable `.gsw.json` paths survive moving the project directory; missing assets are reported in the project tree. |
| Dataset | Images and video import, metadata preservation | partial | Folder import works; image/video copy, frame extraction, and import progress remain missing. |
| Reconstruction | COLMAP presets and alignment cache | missing | Typed COLMAP job with validated options, cancellable process tree, persisted log, and recoverable output. |
| Training | 3DGS/2DGS presets, advanced options, resume | partial | Native dialog must call the guarded worker path, prevent output loss, expose progress, and support cancellation/resume. |
| Scene load | PLY metadata, points, soft/real splats, cameras | partial | ASCII and binary little-endian PLY vertices render in the GPU viewport; Gaussian splats and cameras remain missing. |
| Navigation | Orbit, pan, zoom, reset | available | Mouse navigation and scene-bounds camera fit work at supported UI scales. |
| Selection | Rectangle, lasso, brush, visible-only, invert | hidden | Controls stay hidden until ID/depth picking and a persistent selection model pass tests. |
| Editing | Delete, crop, undo/redo, save crop | hidden | Controls stay hidden until edits preserve original Gaussian indices and undo is lossless. |
| Mesh | 2DGS/SuGaR/GS2Mesh/OpenMVS workflows | missing | Typed jobs expose source, options, progress, output validation, and retry. |
| Texture | Bake, preview, trim, OBJ/GLB download | missing | Native asset entry survives reopen and exported files pass format validation. |
| Analysis | PSNR and experiment comparison | missing | Results are persisted and linked to exact scene, iteration, backend, and evaluation settings. |
| Assets | Asset and experiment managers | missing | Imported/generated outputs are searchable, inspectable, and portable within the project. |
| Export | PLY, SPZ, SOG, mesh, texture, GLB | missing | Each exporter validates source compatibility and writes atomically without replacing existing data silently. |
| Tasks | Logs, cancel, retry, open output | partial | One supervised process is available; typed queue, process-tree cancellation, persistence, and retry remain missing. |
| Renderer metrics | Point/splat frame and GPU timing | partial | Point preview reports frame processing time; SIBR metrics appear only after the embedded renderer supplies them. |
| UI scaling | DPI-aware automatic/manual scale | available | 75%-125% settings persist and desktop QA covers 1366x768, 1920x1080, and high-DPI displays. |

## Priority order

1. P0: honest controls, robust project/assets state, typed training/COLMAP worker, real PLY point preview, actionable errors.
2. P1: camera visualization, ID/depth selection, delete/crop/undo, save crop, real Gaussian rendering.
3. P2: image/video import, 2DGS/resume, mesh/texture/export, PSNR, experiments, persistent task queue.

`main` remains the stable legacy release until every P0 and P1 row is `available` and parity regression checks pass.
