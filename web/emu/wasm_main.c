// wasm_main.c - Emscripten-exported API for the browser.
//   acu_boot_wasm()                 -> 1 on success (engine loaded + config + soundbank)
//   acu_synth_wasm(textPtr)         -> µ-law byte length (0 on failure); buffer at acu_ulaw_ptr()
//   acu_ulaw_ptr()                  -> pointer into HEAPU8 of the last synthesis result
//
// Input is chunked to <=200 chars at clause/space boundaries: avcore infinite-loops on
// long, expansion-dense text in a single call (the proven SAPI-engine mitigation).
#include <emscripten.h>
#include "emu.h"
#include "chunker.h"
#include <stdlib.h>
#include <string.h>

static uint8_t*  g_out = NULL;
static uint32_t  g_out_len = 0, g_out_cap = 0;

static void out_reset(void){ g_out_len = 0; }
static void out_append(const uint8_t* p, uint32_t n){
    if(g_out_len + n > g_out_cap){
        uint32_t nc = (g_out_cap?g_out_cap:65536);
        while(nc < g_out_len + n) nc *= 2;
        g_out = (uint8_t*)realloc(g_out, nc); g_out_cap = nc;
    }
    memcpy(g_out + g_out_len, p, n); g_out_len += n;
}

EMSCRIPTEN_KEEPALIVE int acu_boot_wasm(void){
    acu_boot("avcore_acu.dll", "");
    return ACU_BOOT_OK;
}

// Synthesize one already-bounded chunk; append µ-law to the output buffer.
static int synth_chunk(const char* text){
    uint8_t* u = NULL;
    uint32_t n = acu_synth(text, &u);
    if(n && u){ out_append(u, n); }
    if(u) free(u);
    return (int)n;
}

EMSCRIPTEN_KEEPALIVE int acu_synth_wasm(const char* text){
    out_reset();
    static int starts[2048], lens[2048];
    int nc = acu_chunk(text, starts, lens, 2048);
    char buf[ACU_CHUNKMAX + 8];
    for(int c = 0; c < nc; c++){
        int l = lens[c]; if(l > ACU_CHUNKMAX) l = ACU_CHUNKMAX;
        memcpy(buf, text + starts[c], l); buf[l] = 0;
        synth_chunk(buf);
    }
    return (int)g_out_len;
}

EMSCRIPTEN_KEEPALIVE uint8_t* acu_ulaw_ptr(void){ return g_out; }
