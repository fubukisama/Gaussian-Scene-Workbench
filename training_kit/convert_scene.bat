@echo off
setlocal

if "%~1"=="" (
  echo Usage: convert_scene.bat scene_name
  exit /b 1
)

set "ROOT=%~dp0.."
set "SRC=%ROOT%\gaussian-splatting"
set "SCENE=%~1"
set "COLMAP_BIN=%USERPROFILE%\Downloads\colmap-x64-windows-cuda\bin"

if not exist "%ROOT%\datasets\%SCENE%\images" (
  echo Missing images folder:
  echo %ROOT%\datasets\%SCENE%\images
  exit /b 1
)

set "PATH=%COLMAP_BIN%;%PATH%"
cd /d "%SRC%"
call "%~dp0activate_3dgs_env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
"%GAUSSIAN_SPLATTING_CONDA_PREFIX%\python.exe" convert.py -s "%ROOT%\datasets\%SCENE%"
