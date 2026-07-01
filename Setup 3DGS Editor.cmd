@echo off
setlocal

set "ROOT=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\bootstrap_3dgs_editor.ps1" -NonInteractive
if errorlevel 1 (
  echo.
  echo Setup failed. See setup_3dgs_editor.log in this folder.
  pause
  exit /b %errorlevel%
)
echo.
echo Setup finished. You can now run 3DGS Editor.exe.
pause
