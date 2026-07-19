// vfs.c - NATIVE backend: guest file paths map directly to real disk files.
// (The browser build swaps this file for vfs_wasm.c, which does the same plain stdio
//  against a data tree preloaded into Emscripten MEMFS from acu.data; same API.)
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vfile { FILE* fp; uint32_t size; };

static char g_sndbank[300], g_dictdir[300], g_tempdir[300];
static int  g_pause[4] = {680,200,300,0};

void vfs_init(const char* data_root){
    // data_root e.g. "C:\\Program Files (x86)\\AcuVoiceRoger\\data\\"
    snprintf(g_sndbank,sizeof g_sndbank,"%sUlaw08Sb\\",data_root);
    snprintf(g_dictdir,sizeof g_dictdir,"%sDictfls\\",data_root);
    snprintf(g_tempdir,sizeof g_tempdir,"%sTemp\\",data_root);
}

vfile* vfs_open(const char* guest_path){
    FILE* fp = fopen(guest_path, "rb");
    if(!fp) return NULL;
    fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
    vfile* f = (vfile*)malloc(sizeof(vfile));
    f->fp=fp; f->size=(uint32_t)sz; return f;
}
int vfs_read(vfile* f, uint32_t pos, void* dst, uint32_t n){
    if(!f) return 0; if(fseek(f->fp,(long)pos,SEEK_SET)!=0) return 0;
    return (int)fread(dst,1,n,f->fp);
}
uint32_t vfs_size(vfile* f){ return f?f->size:0; }
void vfs_close(vfile* f){ if(f){ if(f->fp)fclose(f->fp); free(f); } }

int ini_get(const char* section,const char* key,const char* def,char* out,int outsz){
    const char* v = NULL;
    if(!strcmp(section,"AcuVoiceAppDir")){
        if(!strcmp(key,"SNDBANK")) v=g_sndbank;
        else if(!strcmp(key,"DICTFLSDIR")) v=g_dictdir;
        else if(!strcmp(key,"TEMPDIR")) v=g_tempdir;
    } else if(!strcmp(section,"AcuVoiceSettings")){
        static char num[16];
        if(!strcmp(key,"PAUSE1")){ snprintf(num,sizeof num,"%d",g_pause[0]); v=num; }
        else if(!strcmp(key,"PAUSE2")){ snprintf(num,sizeof num,"%d",g_pause[1]); v=num; }
        else if(!strcmp(key,"PAUSE3")){ snprintf(num,sizeof num,"%d",g_pause[2]); v=num; }
        else if(!strcmp(key,"PAUSE4")){ snprintf(num,sizeof num,"%d",g_pause[3]); v=num; }
    } else if(!strcmp(section,"AcuVoiceDictionary")){
        if(!strcmp(key,"CUSTOM")) v="NONE";
    }
    if(!v) v=def?def:"";
    int n=(int)strlen(v); if(n>outsz-1)n=outsz-1;
    memcpy(out,v,n); out[n]=0; return n;
}
