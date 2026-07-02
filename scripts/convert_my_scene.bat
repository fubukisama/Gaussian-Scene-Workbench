@echo off
setlocal

set "ROOT=%~dp0.."
set "SRC=%ROOT%\gaussian-splatting"
set "COLMAP_BIN=%USERPROFILE%\Downloads\colmap-x64-windows-cuda\bin"

set "PATH=%COLMAP_BIN%;%PATH%"
cd /d "%SRC%"

call "%ROOT%\training_kit\activate_3dgs_env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
"%GAUSSIAN_SPLATTING_CONDA_PREFIX%\python.exe" convert.py -s "%ROOT%\datasets\my_scene"
