@echo off
setlocal

set "ROOT=%~dp0.."
set "SRC=%ROOT%\gaussian-splatting"
if defined MINIFORGE_ROOT (
  set "MINIFORGE=%MINIFORGE_ROOT%"
) else if defined CONDA_ROOT (
  set "MINIFORGE=%CONDA_ROOT%"
) else if exist "%~d0\miniforge3\condabin\conda.bat" (
  set "MINIFORGE=%~d0\miniforge3"
) else (
  set "MINIFORGE=%USERPROFILE%\miniforge3"
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%SRC%\environment.yml" (
  echo Could not find gaussian-splatting source at:
  echo %SRC%
  exit /b 1
)

if not exist "%MINIFORGE%\condabin\conda.bat" (
  echo Could not find Miniforge at:
  echo %MINIFORGE%
  exit /b 1
)

set "VSINSTALL="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -requires Microsoft.VisualStudio.Workload.VCTools -property installationPath`) do set "VSINSTALL=%%i"
)

if defined VSINSTALL (
  call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
) else (
  echo Visual Studio C++ Build Tools were not found.
  exit /b 1
)

cd /d "%SRC%"

call "%MINIFORGE%\condabin\conda.bat" env list | findstr /C:"gaussian_splatting" >nul
if %ERRORLEVEL% EQU 0 (
  echo Conda environment gaussian_splatting already exists.
) else (
  call "%MINIFORGE%\condabin\conda.bat" env create --file environment.yml
  if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
)

call "%MINIFORGE%\condabin\conda.bat" activate gaussian_splatting
python -c "import torch; print('torch', torch.__version__, 'cuda available:', torch.cuda.is_available())"
python -c "import diff_gaussian_rasterization, simple_knn; print('3DGS CUDA extensions import OK')"

echo.
echo 3DGS environment is ready.
