@echo off
setlocal

set "ROOT=%~dp0.."
set "MINIFORGE=%USERPROFILE%\miniforge3"
set "COLMAP_BIN=%USERPROFILE%\Downloads\colmap-x64-windows-cuda\bin"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%MINIFORGE%" (
  set "PATH=%MINIFORGE%;%MINIFORGE%\Scripts;%MINIFORGE%\condabin;%PATH%"
)

if exist "%COLMAP_BIN%\colmap.exe" (
  set "PATH=%COLMAP_BIN%;%PATH%"
)

set "VSINSTALL="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -requires Microsoft.VisualStudio.Workload.VCTools -property installationPath`) do set "VSINSTALL=%%i"
)

if defined VSINSTALL (
  call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
) else (
  echo Visual Studio C++ Build Tools were not found.
)

cd /d "%ROOT%"
echo.
echo 3DGS shell ready.
echo Workspace: %CD%
echo.
cmd /k
