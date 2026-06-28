// emu.h - shared types for the AcuVoice x86 emulator (native-first, then emcc→WASM)
#ifndef EMU_H
#define EMU_H
#include <stdint.h>
#include <stddef.h>

// ---------------- guest memory (region-based VA translation) ----------------
// Guest virtual addresses are sparse (image @0x10000000, stack low, TEB high), so we
// map a handful of host-backed regions and translate on every access.
#define MAX_REGIONS 32
typedef struct { uint32_t va; uint32_t size; uint8_t* host; const char* name; } region_t;

typedef struct {
    region_t regions[MAX_REGIONS];
    int nreg;
} mem_t;

extern mem_t MEM;

void     mem_init(void);
uint8_t* mem_map(uint32_t va, uint32_t size, const char* name); // allocate+register, returns host
uint8_t* mem_host(uint32_t va);                                  // host ptr or NULL (fault)
region_t* mem_region_of(uint32_t va);

uint8_t  rd8(uint32_t va);
uint16_t rd16(uint32_t va);
uint32_t rd32(uint32_t va);
void     wr8(uint32_t va, uint8_t v);
void     wr16(uint32_t va, uint16_t v);
void     wr32(uint32_t va, uint32_t v);
void     mem_read(uint32_t va, void* dst, uint32_t n);
void     mem_write(uint32_t va, const void* src, uint32_t n);

// ---------------- guest layout constants ----------------
#define IMAGE_BASE     0x10000000u
#define STACK_TOP      0x00800000u   // TEB StackBase (top of guest stack)
#define STACK_SIZE     0x00700000u   // 7 MB mapped: [0x100000, 0x800000)
#define STACK_ESP0     (STACK_TOP - 0x00200000u)  // start ESP 2 MB below StackBase
                                                  // (mimic being deep in a call chain so the
                                                  //  DLL's ESP-relative locals have headroom)
#define HEAP_BASE      0x30000000u   // arena allocator hands out from here
#define HEAP_SIZE      0x04000000u   // 64 MB guest heap (plenty: ~1 MB tables + ~1 MB/utterance)
#define TEB_BASE       0x7ffde000u
#define PEB_BASE       0x7ffdf000u
#define IMP_BASE       0x71000000u   // import thunk pseudo-addresses live here
#define IMP_STRIDE     16
#define MAX_IMPORTS    512

// ---------------- CPU ----------------
enum { EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI };
typedef struct {
    uint32_t r[8];         // general regs, indexed by enum
    uint32_t eip;
    uint32_t eflags;
    uint32_t seg_fs_base;  // FS base (→ TEB)
    uint32_t seg_gs_base;
    // x87 (minimal): 8-entry stack of doubles + top pointer
    double  st[8];
    int     fpu_top;
    uint16_t fpu_sw, fpu_cw;
    int      halted;
    uint32_t fault_addr; int faulted; const char* fault_msg;
} cpu_t;

extern cpu_t CPU;

// EFLAGS bits
#define FL_CF 0x0001
#define FL_PF 0x0004
#define FL_AF 0x0010
#define FL_ZF 0x0040
#define FL_SF 0x0080
#define FL_TF 0x0100
#define FL_IF 0x0200
#define FL_DF 0x0400
#define FL_OF 0x0800

void cpu_reset(void);
// Execute up to `max_insns` instructions; returns 0 if it ran out, 1 if HLT/return-sentinel hit,
// negative on fault. EIP==RET_SENTINEL means the top-level call returned.
int  cpu_run(uint64_t max_insns);
void cpu_push32(uint32_t v);
uint32_t cpu_pop32(void);

#define RET_SENTINEL 0xdeadbeefu   // pushed as return address for the top-level call

// ---------------- PE loader ----------------
typedef struct {
    uint32_t image_base, size_of_image, entry_rva;
    uint32_t export_rva, export_size;
} pe_info_t;

extern pe_info_t PE;
int      pe_load(const char* path);              // map sections, relocs, imports, TEB
uint32_t pe_get_export(const char* name);        // VA of an export by (decorated) name
void     pe_run_dllmain(void);                   // call entry(DLL_PROCESS_ATTACH)

// ---------------- imports / Win32 shims ----------------
typedef void (*shim_fn)(void);   // reads args off guest stack, sets EAX, sets *cleanup bytes
typedef struct { const char* name; shim_fn fn; int argbytes; } shim_t;
shim_fn  win32_resolve(const char* dll, const char* name, int* argbytes);
void     win32_init(void);
// helpers for shims:
uint32_t arg32(int i);          // stdcall arg i (0-based), i.e. [ESP + 4 + 4*i]
void     ret_set(uint32_t eax); // set return value
extern int g_shim_cleanup;      // bytes to pop for the current shim (set by dispatcher)

// ---------------- VFS (pluggable: native disk now, range-fetch in browser) ----------------
typedef struct vfile vfile;
void   vfs_init(const char* root);   // native: filesystem root that maps the guest data tree
vfile* vfs_open(const char* guest_path);
int    vfs_read(vfile* f, uint32_t pos, void* dst, uint32_t n); // returns bytes read
uint32_t vfs_size(vfile* f);
void   vfs_close(vfile* f);
// INI access (serve acuvoice.ini values)
int    ini_get(const char* section, const char* key, const char* def, char* out, int outsz);

// ---------------- host orchestration ----------------
// Synthesize `text` → newly-malloc'd µ-law buffer (host memory). Returns len, or 0 on failure.
uint32_t acu_synth(const char* text, uint8_t** out_ulaw);
void     acu_boot(const char* dll_path, const char* data_root); // load+init the engine once
extern int ACU_BOOT_OK;

// logging
void emu_log(const char* fmt, ...);
extern int EMU_VERBOSE;

#endif
