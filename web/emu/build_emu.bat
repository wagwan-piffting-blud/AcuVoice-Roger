@echo off
REM Native build of the emulator test driver (x64 host emulating the 32-bit guest).
REM Run from PowerShell:  .\build_emu.bat
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars64 failed & exit /b 1 )
cl /nologo /W3 /O2 /MT mem.c cpu.c loader.c win32.c vfs.c host.c main_native.c /Fe:acu_emu.exe
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
del *.obj 2>nul
echo built acu_emu.exe
