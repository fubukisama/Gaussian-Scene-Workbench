# 3DGS Editor 0.1.4 Windows x64

Updated: 2026-07-02

Windows one-click package update:

- Adds selectable runtime install path in `Setup 3DGS Editor.cmd`.
- Defaults runtime installation to the same drive as the extracted package, avoiding the system drive by default.
- Detects same-drive Miniforge/Conda locations such as `E:\miniforge3` and `E:\conda\envs\gaussian_splatting`.
- Updates desktop startup, training helpers, environment checks, and repair scripts to use the selected or detected runtime path.
- Includes adaptive UI scaling for high-DPI displays.

Package SHA256 is provided as a separate `.sha256` asset.
