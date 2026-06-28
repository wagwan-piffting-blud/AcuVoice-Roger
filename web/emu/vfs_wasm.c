// vfs_wasm.c - WASM backend: file I/O is delegated to JS (browser: sync Range XHR
// in a Worker with a block cache; node test: fs). Same vfs_* API as native vfs.c.
#include "emu.h"
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// JS provides Module.acuFileSize(url)->int (bytes, or -1 if missing)
//          and Module.acuFileRead(url,pos,len,dstPtr)->int (bytes written into HEAPU8[dst..])
EM_JS(int, js_file_size, (const char* url), {
    return Module.acuFileSize(UTF8ToString(url));
});
EM_JS(int, js_file_read, (const char* url, unsigned pos, unsigned len, unsigned dst), {
    return Module.acuFileRead(UTF8ToString(url), pos, len, dst);
});

struct vfile { char url[256]; uint32_t size; };

void vfs_init(const char* data_root){ (void)data_root; }  // paths are hardcoded URLs in ini_get

vfile* vfs_open(const char* guest_path){
    // guest_path is already a page-relative URL like "data/ulaw08sb/hashsnds.ply"
    int sz = js_file_size(guest_path);
    if(sz < 0) return NULL;
    vfile* f = (vfile*)malloc(sizeof(vfile));
    snprintf(f->url,sizeof f->url,"%s",guest_path);
    f->size = (uint32_t)sz;
    return f;
}
int vfs_read(vfile* f, uint32_t pos, void* dst, uint32_t n){
    if(!f) return 0;
    return js_file_read(f->url, pos, n, (unsigned)(uintptr_t)dst);
}
uint32_t vfs_size(vfile* f){ return f?f->size:0; }
void vfs_close(vfile* f){ if(f) free(f); }

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
