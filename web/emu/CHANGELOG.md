# Changelog — AcuVoice Roger x86 emulator (`web/emu/`)

Tracks fixes to the CPU/loader/Win32-shim layer that backs the WASM
build of `AvCore_acu.dll`. The voice files, SAPI shim, and Emscripten
glue (`web/wasm/`) are out of scope here.

## 2026-06-30 — `vfs_wasm.c`: soundbank folded into `acu.data`

The browser build previously streamed the ~160 MB soundbank from the
server on demand via synchronous HTTP-Range XHRs (a JS block cache in
`worker.js`, delegated to C through `Module.acuFileSize`/`acuFileRead`
in `vfs_wasm.c`). That required the `data/` tree to be deployed
alongside the page *and* the host to honour Range requests — but
`web/site/data/` is gitignored, so a clean deploy had nothing to fetch.

`vfs_wasm.c` now uses plain POSIX stdio (`fopen`/`fseek`/`fread`),
identical to the native `vfs.c` backend, reading from Emscripten's
in-memory FS. `build_wasm.ps1` gained a second `--preload-file` that
bakes the entire lowercased data tree into `acu.data`, so the voice is
a single self-contained ~156 MB bundle: no separate data dir, no Range
dependency. The engine's file requests are lowercase and MEMFS is
case-sensitive, so `prep_data.ps1` (auto-run by the build) supplies the
lowercased names. `worker.js`/`test_wasm.js` dropped their JS file-I/O
callbacks; the app UI dropped the per-session "streamed bytes" counter.

Verified byte-exact: WASM output for
`"one two three, tornado warning until 8:45 PM."` reading from the
bundle is SHA-256-identical to `acu_emu.exe` reading the real disk
files (74 216-byte WAV, `CC7FE890…48086C`).

## 2026-06-30 — `_emu` parity backports

All of the below were authored upstream in
`D:\Programs\Audacity242\Plug-Ins\_emu\` (the VST/LADSPA host that
shares this CPU core) and then proved themselves end-to-end when the
spfy host_emu port booted `SWIttsFe-en-US.dll`. None of them changes
the audio Roger produces on the existing chunk-text path
(`acu_emu.exe "Hello world."` continues to produce the same
1.48 s µ-law output as before), but they fix latent bugs that *would*
have surfaced as Roger's emulator picks up SSE/FPU-heavy DLLs or as
prosody/sample-count code paths start exercising the rounding modes.

### `cpu.c` — three x87 fixes

The interpreter inherited from the early shared-core era had three
silent bugs that only surface in float-heavy DSP. The donor (`_emu`)
hit and fixed them via the `sc4` LADSPA compressor and `dblue_Crusher`
bitcrusher; the same bugs were sitting in Roger's `cpu.c`.

1. **`FSCALE` (D9 FD)** — was `{double s=*st(0); fpop(); *st(0)=ldexp(*st(0),(int)s);}`,
   which both popped `st(0)` and swapped the operand roles. Correct
   semantic is `ST(0) = ST(0) * 2^trunc(ST(1))` with NO pop. Without
   this, `pow`/`exp` are broken and the FPU stack drifts. AcuVoice's
   prosody and concat-cost code use both; the symptom is subtle drift
   rather than an audible crunch, but it's still wrong.

2. **`FXAM` (D9 E5)** — was a no-op (`case 0xE5:return 1;`). MSVC's
   `exp`/`pow` dispatch on `fxam; fnstsw; xlatb; jmp [table]` (a
   computed jump on the classification), so stale flags mis-dispatch
   for certain inputs. Now classifies zero / NaN / inf / denormal /
   normal + sign per the Intel SDM into C0..C3 of `fpu_sw`.

3. **`FIST` / `FISTP` / `FRNDINT` rounding** — all five integer-store
   sites + `FRNDINT` were truncating via `(int32_t)*st(0)` (= C cast,
   = truncate toward zero) or `nearbyint(*st(0))` (= host's RC, not
   guest's). x87 rounds per the control-word RC field, which the guest
   sets to whatever it wants via `FLDCW`. Added `fpu_round_rc()`
   helper (RC = `cw>>10 & 3` → nearest-even / floor / ceil / trunc) and
   threaded it through `case 0xDB reg=2,3`, `case 0xDF reg=2,3,7`, and
   `case D9 0xFC` (FRNDINT). Roger's TTS internally rounds frame /
   sample counts; truncating instead of round-to-nearest accumulates
   sub-frame drift.

### `win32.c` — `g_shim_cleanup` re-entrance fix

The file-global `g_shim_cleanup` was set on `win32_dispatch` entry from
`g_imp[idx].argbytes` and read on exit to advance ESP past the stdcall
args. If a shim itself drives `cpu_run` (e.g. a CRT `_initterm` that
calls global constructors, each of which may dispatch nested imports
like `QueryPerformanceCounter` with `clean=4`), those nested dispatches
overwrite the outer state. The outer cleanup then uses the stale
inner value, ESP comes back misaligned to the outer caller, and the
caller's `ret N` epilogue pops a function argument as the return
address — wild branch, immediate fault.

Roger's current `AvCore_acu.dll` init path doesn't hit this (the DLL
doesn't have a re-entrant `_initterm` chain), but the spfy host_emu
port surfaced it the moment it tried to boot `SWIttsFe-en-US.dll`. Fix
is preventative: snapshot `g_shim_cleanup` locally around the shim
call and restore after. Behaviour-preserving for the existing voice.

### `loader.c` / `emu.h` — `pe_load_mem(bytes, len)`

Public, declared in `emu.h`, used by hosts that have the DLL bytes
embedded as a const blob (the spfy use case) rather than at a file
path. `pe_load(path)` is now a thin `fopen`→`pe_load_mem` wrapper.
Roger's WASM host (`host.c`) already does the same thing internally
via Emscripten's preload; this exposes a portable C entry that future
hosts can use without duplicating the section-mapping logic.

### `win32.c` — `EMU_IATDUMP=1`

New env-gated print inside `win32_register_import` that lists every
import at PE-load time. Run `EMU_IATDUMP=1 .\acu_emu.exe ...` to scope
the shim surface when porting to a new voice DLL.

## Verification

After the backports, `web/emu/build_emu.bat` builds clean (only the
pre-existing `getenv` C4996 warnings) and
`.\acu_emu.exe "Hello world."` produces the same 1.48 s µ-law output
as before (23 712-byte `emu_out.wav`).
