# AcuVoice "Roger" (Fonix AcuVoice, ~1998) - On-Disk Data Format Report

Target: reimplement the data-file parsers in C / WebAssembly.
Scope: every file under `data\`, plus `lib\` contents. The engine DLL (`avcore_acu.dll`) is analyzed **only** for format-confirming strings - you are handling the code RE separately.
Root: `C:\Program Files (x86)\AcuVoiceRoger\`

All multi-byte integers in the binary soundbank are **little-endian**. Nothing was modified.

---

## 0. Executive summary of the format

The voice is a **concatenative unit-selection synthesizer**. It has two cooperating subsystems:

1. **Text → phoneme/unit string** (the `Dictfls\` directory): a front-coded main pronunciation dictionary plus auxiliary, suffix, and user dictionaries, each paired with a two-level hash index (`*.tab` + `*.dvl`), and rule tables for number/ordinal expansion (`Prtable.srt`, `ptableex.srt`) and per-unit segment durations (`Tstamp.srt`).

2. **Unit string → audio** (the `Ulaw08Sb\` directory): five large soundbank files, each a self-contained **hash table of audio units → contiguous μ-law audio blob**. Four `Hashfon*.cmp` banks hold the same unit inventory at four pitch/intonation contexts; `Hashsnds.ply` holds a separate set (sentence-level / "played" units).

Audio encoding is **μ-law, 8-bit, 8 kHz, mono** (directory name `Ulaw08Sb` = *U-law / 08-bit / Soundbank*). The engine actually supports μ-law / A-law / linear-16 / linear-8 (see DLL exports `_Ulaw_gain@8`, `_Alaw_gain@8`, `_Lin16_gain@8`, `_Lin8_gain@8`, and matching `_ChangeGain*@16`), selected by an optional `sndfmt.dat` ("soundbank format file") - **which is NOT present in this install**, so the μ-law-8k convention from the directory name is in force.

---

## 1. Complete file inventory (exact byte sizes)

### `data\Ulaw08Sb\` - soundbank (the audio)
| File | Bytes | Type |
|---|---:|---|
| `Hashfon1.cmp` | 26,507,561 | binary: hash table + μ-law blob (pitch bank 1) |
| `Hashfon2.cmp` | 30,433,031 | binary: hash table + μ-law blob (pitch bank 2) |
| `Hashfon3.cmp` | 35,025,185 | binary: hash table + μ-law blob (pitch bank 3) |
| `Hashfon4.cmp` | 32,715,059 | binary: hash table + μ-law blob (pitch bank 4) |
| `Hashsnds.ply` | 36,530,299 | binary: hash table + μ-law blob (sentence/"played" units) |

### `data\Dictfls\` - dictionaries, hash indexes, rule tables
| File | Bytes | Type |
|---|---:|---|
| `Dict.fle` | 1,040,036 | binary: front-coded main pronunciation dictionary |
| `Hash.tab` | 40,817 | text: hash bucket → byte-offset index into `Dict.fle` |
| `Hash.dvl` | 56 | text: hash divisor vector for `Dict.fle` |
| `Auxdict1.fle` | 481,428 | binary: front-coded auxiliary dictionary, part 1 |
| `Auxdict2.fle` | 482,720 | binary: front-coded auxiliary dictionary, part 2 |
| `Auxhash1.tab` | 27,902 | text: index into `Auxdict1.fle` |
| `Auxhash1.dvl` | 56 | text: divisor vector for aux dict 1 |
| `Auxhash2.tab` | 28,856 | text: index into `Auxdict2.fle` |
| `Auxhash2.dvl` | 58 | text: divisor vector for aux dict 2 |
| `Sufdict1.fle` | 37,217 | binary: front-coded suffix dictionary, part 1 |
| `Sufdict2.fle` | 12,616 | binary: front-coded suffix dictionary, part 2 |
| `Sufdict3.fle` | 13,053 | binary: front-coded suffix dictionary, part 3 |
| `Sufhash1.tab` | 9,952 | text: index into `Sufdict1.fle` |
| `Sufhash1.dvl` | 50 | text: divisor vector for suffix dict 1 |
| `Sufhash2.tab` | 3,022 | text: index into `Sufdict2.fle` |
| `Sufhash2.dvl` | 49 | text: divisor vector for suffix dict 2 |
| `Sufhash3.tab` | 3,007 | text: index into `Sufdict3.fle` |
| `Sufhash3.dvl` | 49 | text: divisor vector for suffix dict 3 |
| `Userdict.fle` | 1,305 | binary: compiled user dictionary |
| `Userdict.txt` | 1,445 | text: human-readable user dictionary source |
| `userdict.bak` | 1,445 | text: backup of `Userdict.txt` |
| `Userhash.tab` | 757 | text: index into `Userdict.fle` |
| `Userhash.dvl` | 38 | text: divisor vector for user dict |
| `Prtable.srt` | 44,380 | text: number/ordinal expansion rule table (phoneme→token) |
| `ptableex.srt` | 67,030 | text: expanded number table with per-unit durations |
| `Tstamp.srt` | 2,369 | text: per-unit segment-duration ("timestamp") table |

### `data\Dictfls\BackUp\` - backups of the user dictionary
| File | Bytes | Type |
|---|---:|---|
| `userdict.fle` | 1,305 | binary (copy of compiled user dict) |
| `userdict.txt` | 1,445 | text |
| `userhash.dvl` | 38 | text |
| `userhash.tab` | 757 | text |

### `data\Temp\`
| File | Bytes | Type |
|---|---:|---|
| `.gitkeep` | 0 | empty (placeholder; engine scratch dir per `acuvoice.ini TEMPDIR`) |

### `lib\` and `lib\UserDict\`
| File | Bytes | Type / note |
|---|---:|---|
| `avcore_acu.dll` | 387,584 | the 32-bit AcuVoice engine DLL |
| `UserDict\avcore.dll` | 387,584 | **byte-identical** copy (MD5 `ad732dbbdb3d077ab1b0969e1e504dbc` for both) |
| `UserDict\Userdict.exe` | 87,552 | user-dictionary editor GUI (dated 1999-06-29) |
| `UserDict\Userhelp.hlp` | 9,152 | WinHelp file for the editor |
| `UserDict\Userhelp.cnt` | 237 | WinHelp contents file |

> Note: `sndfmt.dat` is referenced by the engine ("Error in reading soundbank format file.") but is **absent** in this install.

---

## 2. Soundbank format - `Hashfon*.cmp` and `Hashsnds.ply`

All five files share one identical layout. Verified exhaustively across all five (see §2.4).

### 2.1 Overall layout
```
offset 0x00000000 : uint32 LE  record_count            (size of hash table, incl. empty buckets)
offset 0x00000004 : record[record_count]               (16 bytes each)  → "hash table"
offset 4 + 16*record_count : μ-law audio blob (contiguous), to EOF
```

The hash table is an **open-addressed table with empty (all-zero) slots**; `record_count` includes empties. Load factor 97–100% (see §2.4).

### 2.2 Record structure (16 bytes, fixed stride 0x10)
| Field | Offset in record | Size | Meaning |
|---|---:|---:|---|
| `key`    | 0  | 10 bytes | ASCII unit key, **NUL-padded**, max 10 chars. Empty slot = all zero. |
| `length` | 10 | uint16 LE | length of this unit's audio in bytes (= μ-law samples) |
| `offset` | 12 | uint32 LE | **absolute** byte offset of the unit's audio **within the file** |

`offset` is absolute from the start of the file (not relative to the blob). Confirmed: the minimum `offset` over all records equals exactly `4 + 16*record_count` (the blob start), and `max(offset+length)` equals exactly the file size.

C struct:
```c
#pragma pack(push,1)
typedef struct {
    char     key[10];     /* NUL-padded ASCII unit key */
    uint16_t length;      /* audio byte count (LE) */
    uint32_t offset;      /* absolute file offset of audio (LE) */
} sb_record_t;            /* sizeof == 16 */
#pragma pack(pop)
```

To fetch a unit's audio: `mulaw_bytes = file[ rec.offset .. rec.offset + rec.length )`.

### 2.3 First-256-byte and last-64-byte hex dumps

**`Hashfon1.cmp`** (count 0x3458 = 13400)
```
0x000000: 58 34 00 00 | 77 6f 27 39 6e 7e 00 00 00 00 | e4 09 | 01 c1 5e 01   key="wo'9n~" len=2532 off=0x015EC101
0x000010: 6e 7e 75 7e 39 6e 00 00 00 00 98 0a fc 8f 7d 01   key="n~u~9n" len=2712 off=0x017D8FFC
0x000020: 73 6b 61 7c 39 6c 00 00 00 00 25 0c 67 cf 93 01   key="ska|9l" len=3109 off=0x0193CF67
0x000044: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   <empty bucket>
...
last 16 bytes are raw μ-law audio (e.g. 0x1947919: db de fc 71 63 62 66 6f ef e2 db da dd df e2 e3)
```
header dword = 0x00003458 (13400). hashtable_end = 4 + 13400*16 = 0x34584. file = 0x1947929.

**`Hashfon2.cmp`** header dword = 0x00003458 (13400). First key `pu~#` (`70 75 7e 23`).
**`Hashfon3.cmp`** header dword = 0x00003390 (13200). First key `me~0` (`6d 65 7e 30`).
**`Hashfon4.cmp`** header dword = 0x00003584 (13700). First key `e'0(r)` (`65 27 30 28 72 29`).
**`Hashsnds.ply`** header dword = 0x000032c8 (13000). First key `Da~6l` (`44 61 7e 36 6c`).

The trailing 64 bytes of every file are pure μ-law audio (high-bit-heavy bytes 0xd0–0xff and 0x60–0x7f, the classic μ-law pattern), confirming the blob runs to EOF with no footer.

### 2.4 Cross-file validation (all five files)
| File | record_count | used | empty | load | hashtable_end | blob bytes | Σlength == blob | min off == blob_start | max(off+len) == size |
|---|---:|---:|---:|---:|---:|---:|:--:|:--:|:--:|
| Hashfon1.cmp | 13400 | 13200 | 200 | 98.5% | 0x34584 | 26,293,157 | yes | yes | yes |
| Hashfon2.cmp | 13400 | 13386 | 14 | 99.9% | 0x34584 | 30,218,627 | yes | yes | yes |
| Hashfon3.cmp | 13200 | 13039 | 161 | 98.8% | 0x33904 | 34,813,981 | yes | yes | yes |
| Hashfon4.cmp | 13700 | 13285 | 415 | 97.0% | 0x35844 | 32,495,855 | yes | yes | yes |
| Hashsnds.ply | 13000 | 12701 | 299 | 97.7% | 0x32C84 | 36,322,295 | yes | yes | yes |

"Σlength == blob" means the sum of all `length` fields over used records exactly equals the audio-region size - i.e. units are packed contiguously with **no padding, no gaps, no alignment**. This is the strongest possible confirmation of the layout.

**Total raw μ-law audio across all five banks = 160,143,615 bytes** (≈152.7 MiB), = ≈ 160,143,615 / 8000 ≈ **5.56 hours of 8 kHz audio**.
Total used units across all five = 13200+13386+13039+13285+12701 = **65,611 units**.

### 2.5 Unit inventory & key notation - these are **demisyllables / polyphones**, not plain diphones
Keys are Fonix AcuVoice phonetic unit labels. Examples (Hashfon1): `wo'9n~`, `n~u~9n`, `ska|9l`, `tsu|9m`, `lyo'2n~`, `bru|9l`, `hwo'9l`. Examples (Hashsnds): `Da~6l`, `wo'5nts`, `Gau2l`, `spe~3k`, `mu0nt's`, `Doi5l`.

Decoded structure of a key:
- A **consonant onset cluster** (may be empty or several consonants: `sk`, `ts`, `hw`, `br`, `spr`, `str`...).
- A **vowel** with optional diacritics: `~` (nasalization/quality), `^` (a quality/r-color), `|` (offglide / r-colored), `'` (primary stress on the syllable).
- An embedded **single digit** = the **pitch/intonation-context index** for that unit. The same phonetic unit recurs with different digits: e.g. `lyo'2n~`, `lyo'3n~`, `lyo'9n~`; `Doi5n`, `Doi6n`, `Doi7n`, `Doi8n`. The four `Hashfon*` banks bias toward distinct digit sets (`Hashfon1`: digits {2,3,9}; `Hashfon4`: {0,1,2,3,4,6,8,9}; `Hashsnds`: full {0..9}), confirming the digit selects a prosodic/pitch variant and the four banks cover four pitch ranges/contexts.
- A **coda** consonant cluster (`n~`, `nts`, `l`, `m`, `k`, `t's`, ...).
- `Hashfon4` and `Hashsnds` keys also contain context markers in parentheses: `e'0(r)`, `Re'0(n)`, `me'0(c~)`, `ske'9(c~)`, `TStu~0)t'` - i.e. the unit was cut in the context of a following phoneme shown in `(...)` or `)`. These are **context-dependent demisyllable units**.

Key-length distribution (chars, used records):
- Hashfon1: {2:15, 3:420, 4:2787, 5:5775, 6:3866, 7:327, 8:10}; longest `ks~o'2n~`.
- Hashfon4: {2:10, 3:280, 4:1800, 5:3920, 6:3832, 7:2703, 8:671, 9:69}; longest `ske'9(c~)`.
- Hashsnds: {2:9, 3:492, 4:3587, 5:4516, 6:3454, 7:536, 8:99, 9:8}; longest `TStu~0)t'`.

Character set seen in keys: lowercase phoneme letters, uppercase macro-phonemes (`D G J K L M N P R S T V W Y Z B C F`), diacritics `' ~ ^ | * # ( ) , < ?`, and digits `0–9`. The full unit alphabet is the same one used in `Dict.fle` transcriptions (§3.3) minus the syllable/stress delimiters.

### 2.6 Addressing / lookup (how the engine finds a unit)
There is **no separate index file** for the soundbanks - the index *is* the in-file hash table at the head of each `.cmp`/`.ply`. The engine forms a unit key string (from the dictionary transcription split into demisyllables), selects the appropriate pitch bank (`Hashfon1..4`) or the `Hashsnds` bank, hashes the key into `[0, record_count)`, probes records (open addressing; empty slot terminates a miss), matches on the 10-byte key, then reads `length` μ-law bytes at absolute `offset`. The exact hash/probe function lives in `avcore_acu.dll` (out of scope here), but the on-disk contract above is complete and sufficient to build the reader: you can also just **linearly scan** all 16-byte records and build your own `{key → (offset,length)}` map in C/WASM, ignoring the original hash entirely.

### 2.7 μ-law decode
Standard ITU G.711 μ-law, 8 kHz mono. Decode each byte to 16-bit PCM with the canonical table (or `_ChangeGainUlaw`/`Ulaw_gain` equivalent). The README's "16 kHz 16-bit" output is the engine's post-processing/resample of this 8 kHz μ-law source.

---

## 3. Main dictionary - `Dict.fle` (+ `Hash.tab`, `Hash.dvl`)

### 3.1 First 256 / last 64 bytes
```
0x000000: 00 8f 33 4a 64 45 35 31 74 45 35 31 69 29 64 7c   ".3JdE51tE51i)d|"
0x000010: 01 00 11 65 0c 33 4a 64 69 29 76 45 35 31 69 29   "...e.3Jdi)vE51i)"
0x000020: 76 00 04 29 6d 27 29 00 04 29 75 7c 29 00 04 29   "v..)m')..)u|)..)"
...
last 64 (...0xfde64): 8a 8b 7a 61 75 25 65 7e 2f 7c 03 00 13 69 67 61   "..zau%e~/|...iga"
            ...     10 69 63 68 8b 02 43 7a 75 7c 25 69 2f 6b 7c 03   ".ich..Czu|%i/k|."
            ...     00 11 6f 74 65 8c 00 7a 69 7e 25 67 6f 7e 2f 74   "..ote..zi~%go~/t"
```

### 3.2 Format: front-coded (prefix-compressed) entry stream
`Dict.fle` is a stream of variable-length entries, each terminated by `'|'` (0x7C) **after the transcription**. Structure of one entry (empirically decoded against the known-plaintext `Userdict.txt`/`Userdict.fle` pair, which uses the same format):

```
[ keep_byte ]          1 byte  : number of leading chars to copy from the PREVIOUS word's key
[ flag bytes ]         ~2 bytes: entry flags / part-of-speech / variant (e.g. 0x8X 0x0X)
[ word_suffix ]        N bytes : the remaining (uncompressed) tail of the headword, printable a–z
[ transcription ]      M bytes : phonetic transcription in the internal alphabet (§3.3)
[ '|' = 0x7C ]         1 byte  : entry terminator
```

Worked sample from `Userdict.fle` (its plaintext is known from `Userdict.txt`, so the codec is unambiguous there and the same codec is used in `Dict.fle`):

```
'|'  8f 00  '(tralopithecus'        → entry for "australopithecus", transcription "o'#stre#lu#pi%t'i+ku/s"  '|'
03 00 11  'nal'                      → "...nal" (keep+suffix), transcription "bu|#no'*l"  '|'
03 00 10  'wan'                      → "...wan", transcription "bu#wo'*n"  '|'
01 00 0b  'u'                        → "...u",   transcription "c~au*"  '|'
04 00 12  'ente'                     → transcription "da~#to*nt"  '|'
```

Notes for the C parser:
- The headword is reconstructed incrementally: `word = prev_word[:keep] + word_suffix`. (The `keep` is taken from the previous **emitted key within the same hash bucket** - see §3.4; this is why a globally-alphabetical reconstruction with simple LCP does **not** line up. Reset the "previous word" to empty at the start of each bucket given by `Hash.tab`.)
- The transcription alphabet in the compiled `.fle` differs slightly from the human `.txt` form: the `.fle` uses `#` `+` `*` `%` as syllable/stress/boundary markers where the `.txt` uses `/` and `\`. (e.g. txt `bu|no'/l` ⇒ fle `bu|#no'*l`; txt `c~au/` ⇒ fle `c~au*`.)
- Control bytes with the high bit set (`0x80+`) appear as flag/length bytes between entries; printable bytes `0x20–0x7E` are word/transcription text; `0x7C '|'` is the terminator.

### 3.3 Phoneme / transcription alphabet (from `Userdict.txt`, the readable form)
`word  transcription  flags` per line, space-separated. Examples:
```
harry        ha/re~        10
motorola     mo~tu|o~/lu   02
mesocyclone  me\so~si~/klo~n  00
ipx          i~\pe~e/ks    02
ya           yi            40
singapore    si/n~gupo|    02D
```
Transcription symbols:
- Vowels/letters: `a e i o u` plus diacritics `~` (tense/long or nasal quality), `^` (r-colored / special quality), `|` (r-colored offglide), e.g. `e~` `o~` `u~` `a~` `i~`, `u|` `o|` `a|`, `e^`.
- `'` = primary stress (precedes/marks the stressed syllable), `/` = syllable boundary (primary), `\` = secondary boundary/stress.
- Consonants are written largely as themselves; digraphs use the same macro letters seen in soundbank keys (`c~`=ch/sh-class, `s~`=sh, `t'`=th, `n~`=ng, `j`, `z~`, etc.).
- Trailing **flags** (2 hex-ish digits, occasionally with a letter suffix like `D`): part-of-speech / homograph / expansion class. Observed values: `00, 02, 10, 40, 02D`. `00` ≈ default/noun, `02`/`10` ≈ alternate POS or "do-not-expand", `40` ≈ function word, trailing `D` a secondary variant marker.

### 3.4 `Hash.tab` - bucket → offset index (ASCII)
Plain text, CRLF line endings. Two whitespace-separated columns: `bucket_id  byte_offset_into_Dict.fle`.
```
2759            <- line 1: standalone number = number of buckets / table size
20555           <- line 2: a second size/parameter (matches Hash.dvl line 2)
14   174
15   67170
16   119791
...
60821  1040018  <- last line; offset ≈ Dict.fle size (1,040,036)
```
2761 lines total. Line 1 (`2759`) and line 2 (`20555`) are header scalars; the remaining lines are `bucket offset` pairs that point at the first entry of each non-empty bucket inside `Dict.fle`. The final offset (1,040,018) is 18 bytes before EOF, i.e. the last entry. **Bucket boundaries are where the front-coding "previous word" resets to empty.**

### 3.5 `Hash.dvl` - divisor vector (ASCII, 56 bytes)
CRLF-separated integers, two logical sections:
```
218
2759            \
2759             |  five copies of the primary table size (2759)
2759             |
2759            /
2759
223             \
368              |
805              |  a descending chain of secondary divisors
1021             |  (multi-level / minimal-perfect-hash parameters)
1156            /
```
First value (218) is a top-level parameter; the run of `2759` is the bucket count (matches `Hash.tab` line 1); the tail (`223, 368, 805, 1021, 1156`) are secondary hash divisors used by the engine's multi-probe hash. Every `*.dvl` follows this shape: 6 copies of the table size, then a 4–5 element divisor chain (see §5).

---

## 4. Auxiliary, suffix, and user dictionaries (same `.fle` codec)

### 4.1 `Auxdict1.fle` / `Auxdict2.fle` (+ `Auxhash{1,2}.tab/.dvl`)
Same front-coded entry format as `Dict.fle`. First bytes of `Auxdict1.fle`:
```
7c 03 00 0f 'erg' 8a 02 'o%bu|/g' 7c 03 00 0e 'hen' 89 02 'o%kn'/' 7c ...
```
These are **affix / inflectional auxiliary forms** (e.g. `...erg`, `...hen`, `...ons`) appended to roots. `Auxhash1.tab` (1902 `bucket offset` lines, last `38025  481401` ≈ file size 481,428) and `Auxhash1.dvl` (table size 1900, divisor chain `24,217,796,1128,1325`) index it; `Auxhash2.*` index `Auxdict2.fle` (table size 1940).

### 4.2 `Sufdict1.fle` / `Sufdict2.fle` / `Sufdict3.fle` (+ `Sufhash{1,2,3}.*`)
Suffix-rule dictionaries. `Sufdict1.fle` head:
```
00 02 8c 'b' 7c 03 00 1b 'her' 8a 02 'bo+ku|/' 'ker' 8a 02 'bo+ku|/' ...
```
Entries map word-final letter sequences (`her`, `hers`, `er`, `ers`, `e`, `es`, `d`...) to their pronounced suffix transcription (`bo+ku|/`, `ba~+ku|/`, ...) - i.e. **letter-to-sound suffix-stripping rules** used when a word is not found whole in `Dict.fle`. `Sufhash1.dvl` table size 741 (chain `121,235,251,307`); `Sufhash2`/`3` table size 239.

### 4.3 `Userdict.fle` / `Userdict.txt` (+ `Userhash.tab/.dvl`) and `BackUp\`
- `Userdict.txt` (1,445 B) is the **editable source**, fully human-readable (69 entries, format `word transcription flags`, §3.3). This is the file the bundled `Userdict.exe` edits.
- `Userdict.fle` (1,305 B) is the **compiled** front-coded form (decoded in §3.2; the codec was reverse-engineered from this exact pair).
- `Userhash.tab` (757 B): header lines `62`, `77`, then `bucket offset` pairs ending `59889 1290` (≈ `Userdict.fle` size).
- `Userhash.dvl` (38 B): `62` ×6 then chain `3, 9, 11, 14`.
- `userdict.bak` is a verbatim copy of `Userdict.txt`. `BackUp\` holds copies of all four user-dict files (lowercase names, identical sizes).

---

## 5. `*.dvl` hash-divisor-vector format (all of them)

Every `.dvl` is ASCII, CRLF-separated decimal integers, layout:
```
[ table_size ] x6        (six identical copies - the bucket count for this dictionary)
[ d1, d2, d3, d4(, d5) ] (4–5 secondary divisors, generally ascending)
```
Observed:
| dvl | table_size (×6) | secondary chain |
|---|---:|---|
| `Hash.dvl` | 2759 (×5) preceded by 218 | 223, 368, 805, 1021, 1156 |
| `Auxhash1.dvl` | 1900 | 24, 217, 796, 1128, 1325 |
| `Auxhash2.dvl` | 1940 | 20 |
| `Sufhash1.dvl` | 741 | 121, 235, 251, 307 |
| `Sufhash2.dvl` | 239 | 67, 102, 239, 239, 135 |
| `Userhash.dvl` | 62 | 3, 9, 11, 14 |

(`Hash.dvl` is the one irregular case: it leads with `218` then five `2759`. Treat the most-repeated value as the table size and the rest as divisors.) These feed the engine's open-addressing / double-hashing probe sequence; the paired `.tab` gives the per-bucket file offsets.

---

## 6. Rule tables (`.srt`, ASCII, CRLF)

### 6.1 `Prtable.srt` - number/ordinal expansion → token map (1848 lines)
Each line: `phoneme_phrase<space>TOKEN`. The phrase is the spoken form (in the `.fle` phoneme alphabet, words separated by `_`), the token is a placeholder of form `bbaNxx` (e.g. `bba0bb`, `bba1bb`, `bba0dd`, `bba1gg`).
```
e~#_le4_vn'#_ bba0bb          ("eleven")
t'u|4_te~#n_ bba1bb           ("thirteen")
twe4n_te~#_ bba8bb            ("twenty")
hu4n_dri#d_ bba6dd            ("hundred")
t'au4_zn'#d_ bba7dd           ("thousand")
mi4l_yn'#_ bba8dd             ("million")
```
The digit after the stress vowel (`le4`, `te~#n`) is the same pitch/duration index used in soundbank keys. Tokens encode the numeric value position (units/teens/tens/scale) the engine substitutes when expanding digit strings.

### 6.2 `ptableex.srt` - expanded number table with durations (1848 lines)
Same rules as `Prtable.srt` but **token-first** and with an explicit **duration in samples** after each unit:
```
bba0bb e~#_673_le4_1272_vn'#_1173
bba1bb t'u|4_1552_te~#n_2403
bba6dd hu4n_1340_dri#d_1390
```
Format: `TOKEN<space>unit_DUR_unit_DUR_..._unit_DUR`. The `_NNN_` integers are per-unit durations (byte/sample counts at 8 kHz; they line up with the `length` magnitudes in the soundbanks). This is the table the synthesizer actually concatenates from when speaking numbers.

### 6.3 `Tstamp.srt` - per-unit segment durations / "timestamps" (131 lines)
`unit_key<space>seg1_seg2_seg3...`:
```
a3 2412
a~4t 1986_1272
DZa0n 260_1160_1912_640
fi~7v 1826_4356_3146
```
Maps a unit key (same notation as soundbank keys) to its internal segment-boundary durations (samples), used for timing/word-tracking and prosody. Small table - appears to be a hand-tuned override/seed set rather than full coverage.

---

## 7. `acuvoice.ini` (engine config - for reference)
```
[AcuVoiceAppDir]
SNDBANK=...\data\Ulaw08Sb\
TEMPDIR=...\data\Temp\
DICTFLSDIR=...\data\Dictfls\
[AcuVoiceSettings]
PAUSE1=680  PAUSE2=200  PAUSE3=300  PAUSE4=0   (inter-phrase pause durations, ms)
[AcuVoiceDictionary]
CUSTOM=NONE
```
The engine string table (in `avcore_acu.dll`) confirms it opens, by lowercase name: `hashfon1..4.cmp`, `hashsnds.ply`, `dict.fle`, `hash.tab/.dvl`, `auxdict1/2.fle`, `auxhash1/2.tab/.dvl`, `sufdict1/2/3.fle`, `sufhash1/2/3.tab/.dvl`, `userdict.fle`, `userhash.tab/.dvl`, `prtable.srt`, `ptableex.srt`, `tstamp.srt`, and the optional `sndfmt.dat`.

---

## 8. Reimplementation checklist (C / WASM)

1. **Soundbank reader** - trivial and the highest-value piece:
   - Read `uint32 count`; loop `count` 16-byte records; skip all-zero (empty) slots; build `map<string,(uint32 offset,uint16 length)>`. Audio = `file[offset .. offset+length)` as G.711 μ-law @ 8 kHz mono. No padding/alignment. You can ignore the original hash entirely and just linear-scan + build your own map.
   - Five banks: `Hashfon1..4.cmp` (pitch contexts 1–4) and `Hashsnds.ply` (sentence units). Pick the bank by the digit in the unit key / prosody target.
2. **Dictionary reader** - front-coded `.fle`: per entry `keep_byte`, flag bytes (high-bit), `word_suffix`, `transcription`, `'|'` terminator; reconstruct `word = prev[:keep] + suffix` with `prev` reset to "" at each `Hash.tab` bucket boundary. Use `Hash.tab` (`bucket→offset`) + `Hash.dvl` (table size + divisor chain) to jump to a bucket, or just stream the whole file building a `{word→transcription}` map (the dicts are small enough: main is 1 MB).
3. **Lookup fallback chain**: main `Dict.fle` → `Auxdict*` (affix forms) → `Sufdict*` (suffix LTS rules) → `Userdict.fle` (user overrides). Then number/ordinal text via `Prtable.srt`/`ptableex.srt`.
4. **Phoneme alphabet**: implement the AcuVoice notation map (§3.3) and split transcriptions into demisyllable unit keys matching the soundbank `key` format (note the `#/+/*/%` vs `/\` distinction between compiled `.fle` and human `.txt`).
5. **`sndfmt.dat` is absent** → hardcode μ-law/8-bit/8 kHz (engine default for the `Ulaw08Sb` directory). If you ever encounter a sibling install with a different `*Sb` dir (e.g. `Lin16...`), switch the decoder accordingly (engine supports μ-law/A-law/lin16/lin8).

---

### Quantitative recap
- Soundbank records (16 B fixed stride): banks total 13400+13400+13200+13700+13000 = **66,700 slots**, **65,611 used units**.
- **Total raw μ-law audio: 160,143,615 bytes (~152.7 MiB, ~5.56 h @ 8 kHz).**
- Dictionaries: 1 main + 2 aux + 3 suffix + 1 user, each with `.tab`+`.dvl` indices; front-coded codec verified against the `Userdict.txt`↔`Userdict.fle` known-plaintext pair.
- Rule tables: number expansion (`Prtable.srt`/`ptableex.srt`, 1848 lines) + per-unit durations (`Tstamp.srt`, 131 lines).
