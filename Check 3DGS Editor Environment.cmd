@echo off
setlocal

set "ROOT=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\bootstrap_3dgs_editor.ps1" -CheckOnly -Interactive
echo.
echo Environment check finished. See setup_3dgs_editor.log in this folder.
pause

