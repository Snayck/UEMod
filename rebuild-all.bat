@echo off
cd /d "%~dp0"
call "%~dp0UEHook\rebuild.bat"
if errorlevel 1 goto fail
call "%~dp0UEEditor\rebuild.bat"
if errorlevel 1 goto fail
echo All projects rebuilt successfully
exit /b 0
:fail
echo Rebuild all FAILED
pause
exit /b 1
