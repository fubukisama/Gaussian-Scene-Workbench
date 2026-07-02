@echo off
setlocal

set "ROOT=%~dp0"
set "DEFAULT_RUNTIME=%~d0\3DGS-Editor-Runtime"
set "INSTALL_ROOT=%~1"

if "%INSTALL_ROOT%"=="" (
  echo.
  echo 3DGS Editor runtime install folder
  echo Default: %DEFAULT_RUNTIME%
  echo.
  echo Press Enter to use the default, or type another folder such as E:\3DGS-Editor-Runtime.
  set /p "INSTALL_ROOT=Runtime folder: "
)

if "%INSTALL_ROOT%"=="" set "INSTALL_ROOT=%DEFAULT_RUNTIME%"

echo.
echo Runtime folder: %INSTALL_ROOT%
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\bootstrap_3dgs_editor.ps1" -NonInteractive -InstallRoot "%INSTALL_ROOT%"
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
