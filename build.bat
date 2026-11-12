@echo off
rem Usage: build.bat [build-dir] [config] [targets...]
rem   build.bat                         -> build\ RelWithDebInfo, all targets
rem   build.bat build-sim Release snow_sim snow_sim_tests
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
set "BDIR=%~1"
if "%BDIR%"=="" set "BDIR=build"
set "CFG=%~2"
if "%CFG%"=="" set "CFG=RelWithDebInfo"
set "TARGETS="
:collect
if "%~3"=="" goto run
set "TARGETS=!TARGETS! %~3"
shift /3
goto collect
:run
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo vcvars64.bat not found & exit /b 1 )
cd /d "%ROOT%"
cmake -S . -B "%BDIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CFG% >"%BDIR%.configure.log" 2>&1
if errorlevel 1 ( type "%BDIR%.configure.log" & exit /b 1 )
if "%TARGETS%"=="" (
  cmake --build "%BDIR%"
) else (
  cmake --build "%BDIR%" --target %TARGETS%
)
exit /b %errorlevel%
