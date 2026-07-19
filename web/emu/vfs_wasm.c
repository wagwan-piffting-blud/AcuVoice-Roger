// vfs_wasm.c - WASM backend: the ENTIRE data tree (dictionaries + ~160 MB soundbank)
// is preloaded into Emscripten's in-memory FS (MEMFS) by --preload-file, so it ships
// packaged inside acu.data. File I/O is therefore plain POSIX stdio against MEMFS -
// the same code path as the native vfs.c backend, just with the wasm ini_get below.
// (This replaces the old sync-Range-XHR-per-session backend; the soundbank is now
//  part of the bundle instead of being streamed from the server on demand.)
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vfile { FILE* fp; uint32_t size; };

void vfs_init(const char* data_root){ (void)data_root; }  // MEMFS paths are hardcoded in ini_get

vfile* vfs_open(const char* guest_path){
    // guest_path is a MEMFS path like "data/ulaw08sb/hashsnds.ply". The engine hardcodes
    // lowercase names and MEMFS is case-sensitive, so prep_data.ps1 lowercases everything
    // in the preloaded tree to match. fopen resolves the relative path against cwd "/".
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
    const char* v=NULL;
    if(!strcmp(section,"AcuVoiceAppDir")){
        if(!strcmp(key,"SNDBANK")) v="data/ulaw08sb/";
        else if(!strcmp(key,"DICTFLSDIR")) v="data/dictfls/";
        else if(!strcmp(key,"TEMPDIR")) v="data/temp/";
    } else if(!strcmp(section,"AcuVoiceSettings")){
        if(!strcmp(key,"PAUSE1")) v="680";
        else if(!strcmp(key,"PAUSE2")) v="200";
        else if(!strcmp(key,"PAUSE3")) v="300";
        else if(!strcmp(key,"PAUSE4")) v="0";
    } else if(!strcmp(section,"AcuVoiceDictionary")){
        if(!strcmp(key,"CUSTOM")) v="NONE";
    }
    if(!v) v=def?def:"";
    int nlen=(int)strlen(v); if(nlen>outsz-1)nlen=outsz-1;
    memcpy(out,v,nlen); out[nlen]=0; return nlen;
}
