# 3DGS Editor 0.1.0 Windows x64

Updated Windows one-click package with installer fixes:

- Repairs PyTorch 1.12 MKL/OpenMP DLL compatibility during setup.
- Repairs Pillow image DLL dependencies during setup.
- Writes the current extraction path for prebuilt 3DGS CUDA extensions.
- Creates the desktop log directory automatically.
- Increases startup environment-check timeout to avoid false startup failures.
- Shows a clearer COLMAP message when no good initial image pair can be found.

Package SHA256 is provided as a separate `.sha256` asset.
