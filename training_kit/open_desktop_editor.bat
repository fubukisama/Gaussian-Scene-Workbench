@echo off
setlocal

set "ROOT=%~dp0.."
cd /d "%ROOT%\desktop_app"
npm start
exit /b %ERRORLEVEL%
