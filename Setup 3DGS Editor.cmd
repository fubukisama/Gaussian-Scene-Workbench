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
echo Setup finished. Starting 3DGS Editor...
if not exist "%ROOT%3DGS Editor.exe" (
  echo.
  echo 3DGS Editor.exe was not found in this folder:
  echo %ROOT%
  pause
  exit /b 1
)
start "" "%ROOT%3DGS Editor.exe"
timeout /t 2 /nobreak >nul
exit /b 0
