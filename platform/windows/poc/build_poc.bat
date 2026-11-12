@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /std:c++20 /EHsc /O2 /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS overlay_poc.cpp /Fo:out\ /Fe:out\overlay_poc.exe /link /SUBSYSTEM:WINDOWS
