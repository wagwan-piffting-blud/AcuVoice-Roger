// acu_say.c - native 32-bit reference harness for AcuVoice Roger (avcore_acu.dll)
//
// Two jobs:
//   1) Drive _txtstr_to_sndbuf(text) and write a golden PCM16 WAV (u-law -> PCM16).
//   2) IAT-hook avcore's kernel32 file/INI imports and log every CreateFileA /
//      SetFilePointer / ReadFile / GetFileSize / CloseHandle / GetPrivateProfileStringA.
//      That trace is the exact contract the WASM Win32 shim must reproduce, and it
//      tells us definitively which data files (and byte ranges) the synth path reads.
//
// Build (PowerShell, from this dir):  .\build_native.bat
// Run:  .\acu_say.exe "Tornado warning for your county." out.wav  > trace.txt
//
// 32-bit ONLY (avcore is 32-bit). The DLL reads its config from the absolute path
// baked into its .acu section: C:\Program Files (x86)\AcuVoiceRoger\acuvoice.ini,
// whose SNDBANK/DICTFLSDIR point at the installed data tree.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// ---- avcore exports (stdcall, decorated) ----
typedef unsigned (__stdcall *txtstr_to_sndbuf_t)(const char* text, void** outBuf, unsigned* outLen, unsigned char flags);
typedef unsigned (__stdcall *free_sndbuf_t)(void** pBuf);

// ---------------- u-law -> PCM16 ----------------
static short g_ulaw[256];
static void build_ulaw(void) {
    for (int i = 0; i < 256; i++) {
        int u = (~i) & 0xFF;
        int sign = u & 0x80, exp = (u >> 4) & 7, man = u & 0x0F;
        int s = (((man << 3) + 0x84) << exp) - 0x84;
        g_ulaw[i] = (short)(sign ? -s : s);
    }
}

// ---------------- handle -> name map for readable trace ----------------
#define MAXH 16384
static struct { HANDLE h; char name[260]; LONGLONG pos; } g_h[MAXH];
static int g_hn = 0;
static FILE* g_log = NULL;
static void logf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    if (g_log) { va_list ap2; va_start(ap2, fmt); vfprintf(g_log, fmt, ap2); va_end(ap2); }
    va_end(ap);
}
// Search newest-first and skip closed (h==NULL) slots - handle values get reused.
static const char* name_of(HANDLE h) {
    for (int i = g_hn - 1; i >= 0; i--) if (g_h[i].h == h) return g_h[i].name;
    return "?";
}
static void set_pos(HANDLE h, LONGLONG p) {
    for (int i = g_hn - 1; i >= 0; i--) if (g_h[i].h == h) { g_h[i].pos = p; return; }
}
static LONGLONG get_pos(HANDLE h) {
    for (int i = g_hn - 1; i >= 0; i--) if (g_h[i].h == h) return g_h[i].pos;
    return -1;
}
static void forget_handle(HANDLE h) {
    for (int i = g_hn - 1; i >= 0; i--) if (g_h[i].h == h) { g_h[i].h = NULL; return; }
}

// ---------------- import trampolines ----------------
static HANDLE (WINAPI *real_CreateFileA)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static BOOL   (WINAPI *real_ReadFile)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
static DWORD  (WINAPI *real_SetFilePointer)(HANDLE,LONG,PLONG,DWORD);
static DWORD  (WINAPI *real_GetFileSize)(HANDLE,LPDWORD);
static BOOL   (WINAPI *real_CloseHandle)(HANDLE);
static DWORD  (WINAPI *real_GetPrivateProfileStringA)(LPCSTR,LPCSTR,LPCSTR,LPSTR,DWORD,LPCSTR);

static HANDLE WINAPI hook_CreateFileA(LPCSTR fn, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    HANDLE h = real_CreateFileA(fn, acc, share, sa, disp, flags, tmpl);
    if (g_hn < MAXH && h != INVALID_HANDLE_VALUE) {
        g_h[g_hn].h = h; g_h[g_hn].pos = 0;
        lstrcpynA(g_h[g_hn].name, fn ? fn : "(null)", 260);
        g_hn++;
    }
    logf("[CreateFileA]  %-60s -> %s\n", fn ? fn : "(null)", (h==INVALID_HANDLE_VALUE)?"FAIL":"ok");
    return h;
}
static DWORD WINAPI hook_SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD method) {
    DWORD r = real_SetFilePointer(h, lo, hi, method);
    LONGLONG off = (LONGLONG)(unsigned)r | ((hi)?((LONGLONG)*hi<<32):0);
    if (method == FILE_BEGIN) set_pos(h, off);
    logf("[Seek]         %-40s method=%lu off=%lld\n", name_of(h), method, off);
    return r;
}
static BOOL WINAPI hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    LONGLONG p = get_pos(h);
    BOOL r = real_ReadFile(h, buf, n, got, ov);
    DWORD g = got ? *got : 0;
    logf("[ReadFile]     %-40s @%-10lld len=%-8lu got=%lu\n", name_of(h), p, n, g);
    if (p >= 0) set_pos(h, p + g);
    return r;
}
static DWORD WINAPI hook_GetFileSize(HANDLE h, LPDWORD hi) {
    DWORD r = real_GetFileSize(h, hi);
    logf("[GetFileSize]  %-40s -> %lu\n", name_of(h), r);
    return r;
}
static BOOL WINAPI hook_CloseHandle(HANDLE h) {
    logf("[CloseHandle]  %-40s\n", name_of(h));
    forget_handle(h);
    return real_CloseHandle(h);
}
static DWORD WINAPI hook_GetPrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR out, DWORD sz, LPCSTR file) {
    DWORD r = real_GetPrivateProfileStringA(sec, key, def, out, sz, file);
    logf("[INI]          [%s] %s = \"%s\"   (file=%s)\n", sec?sec:"*", key?key:"*", out?out:"", file?file:"?");
    return r;
}

// ---------------- IAT patch ----------------
// Redirect one imported symbol in module `mod` to `repl`, returning the original.
static void* hook_iat(HMODULE mod, const char* dll, const char* sym, void* repl) {
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp.VirtualAddress) return NULL;
    IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + imp.VirtualAddress);
    void* orig = NULL;
    FARPROC target = GetProcAddress(GetModuleHandleA(dll), sym);
    for (; d->Name; d++) {
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA* ft  = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        if (!d->OriginalFirstThunk) oft = ft;
        for (; oft->u1.AddressOfData; oft++, ft++) {
            // match by resolved address (robust across forwarders / name decoration)
            if ((FARPROC)ft->u1.Function == target) {
                DWORD old;
                VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
                orig = (void*)(uintptr_t)ft->u1.Function;
                ft->u1.Function = (uintptr_t)repl;
                VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
                return orig;
            }
        }
    }
    return NULL;
}

// ---------------- WAV writer (PCM16 mono 8 kHz) ----------------
static void write_wav_pcm16(const char* path, const short* pcm, unsigned nsamp) {
    FILE* f = fopen(path, "wb");
    if (!f) { logf("ERROR: cannot open %s\n", path); return; }
    unsigned dataBytes = nsamp * 2, sr = 8000;
    unsigned riff = 36 + dataBytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    unsigned fmtlen = 16; unsigned short fmt = 1, ch = 1, ba = 2, bps = 16; unsigned br = sr*2;
    fwrite(&fmtlen,4,1,f); fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f); fwrite(&sr,4,1,f);
    fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
    fwrite("data",1,4,f); fwrite(&dataBytes,4,1,f);
    fwrite(pcm, 2, nsamp, f);
    fclose(f);
}

int main(int argc, char** argv) {
    const char* text = (argc > 1) ? argv[1] : "Tornado warning for your county until 8:45 PM.";
    const char* outwav = (argc > 2) ? argv[2] : "out.wav";
    g_log = fopen("trace.txt", "w");
    build_ulaw();

    // Load the patched core relative to the repo, or fall back to the installed copy.
    HMODULE av = LoadLibraryA("..\\..\\lib\\avcore_acu.dll");
    if (!av) av = LoadLibraryA("C:\\Program Files (x86)\\AcuVoiceRoger\\lib\\avcore_acu.dll");
    if (!av) { logf("FATAL: cannot load avcore_acu.dll (err %lu)\n", GetLastError()); return 1; }
    logf("avcore loaded at %p\n", (void*)av);

    // Install IAT hooks on avcore's kernel32 imports.
    real_CreateFileA              = (void*)hook_iat(av, "kernel32.dll", "CreateFileA", hook_CreateFileA);
    real_ReadFile                 = (void*)hook_iat(av, "kernel32.dll", "ReadFile", hook_ReadFile);
    real_SetFilePointer           = (void*)hook_iat(av, "kernel32.dll", "SetFilePointer", hook_SetFilePointer);
    real_GetFileSize              = (void*)hook_iat(av, "kernel32.dll", "GetFileSize", hook_GetFileSize);
    real_CloseHandle              = (void*)hook_iat(av, "kernel32.dll", "CloseHandle", hook_CloseHandle);
    real_GetPrivateProfileStringA = (void*)hook_iat(av, "kernel32.dll", "GetPrivateProfileStringA", hook_GetPrivateProfileStringA);
    logf("hooks: CreateFileA=%p ReadFile=%p Seek=%p GetFileSize=%p Close=%p INI=%p\n",
         real_CreateFileA, real_ReadFile, real_SetFilePointer, real_GetFileSize, real_CloseHandle, real_GetPrivateProfileStringA);

    txtstr_to_sndbuf_t synth = (txtstr_to_sndbuf_t)GetProcAddress(av, "_txtstr_to_sndbuf@16");
    free_sndbuf_t      freeb = (free_sndbuf_t)     GetProcAddress(av, "_free_sndbuf@4");
    if (!synth || !freeb) { logf("FATAL: missing exports synth=%p free=%p\n", synth, freeb); return 2; }

    logf("\n==== SYNTH: \"%s\" ====\n", text);
    void* buf = NULL; unsigned len = 0;
    unsigned rc = synth(text, &buf, &len, 0);
    logf("==== synth rc=%u  ulaw_bytes=%u (%.2f s) ====\n\n", rc, len, len / 8000.0);
    if (rc != 0 || !buf) { logf("synth failed\n"); return 3; }

    // u-law -> PCM16, write WAV
    short* pcm = (short*)malloc(len * sizeof(short));
    for (unsigned i = 0; i < len; i++) pcm[i] = g_ulaw[((unsigned char*)buf)[i]];
    write_wav_pcm16(outwav, pcm, len);
    logf("wrote %s (%u samples, %.2f s)\n", outwav, len, len / 8000.0);
    free(pcm);
    freeb(&buf);
    if (g_log) fclose(g_log);
    return 0;
}
