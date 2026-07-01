@echo off
setlocal

set "ROOT=%~dp0.."
set "SRC=%ROOT%\gaussian-splatting"
set "MINIFORGE=%USERPROFILE%\miniforge3"

cd /d "%SRC%"

call "%MINIFORGE%\condabin\conda.bat" activate gaussian_splatting
python train.py -s "%ROOT%\datasets\my_scene" -m "%ROOT%\output\my_scene" -r 4 --data_device cpu
