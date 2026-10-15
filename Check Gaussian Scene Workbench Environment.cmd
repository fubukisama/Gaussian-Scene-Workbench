@echo off
setlocal

set "ROOT=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\bootstrap_native.ps1" -CheckOnly -Interactive
echo.
echo Environment check finished. See setup_gaussian_scene_workbench.log in this folder.
pause
