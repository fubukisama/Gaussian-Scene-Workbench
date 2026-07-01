# 3DGS Editor Desktop

Electron desktop wrapper for the local 3DGS Crop Editor.

## Start In Development

From:

```bat
C:\Users\Ishida_Lab\Desktop\3dgs\desktop_app
```

run:

```bat
npm start
```

The app starts the Python crop editor server automatically and opens a desktop window.

## Requirements

- Miniforge installed at `%USERPROFILE%\miniforge3`
- Conda environment named `gaussian_splatting`
- Existing project layout:

```text
3dgs/
  crop_editor/
  output/
  desktop_app/
```

## Package

```bat
npm run package:win
```

Output:

```text
desktop_app\dist\3DGS Editor-win32-x64\3DGS Editor.exe
```
