@echo off
setlocal

set "ROOT=%~dp0.."
cd /d "%ROOT%"
powershell -ExecutionPolicy Bypass -File "%ROOT%\scripts\check_3dgs_env.ps1"

call "%USERPROFILE%\miniforge3\condabin\conda.bat" run -n gaussian_splatting python -c "import torch, cv2; from PIL import Image; import diff_gaussian_rasterization, simple_knn, fused_ssim; print('torch', torch.__version__, 'cuda', torch.cuda.is_available()); print('3DGS imports OK')"

