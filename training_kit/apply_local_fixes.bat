@echo off
setlocal

set "ROOT=%~dp0.."
set "SRC=%ROOT%\gaussian-splatting"

if not exist "%SRC%\train.py" (
  echo Could not find gaussian-splatting at:
  echo %SRC%
  exit /b 1
)

powershell -ExecutionPolicy Bypass -File "%~dp0apply_local_fixes.ps1"
exit /b %ERRORLEVEL%

