# AcuVoice Roger - SAPI5 voice installer
#   Installs to:  C:\Program Files (x86)\AcuVoiceRoger\   (fixed: the engine core's
#                 config path is compiled to this location)
#   Registers:    "AcuVoice Roger" as a SAPI5 voice (visible to 32-bit SAPI clients)
#
# Run via RUN_ME.cmd (self-elevates) or:  powershell -ExecutionPolicy Bypass -File install.ps1
param([switch]$NoPause)
$ErrorActionPreference = 'Stop'
try {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) { Write-Host "Must run as Administrator (use RUN_ME.cmd)." -ForegroundColor Red; if(-not $NoPause){Read-Host "Press Enter"}; exit 1 }

    $scriptPath = if ($PSCommandPath) { $PSCommandPath } else { $MyInvocation.MyCommand.Path }
    $pkg  = Split-Path -Parent $scriptPath
    $dest = "${env:ProgramFiles(x86)}\AcuVoiceRoger"
    if ($dest -ne 'C:\Program Files (x86)\AcuVoiceRoger') {
        Write-Host "WARNING: install path is '$dest' but the engine expects 'C:\Program Files (x86)\AcuVoiceRoger'." -ForegroundColor Yellow
    }
    $dll  = "$dest\AcuRogerSAPI.dll"
    $rsvr = "$env:WINDIR\SysWOW64\regsvr32.exe"

    Write-Host "AcuVoice Roger installer" -ForegroundColor Cyan
    Write-Host "  Source: $pkg"
    Write-Host "  Target: $dest`n"

    if (Test-Path $dll) { Write-Host "Unregistering previous install..." -ForegroundColor Yellow; Start-Process $rsvr -ArgumentList '/u','/s',"`"$dll`"" -Wait }

    Write-Host "Copying files (soundbank is ~160 MB, please wait)..." -ForegroundColor Cyan
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    Copy-Item "$pkg\AcuRogerSAPI.dll" $dest
    Copy-Item "$pkg\acuvoice.ini"     $dest
    Copy-Item "$pkg\lib"  $dest -Recurse
    Copy-Item "$pkg\data" $dest -Recurse
    New-Item -ItemType Directory -Path "$dest\data\Temp" -Force | Out-Null

    # The engine core writes a scratch file into TEMPDIR; grant the Users group write there.
    # S-1-5-32-545 = BUILTIN\Users (locale-independent).
    & icacls "$dest\data\Temp" /grant '*S-1-5-32-545:(OI)(CI)M' /Q | Out-Null

    Write-Host "Registering voice with SAPI (32-bit)..." -ForegroundColor Cyan
    Start-Process $rsvr -ArgumentList '/s',"`"$dll`"" -Wait

    $tokenKey = 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\AcuVoiceRoger'
    if (Test-Path $tokenKey) {
        Write-Host "OK: voice token registered." -ForegroundColor Green
        $ps32 = "$env:WINDIR\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
        & $ps32 -NoProfile -Command "foreach(`$v in (New-Object -ComObject SAPI.SpVoice).GetVoices()){ if(`$v.GetAttribute('Name') -eq 'AcuVoice Roger'){ Write-Host '  [OK] AcuVoice Roger visible to SAPI' -ForegroundColor Green } }"
    } else {
        Write-Host "Voice token NOT found - registration may have failed." -ForegroundColor Red
    }

    Copy-Item "$pkg\uninstall.ps1" $dest -ErrorAction SilentlyContinue
    Copy-Item "$pkg\README.md"     $dest -ErrorAction SilentlyContinue
    Write-Host "`nInstall complete. Quick test (32-bit PowerShell):" -ForegroundColor Green
    Write-Host '  C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe -Command "(New-Object -ComObject SAPI.SpVoice).Speak(''Hello from AcuVoice Roger.'')"'
} catch {
    Write-Host "`nERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray
}
if (-not $NoPause) { Read-Host "`nPress Enter to close" }
