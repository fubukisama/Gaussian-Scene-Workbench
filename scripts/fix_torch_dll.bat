@echo off
setlocal

set "ROOT=%~dp0.."
call "%ROOT%\training_kit\activate_3dgs_env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%

echo Installing a PyTorch 1.12-compatible MKL runtime...
conda install -y -c defaults mkl=2021.4 mkl-service=2.4 intel-openmp=2021.4
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

"%GAUSSIAN_SPLATTING_CONDA_PREFIX%\python.exe" -c "import torch; print('torch', torch.__version__); print('cuda available:', torch.cuda.is_available()); print('cuda:', torch.version.cuda); print('gpu:', torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'none')"
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

"%GAUSSIAN_SPLATTING_CONDA_PREFIX%\python.exe" -c "import diff_gaussian_rasterization, simple_knn; print('3DGS CUDA extensions import OK')"
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

echo.
echo PyTorch DLL issue is fixed.
