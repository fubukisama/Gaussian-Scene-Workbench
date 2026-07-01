@echo off
setlocal

set "ROOT=%~dp0.."
set "SRC=%ROOT%\gaussian-splatting"
set "MINIFORGE=%USERPROFILE%\miniforge3"
set "COLMAP_BIN=%USERPROFILE%\Downloads\colmap-x64-windows-cuda\bin"

set "PATH=%COLMAP_BIN%;%PATH%"
cd /d "%SRC%"

call "%MINIFORGE%\condabin\conda.bat" activate gaussian_splatting
python convert.py -s "%ROOT%\datasets\my_scene"
