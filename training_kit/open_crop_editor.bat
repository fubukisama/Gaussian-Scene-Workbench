@echo off
setlocal

set "ROOT=%~dp0.."
call "%~dp0activate_3dgs_env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
"%GAUSSIAN_SPLATTING_CONDA_PREFIX%\python.exe" "%ROOT%\crop_editor\server.py"
exit /b %ERRORLEVEL%
