@echo off
setlocal

if "%~8"=="" (
  echo Usage: crop_gaussians_bbox.bat scene_name iteration min_x max_x min_y max_y min_z max_z [output_scene]
  echo Example: crop_gaussians_bbox.bat test1 30000 -1 1 -1 1 -1 1 test1_crop
  exit /b 1
)

set "ROOT=%~dp0.."
set "SCENE=%~1"
set "ITER=%~2"
set "MIN_X=%~3"
set "MAX_X=%~4"
set "MIN_Y=%~5"
set "MAX_Y=%~6"
set "MIN_Z=%~7"
set "MAX_Z=%~8"
set "OUTSCENE=%~9"

if "%OUTSCENE%"=="" set "OUTSCENE=%SCENE%_crop"

set "SRC_MODEL=%ROOT%\output\%SCENE%"
set "DST_MODEL=%ROOT%\output\%OUTSCENE%"
set "SRC_PLY=%SRC_MODEL%\point_cloud\iteration_%ITER%\point_cloud.ply"
set "DST_PLY=%DST_MODEL%\point_cloud\iteration_%ITER%\point_cloud.ply"

if not exist "%SRC_PLY%" (
  echo Missing model:
  echo %SRC_PLY%
  exit /b 1
)

mkdir "%DST_MODEL%" 2>nul
mkdir "%DST_MODEL%\point_cloud\iteration_%ITER%" 2>nul

if exist "%SRC_MODEL%\cfg_args" copy /Y "%SRC_MODEL%\cfg_args" "%DST_MODEL%\cfg_args" >nul
if exist "%SRC_MODEL%\cameras.json" copy /Y "%SRC_MODEL%\cameras.json" "%DST_MODEL%\cameras.json" >nul
if exist "%SRC_MODEL%\input.ply" copy /Y "%SRC_MODEL%\input.ply" "%DST_MODEL%\input.ply" >nul

call "%~dp0activate_3dgs_env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
"%GAUSSIAN_SPLATTING_CONDA_PREFIX%\python.exe" "%~dp0crop_gaussians_bbox.py" --input "%SRC_PLY%" --output "%DST_PLY%" --min-x %MIN_X% --max-x %MAX_X% --min-y %MIN_Y% --max-y %MAX_Y% --min-z %MIN_Z% --max-z %MAX_Z%
if errorlevel 1 exit /b %ERRORLEVEL%

echo.
echo Cropped model:
echo %DST_MODEL%
echo.
echo Open with SIBR:
echo "%%SIBR_ROOT%%\bin\SIBR_gaussianViewer_app.exe" -m "%DST_MODEL%"
exit /b 0
