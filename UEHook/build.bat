@echo off
rem ===========================================================================
rem  UEHook build script
rem    build.bat            -> Release build (default)
rem    build.bat Debug      -> Debug build
rem  Auto-configures on first run; just rebuilds afterwards.
rem ===========================================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
set "BUILD=%ROOT%build"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

where cmake >nul 2>nul
if errorlevel 1 (
    echo [UEHook] ERROR: cmake was not found on PATH.
    echo          Install CMake or open a "Developer Command Prompt for VS".
    exit /b 1
)

if not exist "%BUILD%\CMakeCache.txt" (
    echo [UEHook] Configuring ^(Visual Studio 17 2022, x64^)...
    cmake -S "%ROOT%." -B "%BUILD%" -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 (
        echo [UEHook] Configure failed.
        exit /b 1
    )
)

echo [UEHook] Building %CONFIG%...
cmake --build "%BUILD%" --config %CONFIG%
if errorlevel 1 (
    echo [UEHook] BUILD FAILED.
    exit /b 1
)

echo.
echo [UEHook] Build succeeded.
echo    Library:  %BUILD%\lib\%CONFIG%\UEHook.lib
echo    Example:  %BUILD%\bin\%CONFIG%\example_mod.dll
exit /b 0
