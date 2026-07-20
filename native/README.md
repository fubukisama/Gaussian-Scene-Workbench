# Gaussian Scene Workbench Native

`native/` is the Qt 6/C++ desktop replacement for the legacy Electron and HTML interface. The native target does not load a browser engine, start a local HTTP server, or require Node.js at runtime.

## Current preview

- Native Qt Widgets application with a GPU-backed OpenGL viewport.
- Dockable project tree, inspector, task queue, and process log.
- Metashape-style untitled projects that can import, reconstruct, and train before the user chooses a save location. First save writes a portable `.gsw.json` document beside a linked `<name>.files` data directory; Save As copies managed data to a newly selected location while external datasets remain linked in place.
- Metashape-style Add Photos/Add Folder entry points that select media first and prefill the import plan without forcing project setup. Managed import includes recursive discovery, metadata manifest, video frame extraction, structured progress, and crash-safe journaled publish/recovery (including project reopen); existing image/COLMAP datasets can also be linked without copying.
- Native COLMAP reconstruction dialog with standard, robust, and sequential presets, automatic newest-version discovery on the application drive, cache overwrite protection, live logs, cancellation, and sparse-model validation.
- Exit confirmation defaults to Cancel, waits for active work to stop, offers to save project/crop progress, associates the latest usable training checkpoint before saving, and uses a Windows Job Object plus PID fallback to synchronously terminate the complete worker process tree before shutdown.
- Native OpenGL point rendering plus depth-sorted screen-space Gaussian splats using activated scale, normalized rotation, sigmoid opacity, and SH-DC color.
- A procedural, world-fixed infinite reference grid on the Y=0/XZ ground plane, with adaptive decimal spacing, anti-aliasing, horizon/distance fading, and colored world axes. It is viewport-only and cannot be selected, transformed, or saved as scene content.
- Optional camera visualization that walks upward from the loaded scene to find a standard 3DGS `cameras.json`, with one camera-trajectory toggle for the frustums and capture path.
- Automatic Gaussian/point mode selection, deterministic large-scene sampling, scene-bounds camera fitting, and a manual diagnostic fallback.
- Full-source rectangle/lasso selection plus a persistent 4-256 px continuous brush, with optional visible-point depth filtering, replace/add/subtract, clear, and invert actions.
- Original-index delete history with undo/redo and atomic cropped PLY export that preserves all vertex fields.
- Existing PowerShell/Python backend execution through `QProcess`.
- Qt high-DPI support with readable 90%-150% manual scaling, automatic screen/window adaptation, common window-resolution presets, and scene-only contextual render/edit toolbars that keep an empty workspace uncluttered.

The current viewport includes a native Gaussian preview, but it is not yet a production SIBR/vksplat-class tile rasterizer. It projects each 3D covariance to a 2D EWA ellipse, sorts preview splats by camera depth after navigation, and composites premultiplied alpha. Higher-order view-dependent SH, GPU tile sorting/culling, and GPU timing remain pending. The displayed metric is CPU submission time and is not labeled as FPS.

Camera metadata is optional. When a scene is loaded, the desktop searches the scene directory and its parents for the nearest standard 3DGS `cameras.json`; a missing sidecar does not prevent the scene from opening, while an unreadable or malformed sidecar is reported to the user. Loading runs outside the UI thread, invalid entries are counted and skipped, and very long trajectories are evenly decimated for display while retaining the full source count. Reopening the same scene refreshes a repaired sidecar. The camera-trajectory control shows or hides the camera frustums and ordered capture path together. Pressing `Home` resets the view around the loaded scene center.

Rectangle, lasso, and brush selection operate on every source vertex even when display rendering is sampled. The brush shows its exact screen-space radius, persists the chosen size, and uses the same visible-only filter and original-index edit model; GPU ID picking remains pending.

Crop export supports ASCII and binary little-endian point/Gaussian PLY files. It copies retained vertex records without re-encoding custom Gaussian fields, updates the vertex count, writes atomically, and refuses indexed mesh PLY files whose face indices would become invalid.

COLMAP is an external native dependency and is never assumed to live on the system drive. The application checks the saved path, `COLMAP_PATH`/`COLMAP_EXE`, repository-local tool folders, the newest semantic version under `<application-drive>:\Tools\COLMAP`, legacy locations, and `PATH`; if none exists, the reconstruction dialog requires the user to select `colmap.exe` before a task can start. Official COLMAP 4.1.0 CUDA has been exercised through the complete native worker pipeline; see `docs/COLMAP_SETUP.md`.

Feature parity and release gates are tracked in `docs/NATIVE_PARITY.md` (packaged as `NATIVE_PARITY.md`).

The Windows preview package includes the native worker and compute source needed to launch training. A compatible external Conda/CUDA environment is still required and may be located outside the system drive. Before 3DGS training starts, the desktop validates the selected dataset, sparse reconstruction, CUDA device, PyTorch, and both native 3DGS extensions. Iteration progress is streamed into the task table. A successful process is accepted only after the requested iteration contains a non-empty Gaussian `point_cloud.ply`; that result is then attached to the project, saved, and loaded into the viewport automatically. Cancelled and failed jobs report the latest usable checkpoint instead of discarding it. Windows Application Control failures are reported explicitly and are never bypassed by the application.

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

Blender's infinite-grid behavior is also used as a visual and algorithmic reference. The native viewport implementation was written independently for this project's Y-up coordinate system; no Blender source or shader code is copied into this MIT-licensed application.
