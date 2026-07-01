@echo off
setlocal

set "ROOT=%~dp0.."
set "COLMAP_BIN=%USERPROFILE%\Downloads\colmap-x64-windows-cuda\bin"
set "VS2022=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"

set "PATH=%COLMAP_BIN%;%PATH%"
call "%VS2022%" -arch=x64 -host_arch=x64
call "%USERPROFILE%\miniforge3\condabin\conda.bat" activate gaussian_splatting

cd /d "%ROOT%"
echo 3DGS shell ready.
cmd /k

