// host.c - boot the engine once, then synthesize text → µ-law (guest→host copy).
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

int EMU_VERBOSE = 0;
void emu_log(const char* fmt, ...){ if(!EMU_VERBOSE) return; va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); }

static uint32_t g_synth_va=0, g_free_va=0;
int ACU_BOOT_OK = 0;
#define SCRATCH 0x61000000u

// Call a guest function (stdcall/cdecl both fine - we clean our own pushes) with n dword args.
static uint32_t call_guest(uint32_t fn, const uint32_t* args, int n){
    CPU.r[ESP] = STACK_ESP0;
    for(int i=n-1;i>=0;i--) cpu_push32(args[i]);
    cpu_push32(RET_SENTINEL);
    CPU.eip = fn;
    CPU.halted=0; CPU.faulted=0;
    cpu_run(2000000000ULL);
    return CPU.r[EAX];
}

void acu_boot(const char* dll_path, const char* data_root){
    mem_init();
    cpu_reset();
    if(pe_load(dll_path)!=0){ fprintf(stderr,"pe_load failed\n"); exit(1); }
    win32_init();
    vfs_init(data_root);
    mem_map(SCRATCH, 0x10000, "scratch");
    emu_log("[boot] running DllMain...\n");
    pe_run_dllmain();
    if(CPU.faulted){ fprintf(stderr,"[boot] DllMain FAULTED\n"); }
    g_synth_va = pe_get_export("_txtstr_to_sndbuf@16");
    g_free_va  = pe_get_export("_free_sndbuf@4");
    ACU_BOOT_OK = (!CPU.faulted && g_synth_va && g_free_va) ? 1 : 0;
    emu_log("[boot] synth=%08x free=%08x ok=%d\n", g_synth_va, g_free_va, ACU_BOOT_OK);
}

// returns µ-law length; *out_ulaw = malloc'd host buffer (caller frees)
uint32_t acu_synth(const char* text, uint8_t** out_ulaw){
    *out_ulaw=NULL;
    if(!g_synth_va) return 0;
    // place text in scratch
    uint32_t text_va = SCRATCH;
    int tl=(int)strlen(text); for(int i=0;i<tl;i++) wr8(text_va+i,(uint8_t)text[i]); wr8(text_va+tl,0);
    uint32_t outbuf_slot = SCRATCH + 0x2000;  // void*  (engine writes guest ptr here)
    uint32_t outlen_slot = SCRATCH + 0x2008;  // unsigned
    wr32(outbuf_slot,0); wr32(outlen_slot,0);
    uint32_t args[4] = { text_va, outbuf_slot, outlen_slot, 0 };
    uint32_t rc = call_guest(g_synth_va, args, 4);
    if(CPU.faulted){ fprintf(stderr,"[synth] FAULTED rc=%u\n",rc); return 0; }
    if(rc!=0){ fprintf(stderr,"[synth] rc=%u (nonzero)\n",rc); return 0; }
    uint32_t buf_va = rd32(outbuf_slot);
    uint32_t len    = rd32(outlen_slot);
    if(!buf_va || !len){ fprintf(stderr,"[synth] empty buf=%08x len=%u\n",buf_va,len); return 0; }
    uint8_t* host = (uint8_t*)malloc(len);
    mem_read(buf_va, host, len);
    *out_ulaw = host;
    // free guest buffer
    uint32_t fa[1] = { outbuf_slot };
    if(g_free_va) call_guest(g_free_va, fa, 1);
    return len;
}
