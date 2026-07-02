@echo off
set "INSTALL_DRIVE=%~d0"

if not defined GAUSSIAN_SPLATTING_CONDA_PREFIX if exist "%INSTALL_DRIVE%\miniforge3\envs\gaussian_splatting\python.exe" set "GAUSSIAN_SPLATTING_CONDA_PREFIX=%INSTALL_DRIVE%\miniforge3\envs\gaussian_splatting"
if not defined GAUSSIAN_SPLATTING_CONDA_PREFIX if exist "%INSTALL_DRIVE%\conda\envs\gaussian_splatting\python.exe" set "GAUSSIAN_SPLATTING_CONDA_PREFIX=%INSTALL_DRIVE%\conda\envs\gaussian_splatting"
if not defined GAUSSIAN_SPLATTING_CONDA_PREFIX if exist "%INSTALL_DRIVE%\anaconda\envs\gaussian_splatting\python.exe" set "GAUSSIAN_SPLATTING_CONDA_PREFIX=%INSTALL_DRIVE%\anaconda\envs\gaussian_splatting"
if not defined GAUSSIAN_SPLATTING_CONDA_PREFIX if exist "%USERPROFILE%\miniforge3\envs\gaussian_splatting\python.exe" set "GAUSSIAN_SPLATTING_CONDA_PREFIX=%USERPROFILE%\miniforge3\envs\gaussian_splatting"

if not defined CONDA_BAT if exist "%INSTALL_DRIVE%\miniforge3\condabin\conda.bat" set "CONDA_BAT=%INSTALL_DRIVE%\miniforge3\condabin\conda.bat"
if not defined CONDA_BAT if exist "%INSTALL_DRIVE%\anaconda\condabin\conda.bat" set "CONDA_BAT=%INSTALL_DRIVE%\anaconda\condabin\conda.bat"
if not defined CONDA_BAT if exist "%USERPROFILE%\miniforge3\condabin\conda.bat" set "CONDA_BAT=%USERPROFILE%\miniforge3\condabin\conda.bat"
if defined CONDA_BAT for %%I in ("%CONDA_BAT%\..\..") do set "CONDA_ROOT=%%~fI"
if defined CONDA_ROOT set "PATH=%CONDA_ROOT%;%CONDA_ROOT%\Scripts;%CONDA_ROOT%\condabin;%PATH%"

if exist "%GAUSSIAN_SPLATTING_CONDA_PREFIX%\python.exe" (
  set "CONDA_PREFIX=%GAUSSIAN_SPLATTING_CONDA_PREFIX%"
  set "GS_CONDA_PREFIX=%GAUSSIAN_SPLATTING_CONDA_PREFIX%"
  set "PATH=%GAUSSIAN_SPLATTING_CONDA_PREFIX%;%GAUSSIAN_SPLATTING_CONDA_PREFIX%\Library\bin;%GAUSSIAN_SPLATTING_CONDA_PREFIX%\Library\usr\bin;%GAUSSIAN_SPLATTING_CONDA_PREFIX%\Scripts;%PATH%"
  exit /b 0
)

if exist "%CONDA_BAT%" (
  call "%CONDA_BAT%" activate gaussian_splatting
  exit /b %ERRORLEVEL%
)

echo Could not find gaussian_splatting environment.
echo Tried %INSTALL_DRIVE%\conda, %INSTALL_DRIVE%\anaconda, %INSTALL_DRIVE%\miniforge3, and %USERPROFILE%\miniforge3.
exit /b 1
