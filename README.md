# AcuVoice Roger SAPI5 voice

A SAPI5 voice that lets Windows apps speak via the AcuVoice "Roger" TTS voice. Built from a full-fat copy of the AcuVoice "Roger" voice from some obscure children's software I found on eBay (alas, not a joke). This README covers basic installation and usage only.

## Who even is "Roger"?

"Roger" is one of the more obscure voices in the Fonix lineup (with Fonix being the company that acquired the rights to **DECTalk**, a much more well-known engine, later on in their history), only appearing in limited mentions across the Internet and in a few period articles and reviews. This voice is notable for being tested (never implemented, to my knowledge) on the Console Replacement System (CRS)-era NOAA Weather Radio, around 2002, as a possible replacement for DECTalk's "Perfect Paul" voice. Ultimately, however, this was never implemented across NOAA Weather Radio, and the "Roger" voice was quietly shelved and forgotten about (with [Speechify Tom](https://archive.org/details/SpeechifyTom) instead taking the primary position of TTS on CRS). The original "Roger" voice files in this distribution happened to be released as part of a single obscure software package that used Fonix AcuVoice, and the voice was never widely distributed or used in any major products (aside from Fonix iVoice, which is now presumed lost media), making it a rare find for TTS enthusiasts and collectors of obscure TTS voices/synthesizers.

## What you get

- A new SAPI5 voice token named **"AcuVoice Roger"**.
- Usable from any 32-bit SAPI5 client: Balabolka, NVDA, older Office "Speak", and 32-bit PowerShell `(New-Object -ComObject SAPI.SpVoice).Speak("hello")`.
- Per-word tracking data (via SAPI), which means you can do things like highlight words as they're spoken in supported clients.
- Audio quality: 16 kHz mono 16-bit PCM. Same as the original Roger voice on a "proper" AcuVoice install.

## Install

1. Extract this folder anywhere.
2. Run RUN_ME.cmd as administrator (right-click → "Run as administrator").
3. Accept the UAC prompts.
4. Wait for the voice files to copy and the COM DLL to register (should only take a few seconds).
5. Done — voice is now registered and ready for synthesis. No full system reboot required, except for any already running SAPI5 clients to pick up the registry changes.

Files install by default to `C:\Program Files (x86)\AcuVoiceRoger\`.

## Quick test

After install, open Balabolka/TTSApp/your frontend of choice and pick "AcuVoice Roger" in the voice dropdown.

## Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File "C:\Program Files (x86)\AcuVoiceRoger\uninstall.ps1"
```

Removes the registry voice token, unregisters the COM DLL, and deletes the install folder.

## Known limitations

- **32-bit clients only.** The underlying TTS engine (`AvCore_acu.dll`) is 32-bit, so this SAPI shim is 32-bit too. Windows 64-bit registry redirection puts the voice under `HKLM\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\AcuVoiceRoger`, which 64-bit SAPI clients (modern Edge Read-Aloud, Windows Narrator, 64-bit PowerShell) don't see. To use Roger from 64-bit apps, you'd need a 64-bit-to-32-bit COM surrogate setup which isn't included here. Apps confirmed working: Balabolka, TTSApp, 32-bit PowerShell, pretty much any 32-bit SAPI5 client.

## Files

```
data\                            ← voice data files (originally from the AcuVoice install)
lib\AvCore_acu.dll               ← the original AcuVoice TTS engine DLL (32-bit)
lib\UserDict\                    ← the original AcuVoice user dictionary editor (32-bit)
src\                             ← source code for the SAPI shim (not needed for install, but included for reference)
AcuRogerSAPI.dll                 ← the SAPI5 engine shim (~15 KB)
acuvoice.ini                     ← config file read by the shim to find the original voice files (points to AvCore_acu.dll and the voice data folder)
README.md                        ← this file
install.ps1, uninstall.ps1       ← Installer and uninstaller scripts
```

## Removing manually if uninstaller breaks

Delete:
- `C:\Program Files (x86)\AcuVoiceRoger\` (folder)
- `HKLM\SOFTWARE\WOW6432Node\Microsoft\Speech\Voices\Tokens\AcuVoiceRoger` (registry key)
- `HKLM\SOFTWARE\Classes\WOW6432Node\CLSID\{68E2D748-B030-48AF-BCBD-05D07352F9A7}` (registry key)

## License / credits

Most of the SAPI/reverse engineered code was made by Claude Code and was done with some human intervention at times (when Claude wanted to do something wrong or dumb). Original voice files are from a AcuVoice Roger install as part of some obscure preschool learning software. This project is provided as-is, with no warranty, and is intended for educational purposes only. Use at your own risk, but for basic TTS "fun", it should be fine. The original Roger voice is unknown copyright/license but is likely still owned by Fonix/SpeechFX (current name for Fonix) in some regard. The SAPI shim code (and the rest of this repository that is generated by Claude) is licensed under GNU GPL v3.0, see the LICENSE file.

Project spearheaded by Wags (https://wagspuzzle.space/), with _immense_ help from Claude Code (https://claude.ai/) for the SAPI shim, reverse engineering, and overall project structure.

## GenAI Disclosure Notice: Portions of this repository have been generated using Generative AI tools (Claude, Claude Code).
