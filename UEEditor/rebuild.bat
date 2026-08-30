@echo off
cd /d "%~dp0"
if exist build rmdir /s /q build
if exist build-dll rmdir /s /q build-dll
echo [UEEditor] configuring standalone exe (build)...
cmake -S . -B build -G "Visual Studio 17 2022"
if errorlevel 1 goto fail
echo [UEEditor] configuring injected DLL (build-dll)...
cmake -S . -B build-dll -G "Visual Studio 17 2022" -DUEEDITOR_DLL=ON
if errorlevel 1 goto fail
echo [UEEditor] building standalone exe (build)...
cmake --build build --config Release
if errorlevel 1 goto fail
echo [UEEditor] building injected DLL (build-dll)...
cmake --build build-dll --config Release
if errorlevel 1 goto fail
echo UEEditor rebuild succeeded
exit /b 0
:fail
echo UEEditor rebuild FAILED
pause
exit /b 1
