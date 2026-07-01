@echo off
setlocal

set "ROOT=%~dp0.."
call "%USERPROFILE%\miniforge3\condabin\conda.bat" activate gaussian_splatting
python "%ROOT%\crop_editor\server.py"
exit /b %ERRORLEVEL%
