@echo off
REM AcuVoice Roger installer launcher - self-elevates to Administrator.
set "DIR=%~dp0"
powershell -NoProfile -Command "Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','%DIR%install.ps1' -Verb RunAs"
