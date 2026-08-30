@echo off
cd /d "%~dp0"
cmake --build build --config Release
if errorlevel 1 goto fail
echo UEHook build succeeded
exit /b 0
:fail
echo UEHook build FAILED
pause
exit /b 1
