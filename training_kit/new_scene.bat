@echo off
setlocal

if "%~1"=="" (
  echo Usage: new_scene.bat scene_name
  exit /b 1
)

set "ROOT=%~dp0.."
set "SCENE=%~1"

mkdir "%ROOT%\datasets\%SCENE%\images" 2>nul
mkdir "%ROOT%\output\%SCENE%" 2>nul

echo Scene created:
echo %ROOT%\datasets\%SCENE%\images
echo.
echo Put your photos in that images folder.

