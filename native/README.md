# Gaussian Scene Workbench Native

`native/` is the Qt 6/C++ desktop replacement for the legacy Electron and HTML interface. The native target does not load a browser engine, start a local HTTP server, or require Node.js at runtime.

## Current preview

- Native Qt Widgets application with a GPU-backed OpenGL viewport.
- Dockable project tree, inspector, task queue, and process log.
- Portable `.gsw.json` project files with relative asset paths.
- Dataset folder import plus asynchronous ASCII/binary PLY point-cloud loading.
- Native COLMAP reconstruction dialog with standard, robust, and sequential presets, explicit E-drive executable selection, cache overwrite protection, live logs, cancellation, and sparse-model validation.
- Native OpenGL point rendering plus depth-sorted screen-space Gaussian splats using activated scale, normalized rotation, sigmoid opacity, and SH-DC color.
- Automatic Gaussian/point mode selection, deterministic large-scene sampling, scene-bounds camera fitting, and a manual diagnostic fallback.
- Full-source rectangle/lasso selection plus a persistent 4-256 px continuous brush, with optional visible-point depth filtering, replace/add/subtract, clear, and invert actions.
- Original-index delete history with undo/redo and atomic cropped PLY export that preserves all vertex fields.
- Existing PowerShell/Python backend execution through `QProcess`.
- Qt high-DPI support plus persistent 75%-125% manual UI scaling.

The current viewport includes a native Gaussian preview, but it is not yet a production SIBR/vksplat-class tile rasterizer. It projects each 3D covariance to a 2D EWA ellipse, sorts preview splats by camera depth after navigation, and composites premultiplied alpha. Higher-order view-dependent SH, GPU tile sorting/culling, and GPU timing remain pending. The displayed metric is CPU submission time and is not labeled as FPS.

Rectangle, lasso, and brush selection operate on every source vertex even when display rendering is sampled. The brush shows its exact screen-space radius, persists the chosen size, and uses the same visible-only filter and original-index edit model; GPU ID picking remains pending.

Crop export supports ASCII and binary little-endian point/Gaussian PLY files. It copies retained vertex records without re-encoding custom Gaussian fields, updates the vertex count, writes atomically, and refuses indexed mesh PLY files whose face indices would become invalid.

COLMAP is an external native dependency and is never assumed to live on the system drive. The application checks the saved path, `COLMAP_PATH`/`COLMAP_EXE`, repository-local tool folders, the current drive, and `PATH`; if none exists, the reconstruction dialog requires the user to select `colmap.exe` before a task can start.

Feature parity and release gates are tracked in `docs/NATIVE_PARITY.md` (packaged as `NATIVE_PARITY.md`).

The Windows preview package includes the native worker and compute source needed to launch training. A compatible external Conda/CUDA environment is still required and may be located outside the system drive.

## Local Windows build

The default toolchain is installed outside the system drive:

- Qt/CMake/Ninja: `E:\conda\envs\gsw_native`
- Visual Studio C++ tools: discovered automatically, including `E:\vsi`

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_native.ps1
```

Build and collect a runnable directory:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_native.ps1 -Configuration Release -Package
```

Set `GSW_NATIVE_QT_ROOT` or pass `-QtRoot` when Qt is installed elsewhere.

Generate the deterministic Gaussian renderer QA project:

```powershell
powershell -ExecutionPolicy Bypass -File native\tests\generate_gaussian_visual_fixture.ps1
```

## License boundary

LichtFeld Studio is used as an architecture and workflow reference. Its source is GPL-3.0-or-later. No LichtFeld source code is copied into this MIT-licensed native preview. Any future direct reuse must be isolated and licensed compatibly before it is merged.
