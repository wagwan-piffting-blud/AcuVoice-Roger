@echo off
REM Build the 32-bit native reference harness for avcore_acu.dll.
REM Run from PowerShell:  .\build_native.bat
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 ( echo vcvars32 failed & exit /b 1 )
cl /nologo /W3 /O2 /MT acu_say.c /Fe:acu_say.exe user32.lib >nul
if errorlevel 1 ( echo build failed & exit /b 1 )
del acu_say.obj 2>nul
echo built acu_say.exe
