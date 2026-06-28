// acu_unit.c - call REAL avcore_acu.dll internal functions directly with controlled inputs,
// to get ground-truth outputs for comparison against the x86 emulator.
//   FUN_10011570 (split-units)  : whole phoneme string -> unit array
//   FUN_100152d0 (right-context): builds the "(d)"-style following-onset annotation
//   FUN_10010270 (classifier)   : phoneme-cluster count used to switch in 152d0
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef void (__cdecl *f11570_t)(const char* text, int* idx, char* arr);
typedef void (__cdecl *f152d0_t)(char* out1, char* ctxout, char* p3, const char* p4, const char* p5);
typedef int  (__cdecl *f10270_t)(const char* s);

int main(int argc, char** argv) {
    const char* text = (argc > 1) ? argv[1] : " C7 pe'2du~4ku8` ";
    static char filebuf[4096];
    if (argc > 1 && argv[1][0] == '@') {                 // @file : read exact raw bytes as the input string
        FILE* f = fopen(argv[1] + 1, "rb");
        if (f) { size_t n = fread(filebuf, 1, sizeof filebuf - 1, f); filebuf[n] = 0; fclose(f); text = filebuf; }
    }
    HMODULE av = LoadLibraryA("..\\..\\lib\\avcore_acu.dll");
    if (!av) av = LoadLibraryA("C:\\Program Files (x86)\\AcuVoiceRoger\\lib\\avcore_acu.dll");
    if (!av) { printf("cannot load avcore (err %lu)\n", GetLastError()); return 1; }

    f11570_t f11570 = (f11570_t)((char*)av + 0x11570);
    f152d0_t f152d0 = (f152d0_t)((char*)av + 0x152d0);
    f10270_t f10270 = (f10270_t)((char*)av + 0x10270);

    // 1) whole-string split
    static char arr[256 * 0x14];
    memset(arr, 0, sizeof arr);
    int idx = 0;
    printf("=== FUN_10011570  INPUT: '%s'\n", text);
    f11570(text, &idx, arr);
    printf("idx=%d\n", idx);
    for (int i = 0; i < idx && i < 64; i++) printf("  [%2d] '%s'\n", i, arr + i * 0x14);

    // 2) classifier on the following-onset 'd'
    printf("=== FUN_10010270('d')   = %d\n", f10270("d"));
    printf("=== FUN_10010270('u~4') = %d\n", f10270("u~4"));

    // 3) right-context builder for the pe'2 unit: param_4='u~4' (following vowel), param_5='d' (following onset)
    {
        char out1[64] = "", ctxout[64] = "", p3buf[64] = "";
        f152d0(out1, ctxout, p3buf, "u~4", "d");
        printf("=== FUN_100152d0(p4='u~4', p5='d'): local_10(out1)='%s'  ctxout(param_2)='%s'  p3buf='%s'\n",
               out1, ctxout, p3buf);
    }
    return 0;
}
