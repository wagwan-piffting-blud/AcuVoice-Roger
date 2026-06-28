# AcuVoice "Roger" - in-browser WebAssembly port

This directory runs the original **Fonix AcuVoice "Roger"** text-to-speech engine
(`lib/avcore_acu.dll`, ~1998) **fully in the browser**, with no server and no cloud API.

Rather than re-implementing the engine's ~300 KB linguistic front-end (number/date/abbreviation
expansion, pronunciation dictionary, letter-to-sound rules, prosody) - which would risk subtly
changing how Roger speaks - this port **runs the real 32-bit Windows DLL unmodified** inside a
purpose-built **x86 interpreter compiled to WebAssembly**. Output is **byte-for-byte identical**
to the desktop voice (verified against golden reference WAVs from the native DLL).

## How it works

```
text ─► [ WASM x86 emulator running avcore_acu.dll ]
              ├─ PE loader maps the DLL, runs its C-runtime + DllMain
              ├─ ~70 Win32 API shims (heap, INI, file I/O, ...)
              ├─ file I/O ─► HTTP Range fetches of the soundbank (cached 256 KB blocks)
              └─ calls _txtstr_to_sndbuf(text) ─► 8 kHz µ-law
       ─► µ-law → PCM16 → Web Audio API   (+ .wav download)
```

The 160 MB soundbank is **streamed on demand** via HTTP range requests (GitHub Pages supports them),
so a session pulls only ~1–2 MB - the index tables plus the specific units a sentence uses.
Everything runs in a Web Worker (synchronous range XHR is allowed there).

## Layout

| Path | What |
|------|------|
| `docs/RE-ARCHITECTURE.md` | Reverse-engineering writeup of the engine + port plan |
| `docs/data_format_report.md` | On-disk soundbank / dictionary format report |
| `emu/` | The emulator in portable C (compiles native **and** to WASM) |
| `emu/cpu.c` | 32-bit x86 interpreter (+ partial x87) |
| `emu/loader.c` | PE32 loader (sections, relocations, imports, TEB/PEB) |
| `emu/win32.c` | Win32 import shims + guest heap + import dispatch |
| `emu/vfs.c` / `emu/vfs_wasm.c` | File backends: native disk / browser Range-fetch |
| `emu/host.c`, `emu/wasm_main.c` | Boot + `acu_synth` orchestration; Emscripten exports |
| `emu/main_native.c` | Native test driver (writes a WAV) |
| `native/acu_say.c` | Reference harness: drives the real DLL + traces its file I/O |
| `native/fixtures/` | Golden reference WAVs + I/O traces (the validation oracle) |
| `site/` | The deployed page: `index.html`, `app.js`, `worker.js` (+ built `acu.*`, `data/`) |
| `test/` | Headless-Chrome browser test (puppeteer-core) |

## Build & test (Windows, from PowerShell)

```powershell
# 1) Native reference harness (drives the real DLL → golden WAVs + I/O trace)
cd web/native;  ./build_native.bat;  ./acu_say.exe "Hello." out.wav

# 2) Native emulator - must be byte-exact vs the fixtures
cd ../emu;  ./build_emu.bat;  ./acu_emu.exe "Hello." emu_out.wav

# 3) Compile the emulator to WebAssembly  (-> ../site/acu.{js,wasm,data})
./build_wasm.ps1
node test_wasm.js "one two three" wasm_out.wav      # node smoke test vs real data

# 4) Assemble the local data tree (lowercased) and run the headless browser test
cd ..;  ./prep_data.ps1
npm install puppeteer-core
node test/browser_test.js                            # synthesizes in real Chrome, compares to fixtures

# 5) Serve locally (Range-capable static server)
node test/server.js 8753 site                        # open http://localhost:8753
```

## Deployment

`.github/workflows/wasm.yml` builds the WASM, assembles the lowercased soundbank/dictionaries
from `data/`, and publishes `web/site/` to **GitHub Pages**. Enable Pages → *Source: GitHub Actions*.
Built artifacts (`acu.*`) and the local `site/data/` copy are gitignored; CI regenerates them.

## Fidelity notes

- Authentic Roger pause cadence (`PAUSE1=680 PAUSE2=200 PAUSE3=300 PAUSE4=0` ms) is baked into the
  WASM config (`emu/vfs_wasm.c`).
- Long/dense input is split into ≤200-char chunks (avcore infinite-loops on long expansion-heavy
  text in a single call - the same mitigation the desktop SAPI engine uses).
