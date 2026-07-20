# Native Feature Parity

This matrix is the release gate for replacing the legacy desktop application. A visible control is not considered implemented until its state, failure behavior, and automated or repeatable acceptance check exist.

Status: `available`, `partial`, `missing`, `hidden`.

| Workflow | Legacy capability | Native status | Native acceptance gate |
| --- | --- | --- | --- |
| Project | Open/save scene-oriented work | partial | Startup and New create an untitled working document without a save prompt. First save chooses any `.gsw.json` location and publishes managed data to the sibling `<name>.files` directory; Save As copies that managed tree, while portable relative paths and linked external sources survive reopen. |
| Dataset | Images and video import, metadata preservation | partial | Metashape-style Add Photos/Add Folder commands select media before any required save setup. Managed import recursively accepts photos/videos into the untitled or saved project, preserves source metadata and originals, extracts frames, reports structured progress, and publishes through a journaled staging transaction that recovers after cancellation, process failure, or reopening a project. Existing `images`/`input` plus `sparse/0` datasets can also be linked without copying. Mask import and detailed per-file counters remain missing. |
| Reconstruction | COLMAP presets and alignment cache | partial | Native standard/robust/sequential jobs validate the executable and dataset, discover the newest versioned non-system-drive install, protect existing caches, stream logs, support process-tree cancellation, and reject incomplete `sparse/0` output. COLMAP 4.1.0 CUDA integration must retain 16/16 registered smoke views plus a complete alignment cache. Persisted logs, resume, and retry remain missing. |
| Training | 3DGS/2DGS presets, advanced options, resume | partial | The first complete native 3DGS path now preflights data/CUDA/extensions, streams iteration progress, supports cancellation, validates the final PLY, preserves partial checkpoints, and auto-loads/saves the result. Advanced options, resume UI, 2DGS parity, and multi-job history remain. |
| Scene load | PLY metadata, points, soft/real splats, cameras | partial | ASCII and binary little-endian points render natively. Standard 3DGS `scale_*`, `rot_*`, and `opacity` fields drive a depth-sorted screen-space Gaussian preview with SH-DC color; higher-order SH and production tile rasterization remain missing. |
| Camera visualization | Standard 3DGS camera poses and capture path | available | Native scene loading walks upward to the nearest `cameras.json`, loads it off the UI thread, refreshes repaired sidecars, and renders camera frustums plus the capture path behind one persistent view toggle. Missing metadata is optional; invalid entries are reported and long trajectories are evenly decimated for display. |
| Navigation | Orbit, pan, zoom, reset | available | Mouse navigation and scene-bounds camera fit work at supported UI scales; `Home` restores the view around the loaded scene center. |
| Reference grid | Infinite world grid and axes | available | A procedural OpenGL grid intersects the fixed Y=0/XZ world plane, adapts its decimal scale while zooming, fades at dense frequencies and the horizon, remains visible after panning beyond the former finite extent, and never enters selection, transforms, or project serialization. |
| Selection | Rectangle, lasso, brush, visible-only, invert | partial | Rectangle, lasso, and configurable 4-256 px continuous brush selection share full-source projection, visible-only depth filtering, clear, invert, add, and subtract behavior; GPU ID picking remains missing. |
| Editing | Delete, crop, undo/redo, save crop | available | Delete history preserves original vertex indices; undo/redo is lossless; cropped ASCII/binary point and Gaussian PLY files retain every source field and are written atomically. |
| Mesh | 2DGS/SuGaR/GS2Mesh/OpenMVS workflows | missing | Typed jobs expose source, options, progress, output validation, and retry. |
| Texture | Bake, preview, trim, OBJ/GLB download | missing | Native asset entry survives reopen and exported files pass format validation. |
| Analysis | PSNR and experiment comparison | missing | Results are persisted and linked to exact scene, iteration, backend, and evaluation settings. |
| Assets | Asset and experiment managers | missing | Imported/generated outputs are searchable, inspectable, and portable within the project. |
| Export | PLY, SPZ, SOG, mesh, texture, GLB | partial | Native cropped PLY export is available for point/Gaussian files and rejects indexed meshes; SPZ, SOG, mesh, texture, and GLB remain missing. |
| Tasks | Logs, cancel, retry, open output | partial | One supervised import, training, or COLMAP process streams logs, parses structured stage/progress events, and supports process-tree cancellation. Exit now requires explicit confirmation, waits for active work to stop, associates the latest valid training checkpoint for the save/discard/cancel decision, and synchronously terminates the complete worker tree through a Windows Job Object with PID fallback. Typed queue persistence, retry, and open-output actions remain missing. |
| Renderer metrics | Point/splat frame and GPU timing | partial | Point and Gaussian modes report CPU submission time without presenting it as FPS. GPU timer queries and SIBR-equivalent renderer metrics remain missing. |
| UI scaling | DPI-aware automatic/manual scale | available | Automatic mode adapts fonts, controls, icons, and viewport overlays to the active screen/window; 90%-150% manual settings persist, common window-resolution presets are bounded to the available desktop, empty workspaces hide scene-only toolbars, and desktop QA covers compact, 1920x1080, and high-DPI layouts. |

## Priority order

1. P0: honest controls, robust project/assets state, typed training/COLMAP worker, real PLY point preview, actionable errors.
2. P1: GPU ID selection, production tile-based Gaussian rendering, and crop-volume tools. Native camera-frustum/capture-path visualization, the screen-space Gaussian preview, rectangle/lasso/brush editing, and lossless cropped PLY export are complete.
3. P2: import masks and richer counters, 2DGS/resume, mesh/texture/export, PSNR, experiments, persistent task queue.

`main` remains the stable legacy release until every P0 and P1 row is `available` and parity regression checks pass.
