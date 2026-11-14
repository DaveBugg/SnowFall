@echo off
rem Builds Release and packs the portable x64 build into dist\SnowFall-<ver>-portable-x64.zip
setlocal
set "ROOT=%~dp0..\..\.."
call "%ROOT%\build.bat" build Release SnowFall || exit /b 1
set "OUT=%ROOT%\dist\SnowFall"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"
copy /y "%ROOT%\build\SnowFall.exe" "%OUT%\" >nul
copy /y "%ROOT%\README.md" "%OUT%\" >nul
rem Portable mode: settings next to the exe instead of %LOCALAPPDATA%.
> "%OUT%\Start portable.cmd" echo @start "" "%%~dp0SnowFall.exe" --data-dir "%%~dp0data"
powershell -NoProfile -Command "Compress-Archive -Force -Path '%OUT%\*' -DestinationPath '%ROOT%\dist\SnowFall-0.1.3-portable-x64.zip'"
echo Created dist\SnowFall-0.1.3-portable-x64.zip
rem Code signing (needs a certificate):
rem   signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /a "%OUT%\SnowFall.exe"
