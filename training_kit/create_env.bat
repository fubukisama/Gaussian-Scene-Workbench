@echo off
setlocal

set "ROOT=%~dp0.."
set "SRC=%ROOT%\gaussian-splatting"
set "MINIFORGE=%USERPROFILE%\miniforge3"
set "VS2022=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"

if not exist "%SRC%\environment.yml" (
  echo Missing gaussian-splatting source at %SRC%
  exit /b 1
)

call "%VS2022%" -arch=x64 -host_arch=x64
set "DISTUTILS_USE_SDK=1"
set "MSSdk=1"
set "TORCH_CUDA_ARCH_LIST=8.6"
set "MAX_JOBS=1"

cd /d "%SRC%"

call "%MINIFORGE%\condabin\conda.bat" env list | findstr /C:"gaussian_splatting" >nul
if errorlevel 1 (
  call "%MINIFORGE%\condabin\conda.bat" env create --file environment.yml
)

call "%MINIFORGE%\condabin\conda.bat" activate gaussian_splatting
conda install -y -c defaults pillow=9.4.0 libdeflate libjpeg-turbo zlib
python -m pip install opencv-python==4.8.1.78
python -m pip install --no-user --force-reinstall submodules/diff-gaussian-rasterization submodules/simple-knn submodules/fused-ssim

if exist "%MINIFORGE%\envs\gaussian_splatting\Library\bin\deflate.dll" (
  copy /Y "%MINIFORGE%\envs\gaussian_splatting\Library\bin\deflate.dll" "%MINIFORGE%\envs\gaussian_splatting\Library\bin\libdeflate.dll" >nul
)

python -c "import torch, cv2; from PIL import Image; import diff_gaussian_rasterization, simple_knn, fused_ssim; print('3DGS environment OK')"

