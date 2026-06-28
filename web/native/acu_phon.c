// acu_phon.c - drive the REAL avcore_acu.dll's phoneme path (synth_to_phon, mode 4)
// to dump the unit-key segment string for a word. Mirrors txtstr_to_sndbuf's loop but
// collects phonemes instead of audio. Used to compare against the emulator's segment.
//   acu_phon.exe "Paducah"
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef unsigned (__stdcall *init_t)(void* ssb, const char* text, unsigned flags);
typedef unsigned (__stdcall *tag_t)(void* ssb, unsigned on);
typedef int      (__stdcall *isseg_t)(void* ssb);
typedef unsigned (__stdcall *phon_t)(void* ssb, char* out);
typedef unsigned (__stdcall *close_t)(void* ssb);

int main(int argc, char** argv) {
    const char* text = (argc > 1) ? argv[1] : "Paducah";
    HMODULE av = LoadLibraryA("..\\..\\lib\\avcore_acu.dll");
    if (!av) av = LoadLibraryA("C:\\Program Files (x86)\\AcuVoiceRoger\\lib\\avcore_acu.dll");
    if (!av) { printf("cannot load avcore\n"); return 1; }
    init_t  init  = (init_t) GetProcAddress(av, "_initialize_SSB@12");
    tag_t   tag   = (tag_t)  GetProcAddress(av, "_set_tag_flag@8");
    isseg_t isseg = (isseg_t)GetProcAddress(av, "_isSeg_Available@4");
    phon_t  phon  = (phon_t) GetProcAddress(av, "_synth_to_phon@8");
    close_t close = (close_t)GetProcAddress(av, "_close_SSB@4");
    if (!init||!isseg||!phon||!close) { printf("missing exports\n"); return 2; }

    unsigned char ssb[0x1000]; memset(ssb, 0, sizeof ssb);
    unsigned rc = init(ssb, text, 0);
    if (rc != 0) { printf("initialize_SSB rc=%u\n", rc); return 3; }
    if (tag) tag(ssb, 0);
    printf("SEGMENT for \"%s\":\n", text);
    int seg = 0;
    while (isseg(ssb)) {
        char buf[16384]; memset(buf, 0, sizeof buf);
        phon(ssb, buf);
        printf("  seg%d: '%s'\n", seg++, buf);
        if (seg > 64) break;
    }
    close(ssb);
    return 0;
}
