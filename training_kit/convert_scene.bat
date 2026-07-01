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
call "%USERPROFILE%\miniforge3\condabin\conda.bat" activate gaussian_splatting
python convert.py -s "%ROOT%\datasets\%SCENE%"

