@echo off
setlocal
if not defined GS_EDITOR_ROOT (
  for %%I in ("%~dp0..\..") do set "GS_EDITOR_ROOT=%%~fI"
)
cd /d "%~dp0"
call npm.cmd start
