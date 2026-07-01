# 3DGS Editor

Windows desktop editor and training helper for 3D Gaussian Splatting workflows.

The project packages a local Electron desktop shell, a browser-based crop/scene editor, COLMAP conversion helpers, and 3DGS training scripts for Windows users.

## Download

The one-click Windows package is published as a GitHub Release asset:

- [3DGS Editor 0.1.0 Windows x64](https://github.com/fubukisama/3DGS-Editor/releases/tag/v0.1.0)
- Asset: `3DGS-Editor-0.1.0-win-x64.zip`
- SHA256: `ABD00460AFC00E65820B389ED65F8ED942B5A99D71F19A47E295AE97D5F52F3C`

The zip is not committed to git because it is larger than GitHub's normal file limit. Use the Release asset for distribution.

## Quick Start

1. Download and extract `3DGS-Editor-0.1.0-win-x64.zip`.
2. Double-click `Setup 3DGS Editor.cmd`.
3. After setup finishes, double-click `3DGS Editor.exe`.

To verify the environment without installing anything, run:

```powershell
.\Check 3DGS Editor Environment.cmd
```

See [README_RELEASE.md](README_RELEASE.md) for English, Chinese, and Japanese release instructions.

## Repository Layout

- `crop_editor/` - local web editor and Python server.
- `resources/app/` - Electron desktop wrapper source.
- `scripts/` - setup, packaging, environment repair, and utility scripts.
- `training_kit/` - Windows batch helpers for conversion and training.
- `gaussian-splatting/` - bundled 3DGS training source and CUDA extension sources.

Generated runtime folders such as `node_modules`, `datasets`, `output`, `desktop_app`, COLMAP downloads, and packaged Electron binaries are intentionally excluded from git.

## Build A Release Package

From a prepared Windows workspace:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_editor_release.ps1
```

The generated zip is intended to be uploaded to GitHub Releases.

## Notes For Capture Data

COLMAP and 3DGS require real camera motion and parallax. Near-duplicate photos from the same viewpoint can match many features but still fail with `No good initial image pair found` because no sparse 3D reconstruction can be initialized.

For small objects or museum exhibits, capture at least 50-100 images while moving around the object with strong overlap. Avoid standing in one place and only rotating the camera.
