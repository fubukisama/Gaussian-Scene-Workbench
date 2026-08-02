# Gaussian Scene Workbench Native

`native/` is the Qt 6/C++ desktop replacement for the legacy Electron and HTML interface. The native target does not load a browser engine, start a local HTTP server, or require Node.js at runtime.

## Current preview

- Native Qt Widgets application with a GPU-backed OpenGL viewport.
- Dockable project tree, inspector, task queue, and process log.
- Metashape-style untitled projects that can import, reconstruct, and train before the user chooses a save location. First save writes a portable `.gsw.json` document beside a linked `<name>.files` data directory; Save As copies managed data to a newly selected location while external datasets remain linked in place.
- Metashape-style Add Photos/Add Folder entry points that select media first and prefill the import plan without forcing project setup. Managed import includes recursive discovery, metadata manifest, video frame extraction, structured progress, and crash-safe journaled publish/recovery (including project reopen); existing image/COLMAP datasets can also be linked without copying. Dataset, reconstruction, scene, and task-history cleanup are separate menu and project-tree actions. Managed dataset copies are removed transactionally; reconstruction cleanup preserves photos, the dataset association, and the loaded scene; linked external datasets are never modified; scene cleanup never deletes the PLY/training output.
- Native COLMAP reconstruction dialog with standard, robust, and sequential presets, automatic newest-version discovery on the application drive, cache overwrite protection, live logs, cancellation, and sparse-model validation. COLMAP's native mapper snapshots are sampled into atomic PLY previews so the sparse point cloud grows in the viewport while camera registration is still running.
- Exit confirmation defaults to Cancel, waits for active work to stop, offers to save project/crop progress, associates the latest usable training checkpoint before saving, and uses a Windows Job Object plus PID fallback to synchronously terminate the complete worker process tree before shutdown.
- Native OpenGL point-cloud rendering plus depth-sorted screen-space Gaussian splats using activated scale, normalized rotation, sigmoid opacity, and SH-DC color.
- Live 3DGS training frames use a bounded double-buffered CUDA VMM allocation exported as a Windows KMT object and imported directly by OpenGL. PyTorch packs the current Gaussian attributes on the GPU, CUDA performs only a device-to-device copy into the shared allocation, and OpenGL renders that same physical memory after per-slot event/fence handoff. The same shared frame can be switched between Gaussian splats and center-point rendering to expose densification in real time. Training never waits for the viewport; a busy or unsupported interop path drops the preview frame and retains periodic PLY checkpoints as the fallback.
- A camera-relative infinite reference grid with adaptive decimal spacing, 4x-MSAA line primitives, level-edge fading, and colored axes. Its default reference plane follows the imported model's lower bound so an offset model is not left floating tens or thousands of units above a hard-coded world origin; View > Reference Plane Grid can switch back to the exact world-zero plane. All six axis-aligned orthographic views select the matching screen-parallel XY, XZ, or YZ plane. Grid levels and axes are emitted as real line geometry instead of a periodic fullscreen texture, so shallow perspective lines remain continuous rather than turning into comb-shaped fragments or moire. The colored axes share the major grid's camera-relative center, visible extent, and edge fade, preventing a coplanar axis from appearing to rise past the plane near the horizon. The finite three-axis marker follows the chosen plane, scales with the scene radius, and participates in the scene depth buffer so model geometry hides rear segments. The infinite background grid keeps independent clip depth, so zooming or panning far away does not expose a black cutoff. Presentation is synchronized to the display by default to prevent camera-motion tearing; `GSW_SWAP_INTERVAL=0` is an explicit benchmark-only override. The grid is viewport-only and cannot be selected, transformed, or saved as scene content.
- Optional camera visualization that walks upward from the loaded scene to find a standard 3DGS `cameras.json`, with one camera-trajectory toggle for the frustums and capture path.
- Automatic Gaussian/point/mesh mode selection, scene-bounds camera fitting, and a manual diagnostic fallback. Point-only PLY files above the 10-million-point editable threshold are converted once into a reusable disk-resident depth-3 octree under the source folder's `.gsw-cache`: fixed-width binary input is streamed in bounded blocks, while ASCII input uses an 8 MiB sliding-buffer, allocation-free scalar parser and one compact temporary spool instead of splitting millions of lines. Leaf pages preserve every finite source XYZ/RGB point in an exact 16-byte record, while bounded deterministic reservoirs provide parent LODs. The viewport performs frustum and projected-size selection, reads at most two pages concurrently off the UI thread, keeps the nearest resident parent visible while detail arrives, and evicts least-recently-used GPU pages within an adaptive 512 MiB-2 GiB budget. `GSW_POINT_CACHE_GPU_BUDGET_MB=64..4096` provides a diagnostic override. The source file is never simplified, rewritten, or held wholly in CPU/GPU memory; editing is disabled for this out-of-core path. PLY polygon meshes support ASCII and binary little-/big-endian data and fan-triangulate indexed faces. Meshes above 5 million vertices or 2 million faces build a separate exact triangle cache: a depth-4 spatial hierarchy retains deterministic parent LODs, dense leaves split into pages of at most 65,536 triangles, global area-weighted normals remain smooth across page boundaries, and local page indices avoid expanding every triangle into three unrelated vertices. Frustum/projected-size selection, asynchronous reads, resident-parent fallback, and GPU LRU eviction match the large-point path; `GSW_MESH_CACHE_GPU_BUDGET_MB=64..4096` overrides its budget, while `GSW_MESH_RESIDENT_VERTEX_LIMIT` and `GSW_MESH_RESIDENT_FACE_LIMIT` force the path for diagnostics. The original vertices/faces remain unchanged and every non-degenerate source triangle is retained in leaf pages; this path is read-only. Metashape-style `comment TextureFile` images and per-face-corner `texcoord` UVs now survive both resident and disk-paged paths. Seam-aware indexed vertices preserve distinct corner UVs without expanding every triangle, while background image decoding, vertical texture-space conversion, edge-clamped mipmaps, anisotropic filtering when supported, and a non-fatal vertex-color fallback provide textured rendering. Multi-material and multi-texture PLY conventions remain unsupported.
- Original PLY coordinates are scanned and retained as double-precision bounds independently from compact GPU coordinates. If a coordinate magnitude reaches 100,000 source units, a CloudCompare-style reversible display shift is selected before float conversion and persisted in point/Mesh cache indexes; source files and lossless crop export remain untouched. The inspector exposes source precision, declared unit, CRS, global minimum/maximum/center/size, display shift/scale, and active reference plane. PLY has no mandatory unit/CRS field, so missing metadata is reported honestly as raw unit `u` instead of being silently labeled metres; recognized header metadata and a same-name `.prj` supply available context. Scene > Export Coordinate and Size Report writes JSON or CSV with the original values and `local = (global + shift) * scale` transform.
- Object-level selection works consistently for resident and disk-paged point clouds, triangle meshes, and Gaussian scenes. Clicking the model or its scene-tree entry selects the complete object without moving it. `查找模型` in the scene menu and visible selection toolbar (shortcut `F`) selects the object and frames its transformed full-source bounds from the current viewing direction. Horizontal and vertical FOV, viewport aspect ratio, model depth, projection mode, rotation, scale, and local display shifts all participate in the fit, so elongated or out-of-core models are brought close without clipping. Its dashed world-space bounding box uses the same depth buffer as the model, preserving visible front edges while hiding rear edges instead of floating over the scene. A Blender-style in-viewport strip offers Move, Rotate, Scale, and combined Transform gizmos: direct handles cover three axes, three planes, view-plane movement, axis/view rotation, an internal free trackball, axis/plane scale, and uniform scale. The combined gizmo uses separate constant-screen-space zones for its centre/planes, move arrows, scale blocks, axis rings, and outer view ring; guarded hit priorities prevent a nearby ring or trackball from stealing a direct handle, and the shortcut hint stays outside the gizmo. Move and Rotate expose a real Global/Local orientation toggle. Scale and combined Transform instead show a disabled two-line `局部 / 锁定` state because their non-uniform scale handles must remain on the model basis to preserve the persisted shear-free TRS representation; clicking the control or pressing comma reports the reason without mutating the remembered Move/Rotate preference. Modal `G`, `R`, `R R`, and `S` remain available without a held mouse button. `X`/`Y`/`Z` constrain globally or locally where representable, `Shift` + axis selects a plane, `Ctrl` snaps, `Shift` provides precision, and numeric input is accepted. Left-click/Enter confirms while right-click/Escape restores the exact starting transform. A pivoted TRS matrix is shared by points, meshes, splats, disk-cache culling, point editing, picking, and camera trajectories without rewriting the source PLY; inverse-transpose mesh normals and scale-aware cache bounds preserve non-uniform scaling. Source-unit translation, normalized quaternion rotation, and local XYZ scale survive normal saves, untitled crash recovery, project snapshots, and transform undo/redo. Full-source rectangle/lasso selection remains available alongside a persistent 4-256 px continuous brush, with optional visible-point depth filtering, replace/add/subtract, clear, and invert actions; `Ctrl` + left-drag temporarily orbits from every trim mode.
- Original-index delete history with undo/redo and atomic cropped PLY export that preserves all vertex fields.
- Existing PowerShell/Python backend execution through `QProcess`.
- Qt high-DPI support with readable 90%-150% manual scaling, automatic screen/window adaptation, common window-resolution presets, and scene-only contextual render/edit toolbars that keep an empty workspace uncluttered.

The current viewport includes a native Gaussian preview, but it is not yet a production SIBR/vksplat-class tile rasterizer. It projects each 3D covariance to a 2D EWA ellipse, sorts preview splats by camera depth after navigation, and composites premultiplied alpha. Higher-order view-dependent SH, GPU tile sorting/culling, and GPU timing remain pending. The displayed metric is CPU submission time and is not labeled as FPS.

Camera metadata is optional. When a scene is loaded, the desktop searches the scene directory and its parents for the nearest standard 3DGS `cameras.json`; a missing sidecar does not prevent the scene from opening, while an unreadable or malformed sidecar is reported to the user. Loading runs outside the UI thread, invalid entries are counted and skipped, and very long trajectories are evenly decimated for display while retaining the full source count. Reopening the same scene refreshes a repaired sidecar. The camera-trajectory control shows or hides the camera frustums and ordered capture path together. Pressing `Home` resets the view around the loaded scene center.

Rectangle, lasso, and brush selection operate on every source vertex even when display rendering is sampled. The brush shows its exact screen-space radius, persists the chosen size, and uses the same visible-only filter and original-index edit model; GPU ID picking remains pending.

Object selection keeps the source-accurate bounds only as a broad-phase check, then renders the currently resident point, Gaussian-center, or mesh primitives into a small device-pixel-aware GPU mask around the pointer. Empty volume inside a loose scan AABB therefore no longer selects the model. Selected bounds use subdued corner brackets while idle and expand to the complete dashed box only during an active transform, so the coordinate extent stays truthful without covering the viewport.

Crop export supports ASCII and binary little-/big-endian point/Gaussian PLY files. It copies retained vertex records without re-encoding custom Gaussian fields, updates the vertex count, writes atomically, and refuses indexed mesh PLY files whose face indices would become invalid.

COLMAP is an external native dependency and is never assumed to live on the system drive. The application checks the saved path, `COLMAP_PATH`/`COLMAP_EXE`, repository-local tool folders, the newest semantic version under `<application-drive>:\Tools\COLMAP`, legacy locations, and `PATH`; if none exists, the reconstruction dialog requires the user to select `colmap.exe` before a task can start. Official COLMAP 4.1.0 CUDA has been exercised through the complete native worker pipeline; see `docs/COLMAP_SETUP.md`.

Feature parity and release gates are tracked in `docs/NATIVE_PARITY.md` (packaged as `NATIVE_PARITY.md`).

The Windows preview package includes the native worker, GPU-preview publisher, and compute source needed to launch training. A compatible external Conda/CUDA environment is still required and may be located outside the system drive. Before 3DGS training starts, the desktop validates the selected dataset, sparse reconstruction, CUDA device, PyTorch, and both native 3DGS extensions. Iteration progress is streamed into the task table; on compatible NVIDIA/Windows drivers the viewport also consumes the live shared-GPU-memory frames, otherwise it reports the reason and stays on the checkpoint-based PLY path. A successful process is accepted only after the requested iteration contains a non-empty Gaussian `point_cloud.ply`; that result is then attached to the project, saved, and loaded into the viewport automatically. Cancelled and failed jobs report the latest usable checkpoint instead of discarding it. Windows Application Control failures are reported explicitly and are never bypassed by the application.

Before managed media import starts, the desktop probes the exact selected Python against the packaged `crop_editor/server.py` and requires `numpy` plus `plyfile`. Video import additionally requires at least one verified extraction route: FFmpeg, OpenCV in that Python, or the existing `gaussian_splatting` Conda `video_extract.py` fallback.

Python discovery prefers `GAUSSIAN_SPLATTING_CONDA_PREFIX`, then `GS_CONDA_PREFIX`, an active `gaussian_splatting` Conda environment, and conventional Miniforge/Conda/Anaconda environment locations on the install drive or in the user profile. An arbitrary `python.exe` on `PATH` is not accepted as a Gaussian environment.

The desktop application only auto-discovers a backend beside its executable. Source-tree development can explicitly select another trusted checkout with `GSW_BACKEND_ROOT`; the current working directory is never treated as a backend implicitly.

## Local Windows build

The build script discovers Qt/CMake/Ninja from `GSW_NATIVE_QT_ROOT`, the active Conda environment, common Miniforge/Miniconda locations, and the repository drive. Visual Studio 2022 C++ Build Tools are discovered automatically. Python 3.10 or newer is required for the worker test and staged-source syntax gates; these checks run for normal builds and `-Package` builds.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_native.ps1
```

Build and collect a runnable directory:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_native.ps1 -Configuration Release -Package
```

When an installed package is still running, use `-PackageDirectoryName <safe-name>` to build a side-by-side package without terminating or partially replacing the active session.

Set `GSW_NATIVE_QT_ROOT` or pass `-QtRoot` when Qt is installed elsewhere.

On a managed Windows workstation, configure an organization-approved code-signing certificate before building so the application and native test executables are signed before launch:

```powershell
$env:GSW_WINDOWS_SIGNING_CERTIFICATE_THUMBPRINT = "40_HEX_CHARACTER_THUMBPRINT"
$env:GSW_WINDOWS_SIGNING_CERTIFICATE_STORE_LOCATION = "LocalMachine"
$env:GSW_WINDOWS_SIGNING_TIMESTAMP_URL = "https://organization.example/rfc3161"
$env:GSW_NATIVE_CMAKE_ROOT = "C:\Program Files\CMake"
powershell -ExecutionPolicy Bypass -File scripts\build_native.ps1 -Configuration Release -Package
```

`GSW_NATIVE_CMAKE_ROOT` (or `-CMakeRoot`) selects a CMake distribution containing both `cmake.exe` and `ctest.exe`; this is useful when a managed policy rejects the copy bundled with Conda. The certificate or publisher must also be permitted by the active Windows Application Control policy. The project does not create or trust self-signed certificates and does not disable the policy. See `docs/WINDOWS_APPLICATION_CONTROL.md` for certificate checks, explicit CUDA-extension signing, verification, and the information to send to endpoint management.

Install or verify COLMAP on a non-system drive:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install_colmap.ps1 -InstallRoot E:\Tools\COLMAP -Variant cuda
```

Generate the deterministic Gaussian renderer QA project:

```powershell
powershell -ExecutionPolicy Bypass -File native\tests\generate_gaussian_visual_fixture.ps1
```

Run the complete media-to-model smoke test through the same worker used by the
native desktop application:

```powershell
# Photo import -> COLMAP -> 1000-step 3DGS -> PLY/checkpoint -> native load
powershell -ExecutionPolicy Bypass -File native\tests\run_training_e2e_smoke.ps1

# Video import -> frame extraction -> the same reconstruction/training checks
powershell -ExecutionPolicy Bypass -File native\tests\run_training_e2e_smoke.ps1 -MediaMode Video
```

Each run uses a new directory below `.tools/training-e2e/`. It generates 16
translated multi-depth views, imports them transactionally, validates the
Gaussian PLY fields and checkpoint, and opens the resulting project with the
native executable when a local build is available. Use `-WorkRoot` to choose a
different empty directory; the script never deletes an existing run.

## License boundary

LichtFeld Studio is used as an architecture and workflow reference. Its source is GPL-3.0-or-later. No LichtFeld source code is copied into this MIT-licensed native preview. Any future direct reuse must be isolated and licensed compatibly before it is merged.

Blender's infinite-grid behavior and public line-primitive rendering approach are used as visual and algorithmic references. The native viewport implementation was written independently for this project's Z-up coordinate system and GLSL 1.30 compatibility; no Blender source or shader code is copied into this MIT-licensed application.
