@echo off
cd /d "%~dp0"
echo [UEEditor] standalone exe (build)...
cmake --build build --config Release
if errorlevel 1 goto fail
echo [UEEditor] injected DLL (build-dll)...
cmake --build build-dll --config Release
if errorlevel 1 goto fail
echo UEEditor build succeeded
exit /b 0
:fail
echo UEEditor build FAILED
pause
exit /b 1
