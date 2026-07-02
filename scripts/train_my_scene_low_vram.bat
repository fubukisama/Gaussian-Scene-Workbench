@echo off
setlocal

set "ROOT=%~dp0.."
set "SRC=%ROOT%\gaussian-splatting"

cd /d "%SRC%"

call "%ROOT%\training_kit\activate_3dgs_env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
"%GAUSSIAN_SPLATTING_CONDA_PREFIX%\python.exe" train.py -s "%ROOT%\datasets\my_scene" -m "%ROOT%\output\my_scene" -r 4 --data_device cpu
