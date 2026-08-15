@echo off
rem  UEEditor build:  build.bat  (Release)  |  build.bat Debug
setlocal
set "ROOT=%~dp0"
set "BUILD=%ROOT%build"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

if not exist "%BUILD%\CMakeCache.txt" (
    cmake -S "%ROOT%." -B "%BUILD%" -G "Visual Studio 17 2022" -A x64 || exit /b 1
)

cmake --build "%BUILD%" --config %CONFIG% || exit /b 1
echo.
echo [UEEditor] Built: %BUILD%\%CONFIG%\UEEditor.dll
