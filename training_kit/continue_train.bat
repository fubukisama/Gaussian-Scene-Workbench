@echo off
setlocal

if "%~3"=="" (
  echo Usage: continue_train.bat scene_name from_iteration to_iteration
  echo Example: continue_train.bat my_scene 7000 30000
  exit /b 1
)

set "ROOT=%~dp0.."
set "SRC=%ROOT%\gaussian-splatting"
set "SCENE=%~1"
set "FROM_ITER=%~2"
set "TO_ITER=%~3"

if not exist "%ROOT%\output\%SCENE%\point_cloud\iteration_%FROM_ITER%\point_cloud.ply" (
  echo Missing source model:
  echo %ROOT%\output\%SCENE%\point_cloud\iteration_%FROM_ITER%\point_cloud.ply
  exit /b 1
)

call "%~dp0apply_local_fixes.bat"
if errorlevel 1 exit /b %ERRORLEVEL%

cd /d "%SRC%"
call "%~dp0activate_3dgs_env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
"%GAUSSIAN_SPLATTING_CONDA_PREFIX%\python.exe" train.py -s "%ROOT%\datasets\%SCENE%" -m "%ROOT%\output\%SCENE%" -r 8 --data_device cpu --iterations %TO_ITER% --load_iteration %FROM_ITER% --test_iterations %TO_ITER% --save_iterations %TO_ITER% --checkpoint_iterations %TO_ITER%
exit /b %ERRORLEVEL%
