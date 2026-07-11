# Native Feature Parity

This matrix is the release gate for replacing the legacy desktop application. A visible control is not considered implemented until its state, failure behavior, and automated or repeatable acceptance check exist.

Status: `available`, `partial`, `missing`, `hidden`.

| Workflow | Legacy capability | Native status | Native acceptance gate |
| --- | --- | --- | --- |
| Project | Open/save scene-oriented work | partial | Portable `.gsw.json` paths survive moving the project directory; missing assets are reported in the project tree. |
| Dataset | Images and video import, metadata preservation | partial | Folder import works; image/video copy, frame extraction, and import progress remain missing. |
| Reconstruction | COLMAP presets and alignment cache | missing | Typed COLMAP job with validated options, cancellable process tree, persisted log, and recoverable output. |
| Training | 3DGS/2DGS presets, advanced options, resume | partial | Native dialog must call the guarded worker path, prevent output loss, expose progress, and support cancellation/resume. |
| Scene load | PLY metadata, points, soft/real splats, cameras | partial | ASCII and binary little-endian points render natively. Standard 3DGS `scale_*`, `rot_*`, and `opacity` fields drive a depth-sorted screen-space Gaussian preview with SH-DC color; higher-order SH, tile rasterization, and cameras remain missing. |
| Navigation | Orbit, pan, zoom, reset | available | Mouse navigation and scene-bounds camera fit work at supported UI scales. |
| Selection | Rectangle, lasso, brush, visible-only, invert | partial | Rectangle/lasso, visible-only depth filtering, clear, and invert operate on full source coordinates; brush selection and GPU ID picking remain missing. |
| Editing | Delete, crop, undo/redo, save crop | available | Delete history preserves original vertex indices; undo/redo is lossless; cropped ASCII/binary point and Gaussian PLY files retain every source field and are written atomically. |
| Mesh | 2DGS/SuGaR/GS2Mesh/OpenMVS workflows | missing | Typed jobs expose source, options, progress, output validation, and retry. |
| Texture | Bake, preview, trim, OBJ/GLB download | missing | Native asset entry survives reopen and exported files pass format validation. |
| Analysis | PSNR and experiment comparison | missing | Results are persisted and linked to exact scene, iteration, backend, and evaluation settings. |
| Assets | Asset and experiment managers | missing | Imported/generated outputs are searchable, inspectable, and portable within the project. |
| Export | PLY, SPZ, SOG, mesh, texture, GLB | partial | Native cropped PLY export is available for point/Gaussian files and rejects indexed meshes; SPZ, SOG, mesh, texture, and GLB remain missing. |
| Tasks | Logs, cancel, retry, open output | partial | One supervised process is available; typed queue, process-tree cancellation, persistence, and retry remain missing. |
| Renderer metrics | Point/splat frame and GPU timing | partial | Point and Gaussian modes report CPU submission time without presenting it as FPS. GPU timer queries and SIBR-equivalent renderer metrics remain missing. |
| UI scaling | DPI-aware automatic/manual scale | available | 75%-125% settings persist and desktop QA covers 1366x768, 1920x1080, and high-DPI displays. |

## Priority order

1. P0: honest controls, robust project/assets state, typed training/COLMAP worker, real PLY point preview, actionable errors.
2. P1: camera visualization, brush/GPU ID selection, production tile-based Gaussian rendering, and crop-volume tools. The native screen-space Gaussian preview, rectangle/lasso editing, and lossless cropped PLY export are complete.
3. P2: image/video import, 2DGS/resume, mesh/texture/export, PSNR, experiments, persistent task queue.

`main` remains the stable legacy release until every P0 and P1 row is `available` and parity regression checks pass.
