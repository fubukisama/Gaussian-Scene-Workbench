# Gaussian Scene Workbench Desktop

Electron desktop application for the local Gaussian scene research workbench.

## Start In Development

From the repository's `resources\app` directory:

```bat
cd resources\app
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
  resources/app/
```

## Package

```bat
npm run package:win
```

Output:

```text
resources\app\dist\Gaussian Scene Workbench-win32-x64\Gaussian Scene Workbench.exe
```
