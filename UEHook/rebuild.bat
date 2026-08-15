@echo off
rem  Clean + build.  Pass a config (default Release):  rebuild.bat Debug
call "%~dp0clean.bat"
call "%~dp0build.bat" %1
exit /b %errorlevel%
