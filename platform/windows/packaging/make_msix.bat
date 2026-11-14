@echo off
rem Builds an UNSIGNED MSIX package: dist\SnowFall-0.1.3-x64.msix
rem   packaging\make_msix.bat [build-dir]        (default build dir: build)
rem   set SNOW_EXE=<path\SnowFall.exe> to package a prebuilt exe instead of building it.
rem Signing is a separate, manual step: see packaging\README.md. This script never creates
rem or installs certificates.
setlocal
set "ROOT=%~dp0..\..\.."
set "BDIR=%~1"
if "%BDIR%"=="" set "BDIR=build"
set "VER=0.1.3"

if "%SNOW_EXE%"=="" (
  call "%ROOT%\build.bat" "%BDIR%" Release SnowFall snow_art || exit /b 1
  set "SNOW_EXE=%ROOT%\%BDIR%\SnowFall.exe"
) else (
  call "%ROOT%\build.bat" "%BDIR%" Release snow_art || exit /b 1
)
if not exist "%SNOW_EXE%" ( echo SnowFall.exe not found: %SNOW_EXE% & exit /b 1 )

rem Locate makeappx.exe (newest Windows 10/11 SDK).
set "MAKEAPPX="
for /f "delims=" %%d in ('dir /b /ad /o-n "%ProgramFiles(x86)%\Windows Kits\10\bin\10.*" 2^>nul') do (
  if not defined MAKEAPPX if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\%%d\x64\makeappx.exe" set "MAKEAPPX=%ProgramFiles(x86)%\Windows Kits\10\bin\%%d\x64\makeappx.exe"
)
if not defined MAKEAPPX ( echo makeappx.exe not found - install the Windows 10/11 SDK & exit /b 1 )

set "STAGE=%ROOT%\dist\msix-stage"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\Assets" || exit /b 1
copy /y "%SNOW_EXE%" "%STAGE%\SnowFall.exe" >nul || exit /b 1
copy /y "%ROOT%\platform\windows\packaging\msix\AppxManifest.xml" "%STAGE%\AppxManifest.xml" >nul || exit /b 1
"%ROOT%\%BDIR%\snow_art.exe" --msix "%STAGE%\Assets" || exit /b 1

if not exist "%ROOT%\dist" mkdir "%ROOT%\dist"
set "OUT=%ROOT%\dist\SnowFall-%VER%-x64.msix"
"%MAKEAPPX%" pack /o /h SHA256 /d "%STAGE%" /p "%OUT%" || exit /b 1
echo Created dist\SnowFall-%VER%-x64.msix (UNSIGNED - sign it before installing, see packaging\README.md)
