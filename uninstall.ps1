# AcuVoice Roger - uninstaller. Unregisters the COM/voice token and removes the install folder.
param([switch]$NoPause)
$ErrorActionPreference = 'Continue'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host "Must run as Administrator." -ForegroundColor Red; if(-not $NoPause){Read-Host "Press Enter"}; exit 1 }

$dest = "${env:ProgramFiles(x86)}\AcuVoiceRoger"
$dll  = "$dest\AcuRogerSAPI.dll"
$rsvr = "$env:WINDIR\SysWOW64\regsvr32.exe"

if (Test-Path $dll) {
    Write-Host "Unregistering voice..." -ForegroundColor Cyan
    Start-Process $rsvr -ArgumentList '/u','/s',"`"$dll`"" -Wait
}
# Belt-and-suspenders: drop token/CLSID even if the DLL is gone.
Remove-Item 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\AcuVoiceRoger' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item 'HKLM:\SOFTWARE\Classes\WOW6432Node\CLSID\{68E2D748-B030-48AF-BCBD-05D07352F9A7}' -Recurse -Force -ErrorAction SilentlyContinue

if (Test-Path $dest) {
    Write-Host "Removing $dest ..." -ForegroundColor Cyan
    # can't delete the folder we're running from; copy uninstaller to temp if needed
    Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "Uninstall complete." -ForegroundColor Green
if (-not $NoPause) { Read-Host "Press Enter to close" }
