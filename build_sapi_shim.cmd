@echo off
REM Build AcuRogerSAPI.dll (32-bit) straight to the repo root, no stray copies.
REM Run from anywhere; auto-inits the VS2022 x86 toolset if cl isn't already on PATH.
setlocal
pushd "%~dp0"

where cl >nul 2>nul
if errorlevel 1 for %%E in (Community Professional Enterprise BuildTools) do (
  if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars32.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars32.bat" >nul
  )
)
where cl >nul 2>nul || (echo ERROR: VS2022 x86 toolset not found. Open an "x86 Native Tools Command Prompt for VS" and re-run. & popd & exit /b 1)

cl /nologo /LD /MT /EHsc src\AcuRogerSAPI.cpp /Fe:AcuRogerSAPI.dll /Fo:src\AcuRogerSAPI.obj ^
   /link /DEF:src\AcuRogerSAPI.def sapi.lib ole32.lib advapi32.lib user32.lib uuid.lib

REM keep only the DLL at the repo root; drop import lib / exports / object
del /q AcuRogerSAPI.lib AcuRogerSAPI.exp src\AcuRogerSAPI.obj >nul 2>nul

if exist AcuRogerSAPI.dll (echo Built AcuRogerSAPI.dll) else (echo BUILD FAILED & popd & exit /b 1)
popd
endlocal
