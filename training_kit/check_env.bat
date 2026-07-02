@echo off
setlocal

set "ROOT=%~dp0.."
cd /d "%ROOT%"
powershell -ExecutionPolicy Bypass -File "%ROOT%\scripts\check_3dgs_env.ps1"

call "%~dp0activate_3dgs_env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
"%GAUSSIAN_SPLATTING_CONDA_PREFIX%\python.exe" -c "import torch, cv2; from PIL import Image; import diff_gaussian_rasterization, simple_knn, fused_ssim; print('torch', torch.__version__, 'cuda', torch.cuda.is_available()); print('3DGS imports OK')"
