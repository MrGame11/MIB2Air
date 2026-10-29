@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
set "setup_exit=%errorlevel%"
echo.
pause
exit /b %setup_exit%
