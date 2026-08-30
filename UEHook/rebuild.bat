@echo off
cd /d "%~dp0"
if exist build rmdir /s /q build
cmake -S . -B build -G "Visual Studio 17 2022"
if errorlevel 1 goto fail
cmake --build build --config Release
if errorlevel 1 goto fail
echo UEHook rebuild succeeded
exit /b 0
:fail
echo UEHook rebuild FAILED
pause
exit /b 1
