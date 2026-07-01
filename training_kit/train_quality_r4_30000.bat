@echo off
setlocal

if "%~1"=="" (
  echo Usage: train_quality_r4_30000.bat scene_name
  exit /b 1
)

set "ROOT=%~dp0.."
set "SRC=%ROOT%\gaussian-splatting"
set "SCENE=%~1"

call "%~dp0apply_local_fixes.bat"
if errorlevel 1 exit /b %ERRORLEVEL%

cd /d "%SRC%"
call "%USERPROFILE%\miniforge3\condabin\conda.bat" activate gaussian_splatting
python train.py -s "%ROOT%\datasets\%SCENE%" -m "%ROOT%\output\%SCENE%_r4" -r 4 --data_device cpu --antialiasing --iterations 30000 --test_iterations 30000 --save_iterations 30000 --checkpoint_iterations 30000
exit /b %ERRORLEVEL%
