// main_native.c - native test driver: emulate avcore_acu.dll → WAV, compare vs fixtures.
#include "emu.h"
#include "chunker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static short g_ulaw[256];
static void build_ulaw(void){ for(int i=0;i<256;i++){ int u=(~i)&0xFF,s=u&0x80,e=(u>>4)&7,man=u&0x0F; int v=(((man<<3)+0x84)<<e)-0x84; g_ulaw[i]=(short)(s?-v:v);} }
static void write_wav(const char* path,const short* pcm,unsigned ns){
    FILE* f=fopen(path,"wb"); if(!f)return; unsigned db=ns*2,sr=8000,riff=36+db,br=sr*2,fl=16; unsigned short fmt=1,ch=1,ba=2,bps=16;
    fwrite("RIFF",1,4,f);fwrite(&riff,4,1,f);fwrite("WAVE",1,4,f);fwrite("fmt ",1,4,f);fwrite(&fl,4,1,f);
    fwrite(&fmt,2,1,f);fwrite(&ch,2,1,f);fwrite(&sr,4,1,f);fwrite(&br,4,1,f);fwrite(&ba,2,1,f);fwrite(&bps,2,1,f);
    fwrite("data",1,4,f);fwrite(&db,4,1,f);fwrite(pcm,2,ns,f);fclose(f);
}

static char* read_file_text(const char* path){
    FILE* f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char* b=(char*)malloc(n+1); fread(b,1,n,f); b[n]=0; fclose(f); return b;
}
// Mirror wasm_main.c's acu_synth_wasm: boot ONCE, chunk the text, synth each chunk in sequence.
// Writes the concatenated µ-law to argv[3] if a global out path was set via env ACU_OUT.
static uint8_t* g_mc=NULL; static uint32_t g_mc_len=0, g_mc_cap=0;
static void multi_chunk(const char* text){
    static int starts[2048], lens[2048];
    int nc = acu_chunk(text, starts, lens, 2048);
    char buf[ACU_CHUNKMAX+8];
    for(int c=0;c<nc;c++){
        int l=lens[c]; if(l>ACU_CHUNKMAX) l=ACU_CHUNKMAX;
        memcpy(buf, text+starts[c], l); buf[l]=0;
        uint8_t* u=NULL; CPU.faulted=0;
        uint32_t n=acu_synth(buf,&u);
        fprintf(stderr,"[chunk %d] @%4d len=%3d faulted=%d ulaw=%u '%.34s'\n",c,starts[c],l,CPU.faulted,n,buf);
        if(n && u){
            if(g_mc_len+n > g_mc_cap){ g_mc_cap=(g_mc_cap?g_mc_cap:65536); while(g_mc_cap<g_mc_len+n) g_mc_cap*=2; g_mc=realloc(g_mc,g_mc_cap); }
            memcpy(g_mc+g_mc_len,u,n); g_mc_len+=n;
        }
        if(u)free(u);
    }
    fprintf(stderr,"[multi] total ulaw=%u (%.3fs) chunks=%d\n", g_mc_len, g_mc_len/8000.0, nc);
}
extern int ITRACE;
int main(int argc,char**argv){
    if(getenv("EMU_ITRACE")) ITRACE=1;
    const char* text   = argc>1?argv[1]:"Hello.";
    if(argc>1 && strncmp(argv[1],"@@",2)==0){
        char* t=read_file_text(argv[1]+2);
        if(getenv("EMU_VERBOSE")) EMU_VERBOSE=1;
        build_ulaw();
        acu_boot("..\\..\\lib\\avcore_acu.dll","C:\\Program Files (x86)\\AcuVoiceRoger\\data\\");
        multi_chunk(t);
        const char* outwav = (argc>2)?argv[2]:"multi_out.wav";
        short* pcm=(short*)malloc(g_mc_len*sizeof(short));
        for(uint32_t i=0;i<g_mc_len;i++) pcm[i]=g_ulaw[g_mc[i]];
        write_wav(outwav,pcm,g_mc_len);
        fprintf(stderr,"wrote %s (%u samples)\n",outwav,g_mc_len);
        return 0;
    }
    if(argc>1 && argv[1][0]=='@'){ char* t=read_file_text(argv[1]+1); if(t) text=t; }
    const char* outwav = argc>2?argv[2]:"emu_out.wav";
    const char* dll    = argc>3?argv[3]:"..\\..\\lib\\avcore_acu.dll";
    const char* root   = argc>4?argv[4]:"C:\\Program Files (x86)\\AcuVoiceRoger\\data\\";
    if(getenv("EMU_VERBOSE")) EMU_VERBOSE=1;
    build_ulaw();
    fprintf(stderr,"=== boot (dll=%s root=%s) ===\n",dll,root);
    acu_boot(dll,root);
    fprintf(stderr,"=== synth \"%s\" ===\n",text);
    uint8_t* ulaw=NULL;
    uint32_t n=acu_synth(text,&ulaw);
    fprintf(stderr,"=== ulaw_bytes=%u (%.2f s) ===\n",n,n/8000.0);
    if(!n||!ulaw){ fprintf(stderr,"SYNTH FAILED\n"); return 1; }
    short* pcm=(short*)malloc(n*sizeof(short));
    for(uint32_t i=0;i<n;i++) pcm[i]=g_ulaw[ulaw[i]];
    write_wav(outwav,pcm,n);
    fprintf(stderr,"wrote %s\n",outwav);

    // optional: compare raw µ-law against a fixture .ulaw if provided
    if(argc>5){ FILE* f=fopen(argv[5],"rb"); if(f){ fseek(f,0,SEEK_END); long fl=ftell(f); fseek(f,0,SEEK_SET);
        uint8_t* ref=(uint8_t*)malloc(fl); fread(ref,1,fl,f); fclose(f);
        long cmp=fl<(long)n?fl:(long)n; long diff=0; for(long i=0;i<cmp;i++) if(ref[i]!=ulaw[i])diff++;
        fprintf(stderr,"COMPARE vs %s: reflen=%ld emulen=%u firstmismatch=%s diffbytes=%ld/%ld\n",
            argv[5],fl,n,(fl==(long)n&&diff==0)?"NONE(EXACT)":"see",diff,cmp); }
    }
    free(pcm); free(ulaw);
    return 0;
}
