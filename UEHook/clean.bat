@echo off
rem  Delete the build directory so the next build.bat reconfigures from scratch.
setlocal
set "BUILD=%~dp0build"
if exist "%BUILD%" (
    echo [UEHook] Removing "%BUILD%" ...
    rmdir /s /q "%BUILD%"
)
echo [UEHook] Clean.
exit /b 0
