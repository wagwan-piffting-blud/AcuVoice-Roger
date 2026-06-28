// mem.c - region-based guest virtual memory
#include "emu.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

mem_t MEM;

void mem_init(void) {
    for (int i = 0; i < MEM.nreg; i++) free(MEM.regions[i].host);
    memset(&MEM, 0, sizeof(MEM));
}

uint8_t* mem_map(uint32_t va, uint32_t size, const char* name) {
    if (MEM.nreg >= MAX_REGIONS) { fprintf(stderr, "mem_map: too many regions\n"); exit(1); }
    // round size up to 16
    size = (size + 15) & ~15u;
    uint8_t* host = (uint8_t*)calloc(1, size);
    if (!host) { fprintf(stderr, "mem_map: OOM %u for %s\n", size, name); exit(1); }
    MEM.regions[MEM.nreg].va = va;
    MEM.regions[MEM.nreg].size = size;
    MEM.regions[MEM.nreg].host = host;
    MEM.regions[MEM.nreg].name = name;
    MEM.nreg++;
    return host;
}

region_t* mem_region_of(uint32_t va) {
    for (int i = 0; i < MEM.nreg; i++) {
        region_t* r = &MEM.regions[i];
        if (va >= r->va && va < r->va + r->size) return r;
    }
    return NULL;
}

uint8_t* mem_host(uint32_t va) {
    region_t* r = mem_region_of(va);
    return r ? r->host + (va - r->va) : NULL;
}

static int g_faults = 0;
static void fault(uint32_t va, const char* what) {
    // A correctly-executing synthesis never faults; if one ever does, halt cleanly so the chunk
    // fails fast (returns no audio) instead of spamming. Print only the first couple for diagnosis.
    CPU.faulted = 1; CPU.fault_addr = va; CPU.fault_msg = what; CPU.halted = 1;
    if (g_faults++ < 2)
        fprintf(stderr, "** MEM FAULT %s @ 0x%08x eip=0x%08x edi=%08x ecx=%08x\n",
                what, va, CPU.eip, CPU.r[EDI], CPU.r[ECX]);
}

uint8_t rd8(uint32_t va){ uint8_t* p=mem_host(va); if(!p){fault(va,"rd8");return 0;} return *p; }
uint16_t rd16(uint32_t va){ uint8_t* p=mem_host(va); if(!p){fault(va,"rd16");return 0;} return p[0]|(p[1]<<8); }
uint32_t rd32(uint32_t va){ uint8_t* p=mem_host(va); if(!p){fault(va,"rd32");return 0;} return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
void wr8(uint32_t va, uint8_t v){ uint8_t* p=mem_host(va); if(!p){fault(va,"wr8");return;} *p=v; }
void wr16(uint32_t va, uint16_t v){ uint8_t* p=mem_host(va); if(!p){fault(va,"wr16");return;} p[0]=v; p[1]=v>>8; }
void wr32(uint32_t va, uint32_t v){ uint8_t* p=mem_host(va); if(!p){fault(va,"wr32");return;} p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }

void mem_read(uint32_t va, void* dst, uint32_t n){
    uint8_t* d=(uint8_t*)dst;
    for(uint32_t i=0;i<n;i++) d[i]=rd8(va+i);
}
void mem_write(uint32_t va, const void* src, uint32_t n){
    const uint8_t* s=(const uint8_t*)src;
    for(uint32_t i=0;i<n;i++) wr8(va+i, s[i]);
}
