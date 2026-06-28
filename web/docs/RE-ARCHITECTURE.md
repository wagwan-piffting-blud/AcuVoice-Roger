# AcuVoice "Roger" - Engine Architecture (reverse-engineered) & WASM port plan

Reverse-engineering notes for `lib/avcore_acu.dll` (Fonix AcuVoice core, ~1998–99),
the sole synthesizer behind the "Roger" voice, plus the plan to run it unmodified
in the browser via a hand-written x86 interpreter compiled to WebAssembly.

Source of truth: Ghidra analysis of `avcore_acu.dll` (387,584 bytes, the `.acu`-patched
core), the on-disk data tree, and a native IAT-tracing harness (`web/native/acu_say.c`)
whose golden WAVs + I/O traces (`web/native/fixtures/`) are the emulator's oracle.

---

## 1. Engine at a glance

A **concatenative unit-selection TTS**. Two subsystems:

```
   text  ──►  LINGUISTIC FRONT-END  ──►  unit string  ──►  ACOUSTIC BACK-END  ──►  µ-law 8 kHz mono
            (tokenize, normalize,                       (hash unit→(file,off,len),
             dictionary + letter-to-sound,               read µ-law bytes, concat,
             prosody phrasing, phon→syllable)            insert pause silences)
```

- Output format: **G.711 µ-law, 8-bit, 8000 Hz, mono** (dir `Ulaw08Sb` = U-law/08-bit/Soundbank;
  `sndfmt.dat` absent ⇒ this default). The engine can also emit A-law/lin8/lin16 (`get_snd_fmt`).
- The front-end is ~300 KB of the `.text`; the back-end is a few small functions.
- **We do NOT reimplement any of this** - we run the real DLL under emulation, so the entire
  linguistic front-end (number/date/abbreviation expansion, dictionary, LTS, prosody) is exact.

## 2. Public entry points (exports we drive)

| Export (ordinal)            | Addr        | Role |
|-----------------------------|-------------|------|
| `_txtstr_to_sndbuf@16` (46) | `0x1000d430`| **text → µ-law buffer** (our entry). `(LPCSTR text, void** outBuf, unsigned* outLen, BYTE flags)` __stdcall, returns 0 on success. flags bit0=raw, bit1=enable control tags. |
| `_free_sndbuf@4` (15)       | `0x1000da00`| frees `*pBuf` - **takes the ADDRESS of the buffer pointer**. |
| `_txtstr_to_sndfil@12` (47) | `0x1000cdc0`| text → .wav file (writes WAV header). |

`_txtstr_to_sndbuf` flow:
`initialize_SSB(&ssb,text,0)` → `set_tag_flag` → loop while `isSeg_Available(&ssb)`:
`synth_to_buffer(&ssb, workbuf, &chunkLen,...)` appends a chunk → grow output → `close_SSB` →
realloc output to exact size, return `(outBuf,outLen)`.

## 3. Synthesis pipeline (internal)

- **SSB** = "Synthesis State Block", ~0x3f8-byte struct. `initialize_SSB` (`0x1000b0c0`) just
  `malloc`+`strcpy`s the input text into it and zero-inits state; the magic word `ssb[0xfd]=0x29a`
  (byte off 0x3f4) marks "initialized" (checked by `synth_to_buffer`). The text→audio work is **lazy**.
- `synth_to_buffer` (`0x1000b8f0`) → **`FUN_1000bc50`** (the master tokenizer/dispatcher, mode arg=3):
  - tokenizes the SSB text (whitespace via `FUN_1000fe20`/isspace),
  - classifies each token via `FUN_10002d00` → 3 flags (number / alpha / punctuation-special),
  - dispatches to **normalization handlers**: `FUN_10006790` (numbers), `FUN_10005660`,
    `FUN_100051b0`, `FUN_10008e60`, `FUN_10006470`, `FUN_10005a30`, `FUN_10002d70` (spell), ...
    - these expand "8:45"→"eight forty five", dates, abbreviations, symbols, using `Dictfls`,
  - accumulates into a linked-list **token graph** (nodes ~0x34 B: `+4` type code, `+8` subtype,
    `+0x1c` phoneme string, `+0x2c` next, `+0x30` child list),
  - flushes when text is exhausted **or** segment word count > 0x3c (60):
    `FUN_1000c9f0`→`FUN_1000ca60` does **prosody phrasing** (split at `: $ - / , ;` boundaries),
    `FUN_1002f520` builds the syllable/unit string, then `switch(mode)`:
    - 1/2 → `FUN_10020ff0` (wave/raw), **3 → `FUN_10021130` (buffer - our path)**, 4 → phoneme string.
  - Sets done bit `ssb[0xf5] |= 2` (read by `isSeg_Available`, checks `ssb+0x3d4 & 2`) when exhausted.

- **`FUN_10021130`** (synth_to_buffer mode-3 renderer): iterates the unit string with
  `FUN_10018330`, and for each unit calls **`_syl_to_snd@12`**, appending its µ-law bytes.

### 3.1 Acoustic back-end - `_syl_to_snd@12` (`0x10021430`) - fully decoded

1. **Pauses**: the unit name is compared against 4 pause-marker strings
   (`DAT_10053d08/34/30/2c`). On match it emits `PAUSEn` ms of silence ×8 samples/ms:
   PCM16→`0x0000`, µ-law→`0xFF`, lin8→`0x80`. The 4 pause lengths are globals
   `DAT_10054104/10054108/1005410c/10054110` = **PAUSE1..PAUSE4** (loaded from INI, ms).
2. **Units**: opens `<SNDBANK>\hashsnds.ply`, hash-looks-up the unit name, indexes a 16-byte
   record `base + id*0x10` with `length@+10 (u16)`, `offset@+12 (u32 file offset)`, `SetFilePointer`+
   `ReadFile`s the raw bytes. Format byte `DAT_1005c254`: plain µ-law → used as-is;
   `0x0a`→expand to PCM16; `0x1e`→decompress (×400 PCM16 scratch) then re-encode to µ-law.

### 3.2 Soundbank loader - `load_all_hash_table()` / `FUN_10042840` (`0x10042840`)

Lazily (first synth) opens **all five** banks, reading only each one's `uint32 count` + `count×16`
record table into memory: `Hashsnds.ply`→`DAT_1005cb74`, `Hashfon1..4.cmp`→`DAT_1005cb64/6c/68/70`.
Audio blobs are read on-demand during synthesis. **Confirmed by trace: the synth path reads audio
from `hashsnds.ply` AND `hashfon1-4.cmp`** (the bank is chosen by a pitch digit embedded in the unit key).

## 4. On-disk data formats (see `web/docs/data_format_report.md` for full detail)

### Soundbanks `data/Ulaw08Sb/{Hashsnds.ply, Hashfon1..4.cmp}` (little-endian, validated)
```
uint32 record_count                          @ 0x00
record_count × { char key[10];               // NUL-padded ASCII unit label; empty slot = all-zero
                 uint16 length;               // µ-law byte count
                 uint32 offset; }             // ABSOLUTE file offset of the audio
contiguous µ-law blob to EOF
```
65,611 used units, 160 MB total. `Hashfon1..4` = pitch contexts; `Hashsnds` = sentence units.

### Dictionaries `data/Dictfls/`
- `Dict.fle` (1 MB) main pronunciation dict, front-coded (prefix-compressed), `|`-terminated entries;
  `Auxdict1/2.fle` (affixes), `Sufdict1/2/3.fle` (suffix LTS), `Userdict.fle` (overrides).
- Two-level hash index per dict: `*.tab` (bucket→byte-offset, ASCII) + `*.dvl` (double-hash divisors).
- Rule tables: `Prtable.srt`/`ptableex.srt` (number→spoken-form + per-unit sample durations),
  `Tstamp.srt` (per-unit segment durations).
- The DLL does the parsing; we just serve the bytes.

### Config `acuvoice.ini` (read once at DLL load via `GetPrivateProfileStringA`)
```
[AcuVoiceAppDir]   SNDBANK=...\Ulaw08Sb\   TEMPDIR=...\Temp\   DICTFLSDIR=...\Dictfls\   (trailing \ required)
[AcuVoiceSettings] PAUSE1=680  PAUSE2=200  PAUSE3=300  PAUSE4=0   (ms; authentic Roger values)
[AcuVoiceDictionary] CUSTOM=NONE
```

## 5. The I/O contract (what the emulator's Win32 shim must serve)

From the native trace (`web/native/fixtures/*.trace.txt`):
- **At DLL load (DllMain):** `GetPrivateProfileStringA` for the keys above. (Happens before the IAT
  hooks in the native harness, so not in the trace - but known from the INI.)
- **First synth:** `load_all_hash_table` reads dict hash tables (`Dict.fle` bucket reads ~20 KB each)
  + all 5 banks' headers/record tables (~214 KB each). Then per-unit µ-law reads (1–3 KB) scattered
  across `hashsnds.ply` + `hashfon1-4.cmp`, and dict-entry reads from `Dict.fle`/`Userdict.fle`.
- **Win32 imports actually used:** `CreateFileA, ReadFile, SetFilePointer, GetFileSize, CloseHandle,
  GetFileAttributesA, GetPrivateProfileStringA, WritePrivateProfileStringA, Heap*/Local*/Virtual*,
  Enter/Leave/Init/DeleteCriticalSection (single-threaded ⇒ no-op), Tls* (simple slots),
  wsprintfA, MessageBoxA (→ console/log), plus the static-CRT startup imports.`

## 6. WASM port architecture (decision: emulate the real DLL)

Reimplementing the front-end faithfully is ~300 KB of intricate, divergence-prone code; the user
prioritizes authentic Roger output. So we **run avcore_acu.dll unmodified** under a small emulator.
Unicorn was evaluated and **cannot** build to WASM (TCI interpreter stripped from Unicorn 2.x). So:

```
web/emu/
  cpu.{c,h}    32-bit x86 interpreter (flat protected mode; integer 386 + common MSVC6 ops,
               x87 as needed; FS→minimal TEB; unknown-opcode trap drives coverage)
  pe.{c,h}     PE loader: map sections at preferred base 0x10000000, apply .reloc, resolve
               imports to the shim table, set up TEB/PEB/TLS, run DllMain(DLL_PROCESS_ATTACH)
  win32.{c,h}  import shims: bump/arena heap (Heap*/Local*/Virtual*), INI (serve §4 values),
               file I/O → VFS, crit-sections no-op, wsprintf, MessageBox→log
  vfs.{c,h}    pluggable backend:  native test = real disk files;  browser = HTTP Range fetch
  host.c       acu_synth(text, &buf, &len): call _txtstr_to_sndbuf, return µ-law; mirrors the
               proven chunking driver (split ≤200 chars - avcore infinite-loops on long dense text)
  main_native.c  native oracle driver (writes WAV) - develop & validate here, byte-compare vs fixtures
```

Same C compiles to WASM via emcc; only `vfs` backend differs.

**Deployment (GitHub Pages, fully local):** ship the raw data tree as static assets. The browser VFS
issues **synchronous HTTP Range requests** (in a Web Worker) for exactly the `(offset,length)` the DLL
reads - GitHub Pages/Fastly supports ranges. A session streams only ~1–2 MB (index tables + the units
actually used), cached thereafter; no 160 MB download, no data preprocessing. JS converts the returned
µ-law to PCM16 (`g_ulaw` table) for WebAudio playback + WAV download.

## 7. Validation strategy

The native `cpu/pe/win32/vfs` build runs the real DLL+data from disk and must produce **byte-identical**
µ-law to `web/native/fixtures/{hello,numbers,alert}.wav` and an I/O read sequence matching the traces.
Only then port to WASM. Determinism makes this a hard oracle.
