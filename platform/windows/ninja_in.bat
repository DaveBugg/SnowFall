@echo off
rem Usage: tools\ninja_in.bat <build-dir> <ninja targets...>   (runs ninja inside the MSVC x64 environment)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "BD=%~1"
shift
set "ARGS="
:loop
if "%~1"=="" goto run
set "ARGS=%ARGS% %1"
shift
goto loop
:run
ninja -C "%BD%" %ARGS%
