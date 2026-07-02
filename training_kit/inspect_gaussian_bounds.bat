@echo off
setlocal

if "%~2"=="" (
  echo Usage: inspect_gaussian_bounds.bat scene_name iteration
  echo Example: inspect_gaussian_bounds.bat test1 30000
  exit /b 1
)

set "ROOT=%~dp0.."
set "SCENE=%~1"
set "ITER=%~2"
set "PLY=%ROOT%\output\%SCENE%\point_cloud\iteration_%ITER%\point_cloud.ply"

if not exist "%PLY%" (
  echo Missing model:
  echo %PLY%
  exit /b 1
)

call "%~dp0activate_3dgs_env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
"%GAUSSIAN_SPLATTING_CONDA_PREFIX%\python.exe" "%~dp0crop_gaussians_bbox.py" --input "%PLY%" --inspect
exit /b %ERRORLEVEL%
