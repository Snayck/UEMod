@echo off
cd /d "%~dp0"
call "%~dp0UEHook\build.bat"
if errorlevel 1 goto fail
call "%~dp0UEEditor\build.bat"
if errorlevel 1 goto fail
echo All projects built successfully
exit /b 0
:fail
echo Build all FAILED
pause
exit /b 1
