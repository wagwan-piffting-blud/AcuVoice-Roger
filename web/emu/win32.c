// win32.c - Win32 import shims + guest heap + import dispatch
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------- import table ----------------
typedef struct { char name[64]; shim_fn fn; int argbytes; } imp_t;
static imp_t g_imp[MAX_IMPORTS];
static int   g_nimp = 0;
int g_shim_cleanup = 0;

uint32_t arg32(int i){ return rd32(CPU.r[ESP] + 4 + 4*i); }
void ret_set(uint32_t v){ CPU.r[EAX] = v; }

int win32_is_import_va(uint32_t va){ return va >= IMP_BASE && va < IMP_BASE + (uint32_t)MAX_IMPORTS*IMP_STRIDE; }

void win32_dispatch(uint32_t va){
    int idx = (va - IMP_BASE) / IMP_STRIDE;
    if (idx < 0 || idx >= g_nimp || !g_imp[idx].fn){ fprintf(stderr,"** bad import dispatch idx=%d va=%08x\n",idx,va); CPU.halted=1; CPU.faulted=1; return; }
    g_shim_cleanup = g_imp[idx].argbytes;
    g_imp[idx].fn();
    uint32_t retaddr = rd32(CPU.r[ESP]);
    CPU.r[ESP] += 4 + g_shim_cleanup;
    CPU.eip = retaddr;
}

// ---------------- guest heap (implicit free list) ----------------
// block: [u32 size][u32 free] then payload(size). addresses in HEAP region.
static uint32_t HEAP_END;
static void heap_init(void){
    mem_map(HEAP_BASE, HEAP_SIZE, "heap");
    HEAP_END = HEAP_BASE + HEAP_SIZE;
    wr32(HEAP_BASE, HEAP_SIZE - 8);
    wr32(HEAP_BASE + 4, 1); // free
}
static uint32_t heap_alloc(uint32_t n, int zero){
    n = (n + 7) & ~7u; if(n==0) n=8;
    uint32_t b = HEAP_BASE;
    while (b < HEAP_END){
        uint32_t sz = rd32(b), fr = rd32(b+4);
        if (fr && sz >= n){
            if (sz >= n + 16){ // split
                uint32_t nb = b + 8 + n;
                wr32(nb, sz - n - 8); wr32(nb+4, 1);
                wr32(b, n);
            }
            wr32(b+4, 0);
            if (zero){ for(uint32_t i=0;i<rd32(b);i+=4) wr32(b+8+i,0); }
            return b + 8;
        }
        b += 8 + sz;
    }
    fprintf(stderr,"** heap OOM requesting %u\n", n);
    return 0;
}
static void heap_free(uint32_t p){
    if(!p) return; uint32_t b=p-8; wr32(b+4,1);
    // forward coalesce
    for(;;){ uint32_t sz=rd32(b); uint32_t nb=b+8+sz; if(nb>=HEAP_END) break; if(rd32(nb+4)) { wr32(b, sz + 8 + rd32(nb)); } else break; }
}
static uint32_t heap_realloc(uint32_t p, uint32_t n){
    if(!p) return heap_alloc(n,0);
    uint32_t oldsz = rd32(p-8);
    uint32_t np = heap_alloc(n,0); if(!np) return 0;
    uint32_t c = oldsz<n?oldsz:n; for(uint32_t i=0;i<c;i++) wr8(np+i, rd8(p+i));
    heap_free(p);
    return np;
}

// ---------------- handle table for files ----------------
#define HBASE 0xF0000000u
static vfile* g_files[256]; static int g_nfiles=0;
static uint32_t g_filepos[256];
static char g_fpath[256][48];   // short tag per slot (for read tracing)

// ---------------- helpers ----------------
static void guest_strcpy_out(uint32_t dst, const char* s, int max){ int i=0; for(; s[i] && i<max-1; i++) wr8(dst+i, s[i]); wr8(dst+i,0); }
static void guest_strn(uint32_t src, char* out, int max){ int i=0; for(; i<max-1; i++){ char c=(char)rd8(src+i); out[i]=c; if(!c)break; } out[i<max?i:max-1]=0; }

static uint32_t g_lasterr = 0;
static uint32_t g_tls[64]; static int g_tlsnext=1;
static uint32_t g_cmdline_va=0, g_modname_va=0;

// =========================================================================
//                               SHIMS
// =========================================================================
static void s_EnterCriticalSection(void){}
static void s_LeaveCriticalSection(void){}
static void s_InitializeCriticalSection(void){}
static void s_DeleteCriticalSection(void){}

static void s_GetPrivateProfileStringA(void){
    char sec[64],key[64],def[256],file[260]; char out[512];
    guest_strn(arg32(0),sec,sizeof sec); guest_strn(arg32(1),key,sizeof key);
    guest_strn(arg32(2),def,sizeof def); uint32_t outva=arg32(3); uint32_t outsz=arg32(4);
    guest_strn(arg32(5),file,sizeof file);
    int n = ini_get(sec,key,def,out,sizeof out);
    if((uint32_t)n > outsz-1) n=outsz-1;
    guest_strcpy_out(outva,out,outsz);
    emu_log("[INI] [%s] %s -> \"%s\"\n",sec,key,out);
    ret_set(n);
}
static void s_WritePrivateProfileStringA(void){ ret_set(1); }

static void s_CreateFileA(void){
    char path[300]; guest_strn(arg32(0),path,sizeof path);
    vfile* f = vfs_open(path);
    if(!f){ emu_log("[CreateFileA] %s -> INVALID\n",path); g_lasterr=2; ret_set(0xFFFFFFFF); return; }
    // reuse a freed slot; only grow g_nfiles when none free (handles are opened+closed per unit,
    // so the live count stays tiny - but the table must NOT overflow over a long session).
    int slot = -1;
    for (int k = 0; k < g_nfiles; k++) if (!g_files[k]) { slot = k; break; }
    if (slot < 0) {
        if (g_nfiles >= 256) { emu_log("** open-file table full\n"); vfs_close(f); g_lasterr=4; ret_set(0xFFFFFFFF); return; }
        slot = g_nfiles++;
    }
    g_files[slot]=f; g_filepos[slot]=0;
    { const char* bn=path; for(const char* q=path; *q; q++) if(*q=='\\'||*q=='/') bn=q+1; snprintf(g_fpath[slot],48,"%s",bn); }
    emu_log("[CreateFileA] %s -> h%d\n",path,slot);
    ret_set(HBASE + slot);
}
static int hslot(uint32_t h){ return (h>=HBASE && h<HBASE+256)?(int)(h-HBASE):-1; }
static void s_SetFilePointer(void){
    int s=hslot(arg32(0)); int32_t lo=(int32_t)arg32(1); uint32_t method=arg32(3);
    if(s<0){ ret_set(0xFFFFFFFF); return; }
    uint32_t sz=vfs_size(g_files[s]);
    uint32_t np = method==0?(uint32_t)lo : method==1?g_filepos[s]+lo : sz+lo;
    g_filepos[s]=np; ret_set(np);
}
static void s_ReadFile(void){
    int s=hslot(arg32(0)); uint32_t buf=arg32(1), n=arg32(2), pgot=arg32(3);
    if(s<0){ ret_set(0); return; }
    static uint8_t tmp[1<<16];
    uint32_t total=0;
    while(total<n){ uint32_t chunk=n-total; if(chunk>sizeof tmp)chunk=sizeof tmp;
        int got=vfs_read(g_files[s], g_filepos[s], tmp, chunk);
        if(got<=0) break; mem_write(buf+total,tmp,got); g_filepos[s]+=got; total+=got; if((uint32_t)got<chunk) break; }
    if(pgot) wr32(pgot,total);
    if(EMU_VERBOSE) emu_log("[rd] %-14s @%-8u +%-6u =%u\n", (s>=0?g_fpath[s]:"?"), g_filepos[s]-total, n, total);
    ret_set(1);
}
static void s_GetFileSize(void){ int s=hslot(arg32(0)); uint32_t hi=arg32(1); if(hi) wr32(hi,0); ret_set(s<0?0xFFFFFFFF:vfs_size(g_files[s])); }
static void s_CloseHandle(void){ int s=hslot(arg32(0)); if(s>=0&&g_files[s]){ vfs_close(g_files[s]); g_files[s]=0; } ret_set(1); }
static void s_GetFileAttributesA(void){ char p[300]; guest_strn(arg32(0),p,sizeof p); vfile* f=vfs_open(p); if(f){ vfs_close(f); ret_set(0x80);} else ret_set(0xFFFFFFFF); }
static void s_WriteFile(void){ uint32_t n=arg32(2),pg=arg32(3); if(pg)wr32(pg,n); ret_set(1); }
static void s_FlushFileBuffers(void){ ret_set(1); }

// heap / mem
static void s_HeapAlloc(void){ uint32_t flags=arg32(1),n=arg32(2); ret_set(heap_alloc(n, (flags&8)!=0)); }
static void s_HeapFree(void){ heap_free(arg32(2)); ret_set(1); }
static void s_HeapReAlloc(void){ uint32_t flags=arg32(1),p=arg32(2),n=arg32(3); (void)flags; ret_set(heap_realloc(p,n)); }
static void s_HeapCreate(void){ ret_set(0xE0000001u); }
static void s_HeapDestroy(void){ ret_set(1); }
static void s_GetProcessHeap(void){ ret_set(0xE0000001u); }
static void s_LocalAlloc(void){ uint32_t flags=arg32(0),n=arg32(1); ret_set(heap_alloc(n,(flags&0x40)!=0)); }
static void s_LocalFree(void){ heap_free(arg32(0)); ret_set(0); }
static void s_LocalLock(void){ ret_set(arg32(0)); }
static void s_LocalUnlock(void){ ret_set(1); }
static void s_LocalHandle(void){ ret_set(arg32(0)); }
static void s_VirtualAlloc(void){ uint32_t n=arg32(1); ret_set(heap_alloc(n,1)); }
static void s_VirtualFree(void){ ret_set(1); }

// strings (kernel32 lstr*)
static void s_lstrcpyA(void){ uint32_t d=arg32(0),s=arg32(1); int i=0; for(;;){ uint8_t c=rd8(s+i); wr8(d+i,c); if(!c)break; i++; } ret_set(d); }
static void s_lstrcatA(void){ uint32_t d=arg32(0),s=arg32(1); int e=0; while(rd8(d+e))e++; int i=0; for(;;){ uint8_t c=rd8(s+i); wr8(d+e+i,c); if(!c)break; i++; } ret_set(d); }
static void s_lstrlenA(void){ uint32_t s=arg32(0); int i=0; while(rd8(s+i))i++; ret_set(i); }
static void s_lstrcmpA(void){ uint32_t a=arg32(0),b=arg32(1); int i=0; for(;;){ uint8_t x=rd8(a+i),y=rd8(b+i); if(x!=y){ret_set((int)x-(int)y);return;} if(!x)break; i++; } ret_set(0); }

// locale/charset (approximate, ASCII/Latin-1)
static void s_MultiByteToWideChar(void){ uint32_t src=arg32(2); int srclen=(int)arg32(3); uint32_t dst=arg32(4); int dstlen=(int)arg32(5);
    int n=0; for(int i=0; (srclen<0)||(i<srclen); i++){ uint8_t c=rd8(src+i); if(dst&&n<dstlen) wr16(dst+2*n,c); n++; if(srclen<0&&!c)break; } ret_set(n); }
static void s_WideCharToMultiByte(void){ uint32_t src=arg32(2); int srclen=(int)arg32(3); uint32_t dst=arg32(4); int dstlen=(int)arg32(5);
    int n=0; for(int i=0; (srclen<0)||(i<srclen); i++){ uint16_t c=rd16(src+2*i); if(dst&&n<dstlen) wr8(dst+n,(uint8_t)c); n++; if(srclen<0&&!c)break; } ret_set(n); }
static void s_GetStringTypeA(void){ uint32_t src=arg32(2); int n=(int)arg32(3); uint32_t out=arg32(4); for(int i=0;i<n;i++){ uint8_t c=rd8(src+i); uint16_t t=0; if(c>='A'&&c<='Z')t=1|0x100; else if(c>='a'&&c<='z')t=2|0x100; else if(c>='0'&&c<='9')t=4|0x80; else if(c==' ')t=8|0x40; wr16(out+2*i,t);} ret_set(1); }
static void s_GetStringTypeW(void){ ret_set(1); }
static void s_LCMapStringA(void){ uint32_t flags=arg32(1),src=arg32(2); int n=(int)arg32(3); uint32_t dst=arg32(4); int dn=(int)arg32(5);
    int i=0; for(; (n<0||i<n); i++){ uint8_t c=rd8(src+i); if(flags&0x100){ if(c>='a'&&c<='z')c-=32; } if(flags&0x200){ if(c>='A'&&c<='Z')c+=32; } if(dst&&i<dn)wr8(dst+i,c); if(n<0&&!c)break; } ret_set(n<0?i+1:i); }
static void s_LCMapStringW(void){ ret_set(0); }
static void s_GetLocaleInfoA(void){ uint32_t out=arg32(2); uint32_t sz=arg32(3); if(out&&sz){ guest_strcpy_out(out,"",sz);} ret_set(1); }
static void s_GetLocaleInfoW(void){ ret_set(0); }
static void s_GetACP(void){ ret_set(1252); }
static void s_GetOEMCP(void){ ret_set(437); }
static void s_GetCPInfo(void){ uint32_t out=arg32(1); if(out){ wr32(out,1); wr8(out+4,0); wr8(out+5,0);} ret_set(1); }

// process/module/version
static void s_GetVersion(void){ ret_set(0x0105); }   // major 5 minor 1 (XP-like), NT
static void s_GetCommandLineA(void){ ret_set(g_cmdline_va); }
static void s_GetModuleHandleA(void){ ret_set(PE.image_base); }
static void s_GetModuleFileNameA(void){ uint32_t out=arg32(1),sz=arg32(2); guest_strcpy_out(out,"C:\\AcuVoiceRoger\\avcore.dll",sz); ret_set(26); }
static void s_GetProcAddress(void){ char n[64]; guest_strn(arg32(1),n,sizeof n); emu_log("[GetProcAddress] %s -> 0\n",n); ret_set(0); }
static void s_GetStartupInfoA(void){ uint32_t p=arg32(0); for(int i=0;i<68;i+=4) wr32(p+i,0); wr32(p,68); ret_set(0); }
static void s_GetStdHandle(void){ ret_set(0x10 + arg32(0)); }
static void s_SetStdHandle(void){ ret_set(1); }
static void s_GetFileType(void){ ret_set(1); } // FILE_TYPE_DISK
static void s_SetHandleCount(void){ ret_set(arg32(0)); }
static void s_GetCurrentThreadId(void){ ret_set(0x2000); }
static void s_GetCurrentProcess(void){ ret_set(0xFFFFFFFF); }
static void s_GetLastError(void){ ret_set(g_lasterr); }
static void s_SetLastError(void){ g_lasterr=arg32(0); }
static void s_TlsAlloc(void){ ret_set(g_tlsnext++); }
static void s_TlsFree(void){ ret_set(1); }
static void s_TlsSetValue(void){ uint32_t i=arg32(0); if(i<64)g_tls[i]=arg32(1); ret_set(1); }
static void s_TlsGetValue(void){ uint32_t i=arg32(0); ret_set(i<64?g_tls[i]:0); }
static void s_InterlockedIncrement(void){ uint32_t p=arg32(0); uint32_t v=rd32(p)+1; wr32(p,v); ret_set(v); }
static void s_InterlockedDecrement(void){ uint32_t p=arg32(0); uint32_t v=rd32(p)-1; wr32(p,v); ret_set(v); }
static void s_LoadLibraryA(void){ char n[128]; guest_strn(arg32(0),n,sizeof n); emu_log("[LoadLibraryA] %s\n",n); ret_set(0xD0000001u); }
static void s_ExitProcess(void){ emu_log("[ExitProcess] %u\n",arg32(0)); CPU.halted=1; }
static void s_TerminateProcess(void){ CPU.halted=1; }
static void s_RaiseException(void){ emu_log("[RaiseException] code=%08x\n",arg32(0)); CPU.halted=1; CPU.faulted=1; }
static uint32_t g_env_a=0, g_env_w=0;
static void s_GetEnvironmentStrings(void){ ret_set(g_env_a); }
static void s_GetEnvironmentStringsW(void){ ret_set(g_env_w); }
static void s_FreeEnvironmentStringsA(void){ ret_set(1); }
static void s_FreeEnvironmentStringsW(void){ ret_set(1); }

// wsprintfA - cdecl varargs (argbytes=0). minimal printf.
static void s_wsprintfA(void){
    uint32_t out=arg32(0); char fmt[512]; guest_strn(arg32(1),fmt,sizeof fmt);
    int ai=2; char buf[1024]; int o=0;
    for(int i=0; fmt[i] && o<(int)sizeof buf-1;){
        if(fmt[i]!='%'){ buf[o++]=fmt[i++]; continue; }
        i++; char spec[16]; int si=0; spec[si++]='%';
        while(fmt[i] && !strchr("diuxXcsp%",fmt[i]) && si<14) spec[si++]=fmt[i++];
        char c=fmt[i++]; spec[si++]=c; spec[si]=0;
        char tmp[256];
        if(c=='%'){ buf[o++]='%'; }
        else if(c=='s'){ char s[256]; guest_strn(arg32(ai++),s,sizeof s); o+=snprintf(buf+o,sizeof buf-o,"%s",s); }
        else if(c=='c'){ buf[o++]=(char)arg32(ai++); }
        else { uint32_t v=arg32(ai++); snprintf(tmp,sizeof tmp,spec,v); o+=snprintf(buf+o,sizeof buf-o,"%s",tmp); }
    }
    buf[o]=0; guest_strcpy_out(out,buf,o+1); ret_set(o);
}
static void s_MessageBoxA(void){ char t[256],c[256]; guest_strn(arg32(1),t,sizeof t); guest_strn(arg32(2),c,sizeof c); emu_log("[MessageBox] %s | %s\n",c,t); ret_set(1); }

// ---------------- registry ----------------
typedef struct { const char* name; shim_fn fn; int argb; } reg_t;
static const reg_t REG[] = {
    {"EnterCriticalSection",s_EnterCriticalSection,4},{"LeaveCriticalSection",s_LeaveCriticalSection,4},
    {"InitializeCriticalSection",s_InitializeCriticalSection,4},{"DeleteCriticalSection",s_DeleteCriticalSection,4},
    {"GetPrivateProfileStringA",s_GetPrivateProfileStringA,24},{"WritePrivateProfileStringA",s_WritePrivateProfileStringA,16},
    {"CreateFileA",s_CreateFileA,28},{"SetFilePointer",s_SetFilePointer,16},{"ReadFile",s_ReadFile,20},
    {"GetFileSize",s_GetFileSize,8},{"CloseHandle",s_CloseHandle,4},{"GetFileAttributesA",s_GetFileAttributesA,4},
    {"WriteFile",s_WriteFile,20},{"FlushFileBuffers",s_FlushFileBuffers,4},
    {"HeapAlloc",s_HeapAlloc,12},{"HeapFree",s_HeapFree,12},{"HeapReAlloc",s_HeapReAlloc,16},
    {"HeapCreate",s_HeapCreate,12},{"HeapDestroy",s_HeapDestroy,4},{"GetProcessHeap",s_GetProcessHeap,0},
    {"LocalAlloc",s_LocalAlloc,8},{"LocalFree",s_LocalFree,4},{"LocalLock",s_LocalLock,4},{"LocalUnlock",s_LocalUnlock,4},{"LocalHandle",s_LocalHandle,4},
    {"VirtualAlloc",s_VirtualAlloc,16},{"VirtualFree",s_VirtualFree,12},
    {"lstrcpyA",s_lstrcpyA,8},{"lstrcatA",s_lstrcatA,8},{"lstrlenA",s_lstrlenA,4},{"lstrcmpA",s_lstrcmpA,8},
    {"MultiByteToWideChar",s_MultiByteToWideChar,24},{"WideCharToMultiByte",s_WideCharToMultiByte,32},
    {"GetStringTypeA",s_GetStringTypeA,20},{"GetStringTypeW",s_GetStringTypeW,16},
    {"LCMapStringA",s_LCMapStringA,24},{"LCMapStringW",s_LCMapStringW,24},
    {"GetLocaleInfoA",s_GetLocaleInfoA,16},{"GetLocaleInfoW",s_GetLocaleInfoW,16},
    {"GetACP",s_GetACP,0},{"GetOEMCP",s_GetOEMCP,0},{"GetCPInfo",s_GetCPInfo,8},
    {"GetVersion",s_GetVersion,0},{"GetCommandLineA",s_GetCommandLineA,0},
    {"GetModuleHandleA",s_GetModuleHandleA,4},{"GetModuleFileNameA",s_GetModuleFileNameA,12},{"GetProcAddress",s_GetProcAddress,8},
    {"GetStartupInfoA",s_GetStartupInfoA,4},{"GetStdHandle",s_GetStdHandle,4},{"SetStdHandle",s_SetStdHandle,8},
    {"GetFileType",s_GetFileType,4},{"SetHandleCount",s_SetHandleCount,4},
    {"GetCurrentThreadId",s_GetCurrentThreadId,0},{"GetCurrentProcess",s_GetCurrentProcess,0},
    {"GetLastError",s_GetLastError,0},{"SetLastError",s_SetLastError,4},
    {"TlsAlloc",s_TlsAlloc,0},{"TlsFree",s_TlsFree,4},{"TlsSetValue",s_TlsSetValue,8},{"TlsGetValue",s_TlsGetValue,4},
    {"InterlockedIncrement",s_InterlockedIncrement,4},{"InterlockedDecrement",s_InterlockedDecrement,4},
    {"LoadLibraryA",s_LoadLibraryA,4},{"ExitProcess",s_ExitProcess,4},{"TerminateProcess",s_TerminateProcess,8},
    {"RaiseException",s_RaiseException,16},
    {"GetEnvironmentStrings",s_GetEnvironmentStrings,0},{"GetEnvironmentStringsW",s_GetEnvironmentStringsW,0},
    {"FreeEnvironmentStringsA",s_FreeEnvironmentStringsA,4},{"FreeEnvironmentStringsW",s_FreeEnvironmentStringsW,4},
    {"wsprintfA",s_wsprintfA,0},{"MessageBoxA",s_MessageBoxA,16},
    {0,0,0}
};

static void s_unimpl(void){ emu_log("** UNIMPLEMENTED import called (idx leak)\n"); CPU.halted=1; CPU.faulted=1; }

int win32_register_import(const char* dll, const char* name){
    (void)dll;
    shim_fn fn=s_unimpl; int argb=0; int found=0;
    for(const reg_t* r=REG; r->name; r++) if(!strcmp(r->name,name)){ fn=r->fn; argb=r->argb; found=1; break; }
    if(!found) fprintf(stderr,"** no shim for import %s!%s (will halt if called)\n",dll,name);
    int idx=g_nimp++;
    if(idx>=MAX_IMPORTS){ fprintf(stderr,"too many imports\n"); exit(1); }
    snprintf(g_imp[idx].name,sizeof g_imp[idx].name,"%s",name);
    g_imp[idx].fn=fn; g_imp[idx].argbytes=argb;
    return IMP_BASE + idx*IMP_STRIDE;
}

void win32_init(void){
    heap_init();
    // a small data region for fabricated guest strings (cmdline, env, etc.)
    uint8_t* aux = mem_map(0x60000000, 0x1000, "aux"); (void)aux;
    g_cmdline_va = 0x60000000; const char* cl="avcore"; for(int i=0;cl[i];i++) wr8(0x60000000+i,cl[i]); wr8(0x60000000+6,0);
    g_modname_va = 0x60000100; (void)g_modname_va;
    // minimal environment blocks (double-NUL terminated). ANSI + wide.
    g_env_a = 0x60000200; const char* ev="OS=Windows_NT"; int i=0; for(;ev[i];i++) wr8(g_env_a+i,ev[i]); wr8(g_env_a+i,0); wr8(g_env_a+i+1,0);
    g_env_w = 0x60000300; for(i=0;ev[i];i++) wr16(g_env_w+2*i,ev[i]); wr16(g_env_w+2*i,0); wr16(g_env_w+2*i+2,0);
}
