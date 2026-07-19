// cpu.c - 32-bit x86 interpreter (flat protected mode), native-first.
// Covers the common MSVC6 integer instruction set + string ops + a partial x87.
// Unknown opcodes trap with EIP + bytes so coverage can be extended iteratively.
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

cpu_t CPU;

void win32_dispatch(uint32_t va);   // win32.c
int  win32_is_import_va(uint32_t va);

// ALU logic helpers (defined at end of file)
uint32_t do_logic_or (uint32_t a,uint32_t b,int sz);
uint32_t do_logic_and(uint32_t a,uint32_t b,int sz);
uint32_t do_logic_xor(uint32_t a,uint32_t b,int sz);

void cpu_reset(void){ memset(&CPU,0,sizeof CPU); CPU.eflags=0x202; CPU.fpu_cw=0x037f; CPU.fpu_top=8; }
void cpu_push32(uint32_t v){ CPU.r[ESP]-=4; wr32(CPU.r[ESP], v); }
uint32_t cpu_pop32(void){ uint32_t v=rd32(CPU.r[ESP]); CPU.r[ESP]+=4; return v; }

// x87 FIST/FISTP/FRNDINT round per FPU control-word RC bits (10-11);
// default 00 = round-to-nearest-even. Backported from _emu/cpu.c
// 2026-06-30 — the C `(int)cast` truncates toward zero, x87 rounds
// per the RC field, so any TTS code that does FRNDINT or FIST/FISTP
// stores integer frame/sample counts with the wrong rounding mode →
// off-by-one drift accumulates. Roger's prosody DSP is full of these.
static double fpu_round_rc(double v){
    int rc = (CPU.fpu_cw >> 10) & 3;
    switch(rc){
        case 0: return nearbyint(v);  // RNE (round to nearest even — the x87 default)
        case 1: return floor(v);      // RD  (toward -inf)
        case 2: return ceil(v);       // RU  (toward +inf)
        case 3: return trunc(v);      // RZ  (toward zero)
    }
    return v;
}

// ---------------- fetch ----------------
static uint8_t  fetch8(void){ uint8_t v=rd8(CPU.eip); CPU.eip++; return v; }
static uint16_t fetch16(void){ uint16_t v=rd16(CPU.eip); CPU.eip+=2; return v; }
static uint32_t fetch32(void){ uint32_t v=rd32(CPU.eip); CPU.eip+=4; return v; }

// ---------------- flags ----------------
static const uint32_t SZMASK[5]={0,0xff,0xffff,0,0xffffffff};
static const uint32_t SIGN[5]  ={0,0x80,0x8000,0,0x80000000};
static int parity8(uint32_t v){ v&=0xff; v^=v>>4; v^=v>>2; v^=v>>1; return (~v)&1; }
static void set_szp(uint32_t res,int sz){
    uint32_t m=SZMASK[sz]; res&=m;
    CPU.eflags &= ~(FL_ZF|FL_SF|FL_PF);
    if(res==0) CPU.eflags|=FL_ZF;
    if(res & SIGN[sz]) CPU.eflags|=FL_SF;
    if(parity8(res)) CPU.eflags|=FL_PF;
}
static void setf(uint32_t bit,int on){ if(on) CPU.eflags|=bit; else CPU.eflags&=~bit; }

static uint32_t do_add(uint32_t a,uint32_t b,int sz){
    uint64_t r=(uint64_t)(a&SZMASK[sz])+(b&SZMASK[sz]);
    uint32_t res=(uint32_t)r; set_szp(res,sz);
    setf(FL_CF, (r>>(sz*8))&1);
    setf(FL_AF, ((a^b^res)&0x10)!=0);
    setf(FL_OF, ((~(a^b)&(a^res))&SIGN[sz])!=0);
    return res&SZMASK[sz];
}
static uint32_t do_adc(uint32_t a,uint32_t b,int sz){
    uint32_t c=(CPU.eflags&FL_CF)?1:0;
    uint64_t r=(uint64_t)(a&SZMASK[sz])+(b&SZMASK[sz])+c;
    uint32_t res=(uint32_t)r; set_szp(res,sz);
    setf(FL_CF, (r>>(sz*8))&1);
    setf(FL_AF, ((a^b^res)&0x10)!=0);
    setf(FL_OF, ((~(a^b)&(a^res))&SIGN[sz])!=0);
    return res&SZMASK[sz];
}
static uint32_t do_sub(uint32_t a,uint32_t b,int sz){
    uint32_t aa=a&SZMASK[sz], bb=b&SZMASK[sz];
    uint32_t res=(aa-bb)&SZMASK[sz]; set_szp(res,sz);
    setf(FL_CF, aa<bb);
    setf(FL_AF, ((a^b^res)&0x10)!=0);
    setf(FL_OF, (((a^b)&(a^res))&SIGN[sz])!=0);
    return res;
}
static uint32_t do_sbb(uint32_t a,uint32_t b,int sz){
    uint32_t c=(CPU.eflags&FL_CF)?1:0;
    uint32_t aa=a&SZMASK[sz]; uint64_t bb=(uint64_t)(b&SZMASK[sz])+c;
    uint32_t res=(uint32_t)((aa-bb))&SZMASK[sz]; set_szp(res,sz);
    setf(FL_CF, (uint64_t)aa < bb);
    setf(FL_AF, ((a^b^res)&0x10)!=0);
    setf(FL_OF, (((a^b)&(a^res))&SIGN[sz])!=0);
    return res;
}
static uint32_t do_logic(uint32_t res,int sz){ set_szp(res,sz); setf(FL_CF,0); setf(FL_OF,0); setf(FL_AF,0); return res&SZMASK[sz]; }
static uint32_t do_inc(uint32_t a,int sz){ uint32_t res=(a+1)&SZMASK[sz]; int cf=CPU.eflags&FL_CF; set_szp(res,sz); setf(FL_AF,((a^1^res)&0x10)!=0); setf(FL_OF,(a&SZMASK[sz])==(SIGN[sz]-1)); setf(FL_CF,cf); return res; }
static uint32_t do_dec(uint32_t a,int sz){ uint32_t res=(a-1)&SZMASK[sz]; int cf=CPU.eflags&FL_CF; set_szp(res,sz); setf(FL_AF,((a^1^res)&0x10)!=0); setf(FL_OF,(a&SZMASK[sz])==SIGN[sz]); setf(FL_CF,cf); return res; }

// ---------------- register file access by encoded index+size ----------------
static uint32_t getreg(int idx,int sz){
    if(sz==1){ if(idx<4) return CPU.r[idx]&0xff; return (CPU.r[idx-4]>>8)&0xff; }
    if(sz==2) return CPU.r[idx]&0xffff;
    return CPU.r[idx];
}
static void setreg(int idx,int sz,uint32_t v){
    if(sz==1){ if(idx<4) CPU.r[idx]=(CPU.r[idx]&~0xffu)|(v&0xff); else CPU.r[idx-4]=(CPU.r[idx-4]&~0xff00u)|((v&0xff)<<8); }
    else if(sz==2) CPU.r[idx]=(CPU.r[idx]&~0xffffu)|(v&0xffff);
    else CPU.r[idx]=v;
}

// ---------------- ModRM ----------------
typedef struct { int mod,reg,rm; int is_mem; uint32_t addr; } modrm_t;
static uint32_t g_seg_base; // current effective segment base (for FS/GS overrides)

static void decode_modrm(modrm_t* m,int addr_override){
    (void)addr_override;
    uint8_t b=fetch8();
    m->mod=b>>6; m->reg=(b>>3)&7; m->rm=b&7;
    m->is_mem=(m->mod!=3);
    if(!m->is_mem){ m->addr=0; return; }
    uint32_t addr=0;
    if(m->rm==4){ // SIB
        uint8_t sib=fetch8();
        int scale=sib>>6, index=(sib>>3)&7, base=sib&7;
        uint32_t idxval=(index==4)?0:CPU.r[index];
        uint32_t baseval;
        if(base==5 && m->mod==0){ baseval=fetch32(); }
        else baseval=CPU.r[base];
        addr=baseval + (idxval<<scale);
    } else if(m->rm==5 && m->mod==0){ addr=fetch32(); }
    else addr=CPU.r[m->rm];
    if(m->mod==1) addr+=(int8_t)fetch8();
    else if(m->mod==2) addr+=(int32_t)fetch32();
    m->addr = addr + g_seg_base;
}
static uint32_t rm_read(modrm_t* m,int sz){ return m->is_mem ? (sz==1?rd8(m->addr):sz==2?rd16(m->addr):rd32(m->addr)) : getreg(m->rm,sz); }
static void rm_write(modrm_t* m,int sz,uint32_t v){ if(m->is_mem){ if(sz==1)wr8(m->addr,v);else if(sz==2)wr16(m->addr,v);else wr32(m->addr,v);} else setreg(m->rm,sz,v); }

// ---------------- conditions ----------------
static int cond(int c){
    int cf=!!(CPU.eflags&FL_CF), zf=!!(CPU.eflags&FL_ZF), sf=!!(CPU.eflags&FL_SF), of=!!(CPU.eflags&FL_OF), pf=!!(CPU.eflags&FL_PF);
    switch(c&0xf){
        case 0x0:return of; case 0x1:return !of; case 0x2:return cf; case 0x3:return !cf;
        case 0x4:return zf; case 0x5:return !zf; case 0x6:return cf||zf; case 0x7:return !(cf||zf);
        case 0x8:return sf; case 0x9:return !sf; case 0xa:return pf; case 0xb:return !pf;
        case 0xc:return sf!=of; case 0xd:return sf==of; case 0xe:return zf||(sf!=of); case 0xf:return !(zf||(sf!=of));
    } return 0;
}

// ---------------- shifts/rotates ----------------
static uint32_t do_shift(int op,uint32_t v,uint32_t cnt,int sz){
    uint32_t m=SZMASK[sz]; v&=m; cnt &= 31; if(sz<4) {} // (count masked to 5 bits on 386+)
    if(cnt==0) return v;
    uint32_t res=v; int cf=0,of=0;
    switch(op){
        case 4: case 6: // SHL/SAL
            for(uint32_t i=0;i<cnt;i++){ cf=(res&SIGN[sz])?1:0; res=(res<<1)&m; }
            of=cf ^ ((res&SIGN[sz])?1:0); break;
        case 5: // SHR
            for(uint32_t i=0;i<cnt;i++){ cf=res&1; res>>=1; }
            of=(v&SIGN[sz])?1:0; break;
        case 7: // SAR
            { int neg=(v&SIGN[sz])?1:0;
              for(uint32_t i=0;i<cnt;i++){ cf=res&1; res>>=1; if(neg) res|=SIGN[sz]; }
              of=0; } break;
        case 0: // ROL
            for(uint32_t i=0;i<cnt;i++){ cf=(res&SIGN[sz])?1:0; res=((res<<1)|cf)&m; }
            of=cf ^ ((res&SIGN[sz])?1:0); break;
        case 1: // ROR
            for(uint32_t i=0;i<cnt;i++){ cf=res&1; res=((res>>1)|((uint32_t)cf*SIGN[sz]))&m; }
            of=((res&SIGN[sz])?1:0) ^ (((res<<1)&SIGN[sz])?1:0); break;
        case 2: // RCL
            for(uint32_t i=0;i<cnt;i++){ int nc=(res&SIGN[sz])?1:0; res=((res<<1)|(CPU.eflags&FL_CF?1:0))&m; setf(FL_CF,nc); cf=nc; }
            of=cf ^ ((res&SIGN[sz])?1:0); break;
        case 3: // RCR
            for(uint32_t i=0;i<cnt;i++){ int nc=res&1; res=(res>>1)|((CPU.eflags&FL_CF?1u:0u)*SIGN[sz]); res&=m; setf(FL_CF,nc); cf=nc; }
            break;
    }
    if(op>=4) set_szp(res,sz);   // shifts affect SZP; rotates (0..3) do not
    if(op<2||op>3) setf(FL_CF,cf);  // RCL/RCR already set CF inside the loop
    setf(FL_OF,of);
    return res&m;
}

// ---------------- x87 (partial) ----------------
static double* st(int i){ return &CPU.st[(CPU.fpu_top+i)&7]; }
static void fpush(double v){ CPU.fpu_top=(CPU.fpu_top-1)&7; CPU.st[CPU.fpu_top]=v; }
static double fpop(void){ double v=CPU.st[CPU.fpu_top]; CPU.fpu_top=(CPU.fpu_top+1)&7; return v; }

static void set_fcom(double a,double b){
    CPU.fpu_sw &= ~((1<<8)|(1<<10)|(1<<14)); // clear C0,C2,C3
    if(a!=a||b!=b) CPU.fpu_sw |= (1<<8)|(1<<10)|(1<<14); // unordered (NaN)
    else if(a<b)  CPU.fpu_sw |= (1<<8);   // C0
    else if(a==b) CPU.fpu_sw |= (1<<14);  // C3
    // a>b: all clear
}
static void set_fcomi(double a,double b){ // sets EFLAGS directly (FCOMI/FUCOMI)
    setf(FL_OF,0); setf(FL_SF,0); setf(FL_AF,0);
    if(a!=a||b!=b){ setf(FL_ZF,1); setf(FL_PF,1); setf(FL_CF,1); }
    else if(a<b){ setf(FL_ZF,0); setf(FL_PF,0); setf(FL_CF,1); }
    else if(a==b){ setf(FL_ZF,1); setf(FL_PF,0); setf(FL_CF,0); }
    else { setf(FL_ZF,0); setf(FL_PF,0); setf(FL_CF,0); }
}
static uint16_t fpu_status(void){ return (CPU.fpu_sw & ~0x3800) | (((uint16_t)CPU.fpu_top&7)<<11); }

static int do_x87(uint8_t op){
    modrm_t m; decode_modrm(&m,0);
    int reg=m.reg;
    if(m.is_mem){
        uint32_t a=m.addr;
        switch(op){
            case 0xD9: // FLD/FST/FSTP m32, FLDCW, FNSTCW, FLDENV/FSTENV
                if(reg==0){ uint32_t b=rd32(a); float f; memcpy(&f,&b,4); fpush(f); return 1; }
                if(reg==2){ float f=(float)*st(0); uint32_t b; memcpy(&b,&f,4); wr32(a,b); return 1; }
                if(reg==3){ float f=(float)*st(0); uint32_t b; memcpy(&b,&f,4); wr32(a,b); fpop(); return 1; }
                if(reg==5){ CPU.fpu_cw=rd16(a); if(EMU_VERBOSE) emu_log("[FLDCW] cw=0x%04x (PC=%d)\n",CPU.fpu_cw,(CPU.fpu_cw>>8)&3); return 1; }   // FLDCW
                if(reg==7){ wr16(a,CPU.fpu_cw); return 1; }   // FNSTCW
                if(reg==4||reg==6){ return 1; }               // FLDENV/FNSTENV (ignore)
                break;
            case 0xDD: // FLD/FST/FSTP m64, FRSTOR/FNSAVE, FNSTSW m16
                if(reg==0){ uint64_t b=rd32(a)|((uint64_t)rd32(a+4)<<32); double d; memcpy(&d,&b,8); fpush(d); return 1; }
                if(reg==2||reg==3){ double d=*st(0); uint64_t b; memcpy(&b,&d,8); wr32(a,(uint32_t)b); wr32(a+4,(uint32_t)(b>>32)); if(reg==3)fpop(); return 1; }
                if(reg==7){ wr16(a,fpu_status()); return 1; } // FNSTSW m16
                if(reg==4||reg==6){ return 1; }
                break;
            case 0xDB: // FILD/FIST/FISTP m32, FLD/FSTP m80
                if(reg==0){ int32_t v=(int32_t)rd32(a); fpush((double)v); return 1; }
                if(reg==2){ int32_t v=(int32_t)fpu_round_rc(*st(0)); wr32(a,(uint32_t)v); return 1; }   // FIST  m32 (per CW RC)
                if(reg==3){ int32_t v=(int32_t)fpu_round_rc(*st(0)); wr32(a,(uint32_t)v); fpop(); return 1; }   // FISTP m32 (per CW RC)
                if(reg==5){ /* FLD m80 -> approx via low 64 mantissa is hard; load as double from 80-bit */
                    uint64_t mant=rd32(a)|((uint64_t)rd32(a+4)<<32); uint16_t se=rd16(a+8);
                    int sign=(se>>15)&1; int exp=(se&0x7fff); double val;
                    if(exp==0&&mant==0) val=0; else { val=(double)mant*pow(2.0,(double)(exp-16383-63)); } if(sign)val=-val; fpush(val); return 1; }
                if(reg==7){ /* FSTP m80 */ double d=*st(0); uint64_t mant; uint16_t se; int sign=d<0; if(sign)d=-d;
                    if(d==0){mant=0;se=0;} else { int e; double mf=frexp(d,&e); mant=(uint64_t)ldexp(mf,64); se=(uint16_t)(e-1+16383); } if(sign)se|=0x8000;
                    wr32(a,(uint32_t)mant); wr32(a+4,(uint32_t)(mant>>32)); wr16(a+8,se); fpop(); return 1; }
                break;
            case 0xDF: // FILD/FIST/FISTP m16, FILD/FISTP m64
                if(reg==0){ int16_t v=(int16_t)rd16(a); fpush((double)v); return 1; }
                if(reg==2){ wr16(a,(uint16_t)(int16_t)fpu_round_rc(*st(0))); return 1; }   // FIST  m16 (per CW RC)
                if(reg==3){ wr16(a,(uint16_t)(int16_t)fpu_round_rc(*st(0))); fpop(); return 1; }   // FISTP m16 (per CW RC)
                if(reg==5){ int64_t v=(int64_t)(rd32(a)|((uint64_t)rd32(a+4)<<32)); fpush((double)v); return 1; }
                if(reg==7){ int64_t v=(int64_t)fpu_round_rc(*st(0)); wr32(a,(uint32_t)v); wr32(a+4,(uint32_t)((uint64_t)v>>32)); fpop(); return 1; }   // FISTP m64 (per CW RC)
                break;
            case 0xD8: case 0xDC: { // float arith with m32 (D8) / m64 (DC)
                double b; if(op==0xD8){ uint32_t bb=rd32(a); float f; memcpy(&f,&bb,4); b=f; }
                          else { uint64_t bb=rd32(a)|((uint64_t)rd32(a+4)<<32); memcpy(&b,&bb,8); }
                switch(reg){
                    case 0:*st(0)+=b;return 1; case 1:*st(0)*=b;return 1;
                    case 2:set_fcom(*st(0),b);return 1; case 3:set_fcom(*st(0),b);fpop();return 1;
                    case 4:*st(0)-=b;return 1; case 5:*st(0)=b-*st(0);return 1;
                    case 6:*st(0)/=b;return 1; case 7:*st(0)=b/ *st(0);return 1;
                } break; }
            case 0xDA: { // int arith with m32
                double b=(double)(int32_t)rd32(a);
                switch(reg){ case 0:*st(0)+=b;return 1;case 1:*st(0)*=b;return 1;
                    case 2:set_fcom(*st(0),b);return 1;case 3:set_fcom(*st(0),b);fpop();return 1;
                    case 4:*st(0)-=b;return 1;case 5:*st(0)=b-*st(0);return 1;
                    case 6:*st(0)/=b;return 1;case 7:*st(0)=b/ *st(0);return 1; } break; }
        }
        return 0;
    } else {
        int i=m.rm; uint8_t full=0xC0|(m.reg<<3)|i;
        switch(op){
            case 0xD8: // st(0) op st(i)
                switch(m.reg){case 0:*st(0)+=*st(i);return 1;case 1:*st(0)*=*st(i);return 1;
                    case 2:set_fcom(*st(0),*st(i));return 1;case 3:set_fcom(*st(0),*st(i));fpop();return 1;
                    case 4:*st(0)-=*st(i);return 1;case 5:*st(0)=*st(i)-*st(0);return 1;
                    case 6:*st(0)/=*st(i);return 1;case 7:*st(0)=*st(i)/ *st(0);return 1;} break;
            case 0xDC: // st(i) op st(0) (sub/div reversed vs D8)
                switch(m.reg){case 0:*st(i)+=*st(0);return 1;case 1:*st(i)*=*st(0);return 1;
                    case 4:*st(i)=*st(0)-*st(i);return 1;case 5:*st(i)-=*st(0);return 1;
                    case 6:*st(i)=*st(0)/ *st(i);return 1;case 7:*st(i)/=*st(0);return 1;} break;
            case 0xDE: // ...P (pop) ; DE D9 = FCOMPP
                if(full==0xD9){ set_fcom(*st(0),*st(1)); fpop(); fpop(); return 1; }
                switch(m.reg){case 0:*st(i)+=*st(0);fpop();return 1;case 1:*st(i)*=*st(0);fpop();return 1;
                    case 2:set_fcom(*st(0),*st(i));fpop();return 1;case 3:set_fcom(*st(0),*st(i));fpop();fpop();return 1;
                    case 4:*st(i)=*st(0)-*st(i);fpop();return 1;case 5:*st(i)-=*st(0);fpop();return 1;
                    case 6:*st(i)=*st(0)/ *st(i);fpop();return 1;case 7:*st(i)/=*st(0);fpop();return 1;} break;
            case 0xD9:
                if(m.reg==0){ double v=*st(i); fpush(v); return 1; }      // FLD st(i)
                if(m.reg==1){ double t=*st(0); *st(0)=*st(i); *st(i)=t; return 1; } // FXCH
                if(m.reg==2||m.reg==3){ return 1; }                      // FNOP/FST st (no-op-ish)
                switch(full){
                    case 0xE0:*st(0)=-*st(0);return 1;   // FCHS
                    case 0xE1:if(*st(0)<0)*st(0)=-*st(0);return 1; // FABS
                    case 0xE4:set_fcom(*st(0),0.0);return 1; // FTST
                    case 0xE5:{ // FXAM: classify ST(0) -> C3,C2,C1,C0 (MSVC exp/pow dispatch on this)
                        // Backported from _emu/cpu.c 2026-06-30. Previous version was a no-op,
                        // which silently mis-dispatched MSVC's `fxam; fnstsw; xlatb; jmp [table]`
                        // pattern for certain inputs. AcuVoice's TTS uses pow/exp during prosody
                        // and concat-cost evaluation, so this can produce audible artefacts.
                        double v=*st(0); uint64_t bits; memcpy(&bits,&v,8);
                        CPU.fpu_sw &= ~((1u<<8)|(1u<<9)|(1u<<10)|(1u<<14)); // clear C0,C1,C2,C3
                        int sign = (bits >> 63) & 1;
                        int exp  = (bits >> 52) & 0x7ff;
                        uint64_t mant = bits & 0x000fffffffffffffull;
                        int c3=0,c2=0,c0=0;
                        if (exp == 0 && mant == 0) { c3=1; c2=0; c0=0; }            // zero
                        else if (exp == 0x7ff && mant == 0) { c3=0; c2=1; c0=1; }   // infinity
                        else if (exp == 0x7ff) { c3=0; c2=0; c0=1; }                // NaN
                        else if (exp == 0) { c3=1; c2=1; c0=0; }                    // denormal
                        else { c3=0; c2=1; c0=0; }                                  // normal
                        if (c0) CPU.fpu_sw |= (1u<<8);
                        if (sign) CPU.fpu_sw |= (1u<<9);   // C1 = sign
                        if (c2) CPU.fpu_sw |= (1u<<10);
                        if (c3) CPU.fpu_sw |= (1u<<14);
                        return 1; }
                    case 0xE8:fpush(1.0);return 1;       // FLD1
                    case 0xE9:fpush(3.321928094887362);return 1; // FLDL2T
                    case 0xEA:fpush(1.442695040888963);return 1; // FLDL2E
                    case 0xEB:fpush(3.141592653589793);return 1; // FLDPI
                    case 0xEC:fpush(0.301029995663981);return 1; // FLDLG2
                    case 0xED:fpush(0.693147180559945);return 1; // FLDLN2
                    case 0xEE:fpush(0.0);return 1;       // FLDZ
                    case 0xF0:*st(0)=pow(2.0,*st(0))-1.0;return 1; // F2XM1
                    case 0xF1:{double y=*st(1),x=*st(0); fpop(); *st(0)=y*(log(x)/log(2.0)); return 1;} // FYL2X
                    case 0xF2:*st(0)=tan(*st(0));fpush(1.0);return 1; // FPTAN
                    case 0xF3:{double y=*st(1),x=*st(0); fpop(); *st(0)=atan2(y,x); return 1;} // FPATAN
                    case 0xFA:*st(0)=sqrt(*st(0));return 1; // FSQRT
                    case 0xFC:*st(0)=fpu_round_rc(*st(0));return 1; // FRNDINT (per CW RC, backported from _emu)
                    // FSCALE: ST(0) = ST(0) * 2^trunc(ST(1)), NO POP.
                    // Backported from _emu/cpu.c 2026-06-30. Previous version popped st(0) and
                    // swapped operands -> broke pow/exp and drifted the FPU stack. Capstone-verified.
                    case 0xFD:*st(0)=ldexp(*st(0),(int)*st(1));return 1; // FSCALE
                    case 0xFE:*st(0)=sin(*st(0));return 1; // FSIN
                    case 0xFF:*st(0)=cos(*st(0));return 1; // FCOS
                    case 0xD0:return 1;                  // FNOP
                }
                if(m.reg==4||m.reg==5||m.reg==6||m.reg==7) return 1; // FLDENV etc reg-form (rare)
                break;
            case 0xDD:
                if(m.reg==0){ return 1; }                  // FFREE
                if(m.reg==2){ *st(i)=*st(0); return 1; }   // FST st(i)
                if(m.reg==3){ *st(i)=*st(0); fpop(); return 1; } // FSTP st(i)
                if(m.reg==4){ set_fcom(*st(0),*st(i)); return 1; } // FUCOM
                if(m.reg==5){ set_fcom(*st(0),*st(i)); fpop(); return 1; } // FUCOMP
                break;
            case 0xDA:
                if(full==0xE9){ set_fcom(*st(0),*st(1)); fpop(); fpop(); return 1; } // FUCOMPP
                // DA C0..DF = FCMOVcc (rare) -> ignore conditionally
                return 1;
            case 0xDB:
                if(full==0xE2){ CPU.fpu_sw=0; return 1; }  // FNCLEX
                if(full==0xE3){ CPU.fpu_top=0; CPU.fpu_sw=0; CPU.fpu_cw=0x037f; return 1; } // FNINIT
                if(full==0xE0||full==0xE1||full==0xE4){ return 1; } // FNENI/FNDISI/FNSETPM
                if(m.reg==5){ set_fcomi(*st(0),*st(i)); return 1; }   // FUCOMI
                if(m.reg==6){ set_fcomi(*st(0),*st(i)); return 1; }   // FCOMI
                break;
            case 0xDF:
                if(full==0xE0){ setreg(EAX,2,fpu_status()); return 1; } // FNSTSW AX
                if(m.reg==5){ set_fcomi(*st(0),*st(i)); fpop(); return 1; } // FUCOMIP
                if(m.reg==6){ set_fcomi(*st(0),*st(i)); fpop(); return 1; } // FCOMIP
                break;
        }
        return 0;
    }
}

// ---------------- main step ----------------
static void trap(const char* what,uint32_t eip0){
    CPU.faulted=1; CPU.fault_msg=what; CPU.halted=1;
    uint8_t b0=rd8(eip0),b1=rd8(eip0+1),b2=rd8(eip0+2),b3=rd8(eip0+3);
    fprintf(stderr,"** CPU TRAP (%s) eip=0x%08x bytes=%02x %02x %02x %02x\n",what,eip0,b0,b1,b2,b3);
}

int ITRACE = 0; static int itrace_count = 0; static uint32_t g_p5 = 0;
static int g_newbt = -1;   // BT/BTS memory-operand byte-addressing fix; EMU_OLDBT=1 forces legacy (buggy) path
int cpu_run(uint64_t max_insns){
    if(g_newbt<0) g_newbt = getenv("EMU_OLDBT") ? 0 : 1;
    for(uint64_t n=0; n<max_insns; n++){
        if(CPU.halted) return CPU.faulted?-1:1;
        if(CPU.eip==RET_SENTINEL) return 1;
        if(ITRACE && CPU.eip>=0x1001b0a0 && CPU.eip<=0x1001b104 && itrace_count<400){
            itrace_count++;
            fprintf(stderr,"%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x\n",
                CPU.eip,CPU.r[EAX],CPU.r[EBX],CPU.r[ECX],CPU.r[EDX],CPU.r[ESI],CPU.r[EDI],CPU.r[EBP]);
        }
        if(EMU_VERBOSE){
            switch(CPU.eip){
            case 0x10046a60: emu_log("[trace] _DllMainCRTStartup(reason=%u)\n",rd32(CPU.r[ESP]+8)); break;
            case 0x100468f0: emu_log("[trace] _CRT_INIT(reason=%u)\n",rd32(CPU.r[ESP]+8)); break;
            case 0x100481c0: emu_log("[trace]   heap_init\n"); break;
            case 0x1004a7e0: emu_log("[trace]   ioinit/mtinit\n"); break;
            case 0x1004b2d0: emu_log("[trace]   env_init\n"); break;
            case 0x1004a9b0: emu_log("[trace]   _cinit (static ctors)\n"); break;
            case 0x10018800: emu_log("[trace] DllMain(hinst=%08x reason=%u)\n",rd32(CPU.r[ESP]+4),rd32(CPU.r[ESP]+8)); break;
            case 0x10018420: emu_log("[trace] config_loader entered\n"); break;
            case 0x10040820: emu_log("[trace] FUN_10040820 (dict/sb setup) entered\n"); break;
            case 0x10042840: emu_log("[trace] load_all_hash_table entered\n"); break;
            #define KEYLOG(tag) do{ char w[48]; uint32_t p=CPU.r[EDI]; int k=0; for(;k<47;k++){char c=(char)rd8(p+k);w[k]=c;if(!c)break;} w[k]=0; emu_log("[key %s] '%s'\n",tag,w); }while(0)
            case 0x10014ca0: { int t=(int)rd32(CPU.r[ESP]+4); int p3=(int)(char)rd32(CPU.r[ESP]+0xc); char s[4][24];
                uint32_t pp[4]={rd32(CPU.r[ESP]+0x10),rd32(CPU.r[ESP]+0x1c),rd32(CPU.r[ESP]+0x20),rd32(CPU.r[ESP]+0x24)};
                for(int a=0;a<4;a++){int k=0;for(;k<23;k++){char c=(char)rd8(pp[a]+k);s[a][k]=c;if(!c)break;}s[a][k]=0;}
                emu_log("[ctxbuild] type=%d p3='%c'(%d) key='%s' p7='%s' p8='%s' p9='%s'\n",t,p3>=32?p3:'.',p3,s[0],s[1],s[2],s[3]); } break;
            case 0x1002f746: { uint32_t base=CPU.r[ESP]+0x20; for(int u=0;u<28;u++){ char w[24]; uint32_t p=base+0x14+u*0x14; int k=0; for(;k<23;k++){char c=(char)rd8(p+k);w[k]=c;if(!c)break;} w[k]=0; if(!w[0]&&u>2)continue; emu_log("[emu11570 %2d] '%s'\n",u,w);} } break;
            case 0x1002f730: { char w[256]; uint32_t p=CPU.r[EDI]; int k=0; for(;k<255;k++){char c=(char)rd8(p+k);w[k]=c;if(!c)break;} w[k]=0; emu_log("[b3a0->11570 in] '%s'\n",w);
                if(getenv("EMU_DUMP11570IN")){ FILE* df=fopen(getenv("EMU_DUMP11570IN"),"wb"); if(df){ fwrite(w,1,k,df); fclose(df); } } } break;
            case 0x1002f743: { char w[256]; uint32_t p=CPU.r[EDI]; int k=0; for(;k<255;k++){char c=(char)rd8(p+k);w[k]=c;if(!c)break;} w[k]=0; emu_log("[after 11570 in] '%s'\n",w); } break;
            case 0x1002f751: KEYLOG("after clear"); break;
            case 0x1002f764: { char w[256]; uint32_t p=CPU.r[EDI]; int k=0; for(;k<255;k++){char c=(char)rd8(p+k);w[k]=c;if(!c)break;} w[k]=0; emu_log("[seg] '%s'\n",w); } break;
            case 0x1001b317: { char a[40],b[40]; uint32_t pa=rd32(CPU.r[ESP]+0),pb=rd32(CPU.r[ESP]+4); int k=0;
                for(;k<39;k++){char c=(char)rd8(pa+k);a[k]=c;if(!c)break;} a[k]=0;
                for(k=0;k<39;k++){char c=(char)rd8(pb+k);b[k]=c;if(!c)break;} b[k]=0;
                emu_log("[ctxcmp] candidate='%s' vs target='%s'\n",a,b); } break;
            case 0x1001af40: { char w[40]; uint32_t pw=rd32(CPU.r[ESP]+4); int k=0; for(;k<39;k++){char c=(char)rd8(pw+k); w[k]=c; if(!c)break;} w[k]=0;
                emu_log("[GetPospPhonenc] word='%s' p6=%08x p7=%08x p8=%08x p9=%08x sz=%u\n", w,
                    rd32(CPU.r[ESP]+0x18),rd32(CPU.r[ESP]+0x1c),rd32(CPU.r[ESP]+0x20),rd32(CPU.r[ESP]+0x24),rd32(CPU.r[ESP]+0x28)); } break;
            case 0x100211c4: { uint32_t off=CPU.r[EBP]; char key[24]; uint32_t kp=CPU.r[ESP]+0x18; int k=0;
                for(;k<23;k++){char c=(char)rd8(kp+k);key[k]=c;if(!c)break;} key[k]=0;
                if(off>=24500 && off<=28000) emu_log("[unit @%6u] '%s'\n", off, key); } break;
            case 0x100152d0: { char p4[48],p5[48]; uint32_t a4=rd32(CPU.r[ESP]+0x10),a5=rd32(CPU.r[ESP]+0x14);
                int k=0; for(;k<47;k++){char c=(char)rd8(a4+k);p4[k]=c;if(!c)break;} p4[k]=0; int l4=k;
                for(k=0;k<47;k++){char c=(char)rd8(a5+k);p5[k]=c;if(!c)break;} p5[k]=0;
                emu_log("[ctxsel] follow_vowel='%s'(len%d) follow_onset='%s'(len%d) onset_bytes=%02x %02x %02x %02x\n",
                    p4,l4,p5,k,rd8(a5),rd8(a5+1),rd8(a5+2),rd8(a5+3)); } break;
            case 0x10015314: { char p5[48]; uint32_t a5=CPU.r[ESI]; int k=0;
                for(;k<47;k++){char c=(char)rd8(a5+k);p5[k]=c;if(!c)break;} p5[k]=0;
                emu_log("[classify] onset='%s' -> class=%d\n", p5, (int)CPU.r[EAX]); } break;
            }
        }
        if(win32_is_import_va(CPU.eip)){ win32_dispatch(CPU.eip); continue; }
        uint32_t eip0=CPU.eip;
        // ---- prefixes ----
        int opsz=4, rep=0, repne=0; g_seg_base=0; int lock=0; (void)lock;
        for(;;){
            uint8_t p=rd8(CPU.eip);
            if(p==0x66){ opsz=2; CPU.eip++; }
            else if(p==0x67){ CPU.eip++; }
            else if(p==0xF0){ lock=1; CPU.eip++; }
            else if(p==0xF2){ repne=1; CPU.eip++; }
            else if(p==0xF3){ rep=1; CPU.eip++; }
            else if(p==0x2E||p==0x36||p==0x3E||p==0x26){ CPU.eip++; } // CS/SS/DS/ES = flat
            else if(p==0x64){ g_seg_base=CPU.seg_fs_base; CPU.eip++; }
            else if(p==0x65){ g_seg_base=CPU.seg_gs_base; CPU.eip++; }
            else break;
        }
        uint8_t op=fetch8();
        modrm_t m;
        switch(op){
        // ----- ALU reg/rm families: 00..3D -----
        #define ALU_GROUP(base,fn) \
            case base+0:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1),b=getreg(m.reg,1);rm_write(&m,1,fn(a,b,1));}break; \
            case base+1:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz),b=getreg(m.reg,opsz);rm_write(&m,opsz,fn(a,b,opsz));}break; \
            case base+2:{decode_modrm(&m,0);uint32_t a=getreg(m.reg,1),b=rm_read(&m,1);setreg(m.reg,1,fn(a,b,1));}break; \
            case base+3:{decode_modrm(&m,0);uint32_t a=getreg(m.reg,opsz),b=rm_read(&m,opsz);setreg(m.reg,opsz,fn(a,b,opsz));}break; \
            case base+4:{uint32_t a=getreg(EAX,1),b=fetch8();setreg(EAX,1,fn(a,b,1));}break; \
            case base+5:{uint32_t a=getreg(EAX,opsz),b=(opsz==2?fetch16():fetch32());setreg(EAX,opsz,fn(a,b,opsz));}break;
        ALU_GROUP(0x00,do_add) ALU_GROUP(0x08,do_logic_or) ALU_GROUP(0x10,do_adc)
        ALU_GROUP(0x18,do_sbb) ALU_GROUP(0x20,do_logic_and) ALU_GROUP(0x28,do_sub)
        ALU_GROUP(0x30,do_logic_xor)
        // CMP (0x38..0x3D): like SUB but discard result
        case 0x38:{decode_modrm(&m,0);do_sub(rm_read(&m,1),getreg(m.reg,1),1);}break;
        case 0x39:{decode_modrm(&m,0);do_sub(rm_read(&m,opsz),getreg(m.reg,opsz),opsz);}break;
        case 0x3A:{decode_modrm(&m,0);do_sub(getreg(m.reg,1),rm_read(&m,1),1);}break;
        case 0x3B:{decode_modrm(&m,0);do_sub(getreg(m.reg,opsz),rm_read(&m,opsz),opsz);}break;
        case 0x3C:{do_sub(getreg(EAX,1),fetch8(),1);}break;
        case 0x3D:{do_sub(getreg(EAX,opsz),(opsz==2?fetch16():fetch32()),opsz);}break;

        case 0x40:case 0x41:case 0x42:case 0x43:case 0x44:case 0x45:case 0x46:case 0x47:
            setreg(op-0x40,opsz,do_inc(getreg(op-0x40,opsz),opsz)); break;
        case 0x48:case 0x49:case 0x4A:case 0x4B:case 0x4C:case 0x4D:case 0x4E:case 0x4F:
            setreg(op-0x48,opsz,do_dec(getreg(op-0x48,opsz),opsz)); break;
        case 0x50:case 0x51:case 0x52:case 0x53:case 0x54:case 0x55:case 0x56:case 0x57:
            cpu_push32(CPU.r[op-0x50]); break;
        case 0x58:case 0x59:case 0x5A:case 0x5B:case 0x5C:case 0x5D:case 0x5E:case 0x5F:
            CPU.r[op-0x58]=cpu_pop32(); break;
        case 0x68: cpu_push32(fetch32()); break;
        case 0x6A: cpu_push32((int32_t)(int8_t)fetch8()); break;
        case 0x69:{decode_modrm(&m,0);int32_t a=(int32_t)rm_read(&m,opsz);int32_t b=(int32_t)(opsz==2?(int16_t)fetch16():(int32_t)fetch32());int64_t r=(int64_t)a*b;setreg(m.reg,opsz,(uint32_t)r);setf(FL_CF,(int64_t)(int32_t)r!=r);setf(FL_OF,(int64_t)(int32_t)r!=r);}break;
        case 0x6B:{decode_modrm(&m,0);int32_t a=(int32_t)rm_read(&m,opsz);int32_t b=(int8_t)fetch8();int64_t r=(int64_t)a*b;setreg(m.reg,opsz,(uint32_t)r);setf(FL_CF,(int64_t)(int32_t)r!=r);setf(FL_OF,(int64_t)(int32_t)r!=r);}break;

        // Jcc short
        case 0x70:case 0x71:case 0x72:case 0x73:case 0x74:case 0x75:case 0x76:case 0x77:
        case 0x78:case 0x79:case 0x7A:case 0x7B:case 0x7C:case 0x7D:case 0x7E:case 0x7F:
            { int8_t d=(int8_t)fetch8(); if(cond(op-0x70)) CPU.eip+=d; } break;

        // group1 80/81/83
        case 0x80:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1),b=fetch8();uint32_t r;switch(m.reg){
            case 0:r=do_add(a,b,1);break;case 1:r=do_logic_or(a,b,1);break;case 2:r=do_adc(a,b,1);break;case 3:r=do_sbb(a,b,1);break;
            case 4:r=do_logic_and(a,b,1);break;case 5:r=do_sub(a,b,1);break;case 6:r=do_logic_xor(a,b,1);break;default:do_sub(a,b,1);r=a;}
            if(m.reg!=7)rm_write(&m,1,r);}break;
        case 0x81:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz),b=(opsz==2?fetch16():fetch32());uint32_t r;switch(m.reg){
            case 0:r=do_add(a,b,opsz);break;case 1:r=do_logic_or(a,b,opsz);break;case 2:r=do_adc(a,b,opsz);break;case 3:r=do_sbb(a,b,opsz);break;
            case 4:r=do_logic_and(a,b,opsz);break;case 5:r=do_sub(a,b,opsz);break;case 6:r=do_logic_xor(a,b,opsz);break;default:do_sub(a,b,opsz);r=a;}
            if(m.reg!=7)rm_write(&m,opsz,r);}break;
        case 0x83:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz),b=(uint32_t)(int32_t)(int8_t)fetch8();uint32_t r;switch(m.reg){
            case 0:r=do_add(a,b,opsz);break;case 1:r=do_logic_or(a,b,opsz);break;case 2:r=do_adc(a,b,opsz);break;case 3:r=do_sbb(a,b,opsz);break;
            case 4:r=do_logic_and(a,b,opsz);break;case 5:r=do_sub(a,b,opsz);break;case 6:r=do_logic_xor(a,b,opsz);break;default:do_sub(a,b,opsz);r=a;}
            if(m.reg!=7)rm_write(&m,opsz,r);}break;

        case 0x84:{decode_modrm(&m,0);do_logic(rm_read(&m,1)&getreg(m.reg,1),1);}break;
        case 0x85:{decode_modrm(&m,0);do_logic(rm_read(&m,opsz)&getreg(m.reg,opsz),opsz);}break;
        case 0x86:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1),b=getreg(m.reg,1);rm_write(&m,1,b);setreg(m.reg,1,a);}break;
        case 0x87:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz),b=getreg(m.reg,opsz);rm_write(&m,opsz,b);setreg(m.reg,opsz,a);}break;
        case 0x88:{decode_modrm(&m,0);rm_write(&m,1,getreg(m.reg,1));}break;
        case 0x89:{decode_modrm(&m,0);rm_write(&m,opsz,getreg(m.reg,opsz));}break;
        case 0x8A:{decode_modrm(&m,0);setreg(m.reg,1,rm_read(&m,1));}break;
        case 0x8B:{decode_modrm(&m,0);setreg(m.reg,opsz,rm_read(&m,opsz));}break;
        case 0x8D:{decode_modrm(&m,0);setreg(m.reg,opsz,m.addr);}break; // LEA (addr already includes seg base 0)
        case 0x8F:{decode_modrm(&m,0);rm_write(&m,opsz,cpu_pop32());}break; // POP r/m

        case 0x9B: break; // FWAIT/WAIT - no-op (FPU never faults here)
        case 0x90: break; // NOP (also XCHG eAX,eAX)
        case 0x91:case 0x92:case 0x93:case 0x94:case 0x95:case 0x96:case 0x97:
            { uint32_t t=CPU.r[EAX]; CPU.r[EAX]=CPU.r[op-0x90]; CPU.r[op-0x90]=t; } break;
        case 0x98: if(opsz==4) CPU.r[EAX]=(int32_t)(int16_t)(CPU.r[EAX]&0xffff); else CPU.r[EAX]=(CPU.r[EAX]&~0xffffu)|((uint32_t)(int16_t)(int8_t)(CPU.r[EAX]&0xff)&0xffff); break; // CWDE/CBW
        case 0x99: if(opsz==4) CPU.r[EDX]=(CPU.r[EAX]&0x80000000)?0xffffffff:0; else setreg(EDX,2,(getreg(EAX,2)&0x8000)?0xffff:0); break; // CDQ/CWD

        case 0xA0:{uint32_t a=fetch32()+g_seg_base;setreg(EAX,1,rd8(a));}break;
        case 0xA1:{uint32_t a=fetch32()+g_seg_base;setreg(EAX,opsz,opsz==2?rd16(a):rd32(a));}break;
        case 0xA2:{uint32_t a=fetch32()+g_seg_base;wr8(a,getreg(EAX,1));}break;
        case 0xA3:{uint32_t a=fetch32()+g_seg_base;if(opsz==2)wr16(a,getreg(EAX,2));else wr32(a,getreg(EAX,4));}break;
        case 0xA8:{do_logic(getreg(EAX,1)&fetch8(),1);}break;
        case 0xA9:{do_logic(getreg(EAX,opsz)&(opsz==2?fetch16():fetch32()),opsz);}break;

        // string ops
        case 0xA4: case 0xA5: { int sz=(op==0xA4)?1:opsz; uint32_t cnt=rep||repne?CPU.r[ECX]:1;
            while(cnt){ uint32_t v=(sz==1?rd8(CPU.r[ESI]):sz==2?rd16(CPU.r[ESI]):rd32(CPU.r[ESI]));
                if(sz==1)wr8(CPU.r[EDI],v);else if(sz==2)wr16(CPU.r[EDI],v);else wr32(CPU.r[EDI],v);
                int d=(CPU.eflags&FL_DF)?-sz:sz; CPU.r[ESI]+=d; CPU.r[EDI]+=d; cnt--; if(rep||repne)CPU.r[ECX]--; if(!(rep||repne))break; }
            } break;
        case 0xAA: case 0xAB: { int sz=(op==0xAA)?1:opsz; uint32_t cnt=rep||repne?CPU.r[ECX]:1; uint32_t val=getreg(EAX,sz);
            while(cnt){ if(sz==1)wr8(CPU.r[EDI],val);else if(sz==2)wr16(CPU.r[EDI],val);else wr32(CPU.r[EDI],val);
                int d=(CPU.eflags&FL_DF)?-sz:sz; CPU.r[EDI]+=d; cnt--; if(rep||repne)CPU.r[ECX]--; if(!(rep||repne))break; }
            } break;
        case 0xAC: case 0xAD: { int sz=(op==0xAC)?1:opsz; uint32_t cnt=rep||repne?CPU.r[ECX]:1;
            while(cnt){ setreg(EAX,sz,(sz==1?rd8(CPU.r[ESI]):sz==2?rd16(CPU.r[ESI]):rd32(CPU.r[ESI])));
                int d=(CPU.eflags&FL_DF)?-sz:sz; CPU.r[ESI]+=d; cnt--; if(rep||repne)CPU.r[ECX]--; if(!(rep||repne))break; }
            } break;
        case 0xAE: case 0xAF: { int sz=(op==0xAE)?1:opsz; uint32_t cnt=(rep||repne)?CPU.r[ECX]:1;
            while(cnt){ uint32_t a=getreg(EAX,sz),b=(sz==1?rd8(CPU.r[EDI]):sz==2?rd16(CPU.r[EDI]):rd32(CPU.r[EDI])); do_sub(a,b,sz);
                int d=(CPU.eflags&FL_DF)?-sz:sz; CPU.r[EDI]+=d; cnt--; if(rep||repne){CPU.r[ECX]--; int zf=!!(CPU.eflags&FL_ZF); if(rep&&!zf)break; if(repne&&zf)break;} else break; }
            } break;
        case 0xA6: case 0xA7: { int sz=(op==0xA6)?1:opsz; uint32_t cnt=(rep||repne)?CPU.r[ECX]:1;
            while(cnt){ uint32_t a=(sz==1?rd8(CPU.r[ESI]):sz==2?rd16(CPU.r[ESI]):rd32(CPU.r[ESI])),b=(sz==1?rd8(CPU.r[EDI]):sz==2?rd16(CPU.r[EDI]):rd32(CPU.r[EDI])); do_sub(a,b,sz);
                int d=(CPU.eflags&FL_DF)?-sz:sz; CPU.r[ESI]+=d; CPU.r[EDI]+=d; cnt--; if(rep||repne){CPU.r[ECX]--; int zf=!!(CPU.eflags&FL_ZF); if(rep&&!zf)break; if(repne&&zf)break;} else break; }
            } break;

        case 0xB0:case 0xB1:case 0xB2:case 0xB3:case 0xB4:case 0xB5:case 0xB6:case 0xB7:
            setreg(op-0xB0,1,fetch8()); break;
        case 0xB8:case 0xB9:case 0xBA:case 0xBB:case 0xBC:case 0xBD:case 0xBE:case 0xBF:
            setreg(op-0xB8,opsz,opsz==2?fetch16():fetch32()); break;

        // group2 shifts
        case 0xC0:{decode_modrm(&m,0);uint32_t v=rm_read(&m,1);uint8_t c=fetch8();rm_write(&m,1,do_shift(m.reg,v,c,1));}break;
        case 0xC1:{decode_modrm(&m,0);uint32_t v=rm_read(&m,opsz);uint8_t c=fetch8();rm_write(&m,opsz,do_shift(m.reg,v,c,opsz));}break;
        case 0xD0:{decode_modrm(&m,0);rm_write(&m,1,do_shift(m.reg,rm_read(&m,1),1,1));}break;
        case 0xD1:{decode_modrm(&m,0);rm_write(&m,opsz,do_shift(m.reg,rm_read(&m,opsz),1,opsz));}break;
        case 0xD2:{decode_modrm(&m,0);rm_write(&m,1,do_shift(m.reg,rm_read(&m,1),CPU.r[ECX]&0xff,1));}break;
        case 0xD3:{decode_modrm(&m,0);rm_write(&m,opsz,do_shift(m.reg,rm_read(&m,opsz),CPU.r[ECX]&0xff,opsz));}break;

        case 0xC2:{uint16_t imm=fetch16();uint32_t ra=cpu_pop32();CPU.r[ESP]+=imm;CPU.eip=ra;}break;
        case 0xC3:{CPU.eip=cpu_pop32();}break;
        case 0xC6:{decode_modrm(&m,0);uint8_t imm=fetch8();rm_write(&m,1,imm);}break;
        case 0xC7:{decode_modrm(&m,0);uint32_t imm=(opsz==2?fetch16():fetch32());rm_write(&m,opsz,imm);}break;
        case 0xC9:{CPU.r[ESP]=CPU.r[EBP];CPU.r[EBP]=cpu_pop32();}break; // LEAVE
        case 0xCC: trap("int3",eip0); break;

        case 0xE8:{int32_t d=(int32_t)fetch32();cpu_push32(CPU.eip);CPU.eip+=d;}break; // CALL rel32
        case 0xE9:{int32_t d=(int32_t)fetch32();CPU.eip+=d;}break; // JMP rel32
        case 0xEB:{int8_t d=(int8_t)fetch8();CPU.eip+=d;}break;    // JMP rel8
        case 0xE3:{int8_t d=(int8_t)fetch8(); if((opsz==2?getreg(ECX,2):CPU.r[ECX])==0) CPU.eip+=d;}break; // JECXZ
        case 0xE0:case 0xE1:case 0xE2:{int8_t d=(int8_t)fetch8();CPU.r[ECX]--;int take=(CPU.r[ECX]!=0);
            if(op==0xE1)take=take&&(CPU.eflags&FL_ZF);if(op==0xE0)take=take&&!(CPU.eflags&FL_ZF);if(take)CPU.eip+=d;}break;

        // group3 F6/F7
        case 0xF6:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1);switch(m.reg){
            case 0:case 1:{uint8_t imm=fetch8();do_logic(a&imm,1);}break;
            case 2:rm_write(&m,1,~a);break;
            case 3:rm_write(&m,1,do_sub(0,a,1));break;
            case 4:{uint32_t r=(getreg(EAX,1)&0xff)*(a&0xff);setreg(EAX,2,r);setf(FL_CF,(r&0xff00)!=0);setf(FL_OF,(r&0xff00)!=0);}break;
            case 5:{int32_t r=(int8_t)getreg(EAX,1)*(int8_t)a;setreg(EAX,2,(uint32_t)r);setf(FL_CF,(int8_t)r!=r);setf(FL_OF,(int8_t)r!=r);}break;
            case 6:{uint32_t ax=getreg(EAX,2);if((a&0xff)==0){trap("div0",eip0);break;}setreg(EAX,1,ax/(a&0xff));setreg(EAX,1,ax/(a&0xff));{uint32_t q=ax/(a&0xff),rem=ax%(a&0xff);setreg(EAX,1,q);setreg(EAX,1,q);CPU.r[EAX]=(CPU.r[EAX]&~0xffffu)|((q&0xff)|((rem&0xff)<<8));}}break;
            case 7:{int16_t ax=(int16_t)getreg(EAX,2);if((a&0xff)==0){trap("idiv0",eip0);break;}int8_t q=ax/(int8_t)a,rem=ax%(int8_t)a;CPU.r[EAX]=(CPU.r[EAX]&~0xffffu)|((uint8_t)q|((uint8_t)rem<<8));}break;
            }}break;
        case 0xF7:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz);switch(m.reg){
            case 0:case 1:{uint32_t imm=(opsz==2?fetch16():fetch32());do_logic(a&imm,opsz);}break;
            case 2:rm_write(&m,opsz,~a);break;
            case 3:rm_write(&m,opsz,do_sub(0,a,opsz));break;
            case 4:{ if(opsz==4){uint64_t r=(uint64_t)CPU.r[EAX]*a;CPU.r[EAX]=(uint32_t)r;CPU.r[EDX]=(uint32_t)(r>>32);setf(FL_CF,CPU.r[EDX]!=0);setf(FL_OF,CPU.r[EDX]!=0);} else {uint32_t r=getreg(EAX,2)*(a&0xffff);setreg(EAX,2,r);setreg(EDX,2,r>>16);setf(FL_CF,(r>>16)!=0);setf(FL_OF,(r>>16)!=0);} }break;
            case 5:{ if(opsz==4){int64_t r=(int64_t)(int32_t)CPU.r[EAX]*(int32_t)a;CPU.r[EAX]=(uint32_t)r;CPU.r[EDX]=(uint32_t)((uint64_t)r>>32);setf(FL_CF,(int64_t)(int32_t)r!=r);setf(FL_OF,(int64_t)(int32_t)r!=r);} else {int32_t r=(int16_t)getreg(EAX,2)*(int16_t)a;setreg(EAX,2,r);setreg(EDX,2,r>>16);} }break;
            case 6:{ if(opsz==4){ if(a==0){trap("div0",eip0);break;} uint64_t num=((uint64_t)CPU.r[EDX]<<32)|CPU.r[EAX]; CPU.r[EAX]=(uint32_t)(num/a); CPU.r[EDX]=(uint32_t)(num%a);} else { uint32_t num=(getreg(EDX,2)<<16)|getreg(EAX,2); if((a&0xffff)==0){trap("div0",eip0);break;} setreg(EAX,2,num/(a&0xffff)); setreg(EDX,2,num%(a&0xffff)); } }break;
            case 7:{ if(opsz==4){ if(a==0){trap("idiv0",eip0);break;} int64_t num=((int64_t)(int32_t)CPU.r[EDX]<<32)|CPU.r[EAX]; CPU.r[EAX]=(uint32_t)(num/(int32_t)a); CPU.r[EDX]=(uint32_t)(num%(int32_t)a);} else { int32_t num=((int32_t)(int16_t)getreg(EDX,2)<<16)|getreg(EAX,2); if((a&0xffff)==0){trap("idiv0",eip0);break;} setreg(EAX,2,(uint32_t)(num/(int16_t)a)); setreg(EDX,2,(uint32_t)(num%(int16_t)a)); } }break;
            }}break;

        case 0xF5: setf(FL_CF,!(CPU.eflags&FL_CF)); break; // CMC
        case 0xF8: setf(FL_CF,0); break;
        case 0xF9: setf(FL_CF,1); break;
        case 0xFC: setf(FL_DF,0); break;
        case 0xFD: setf(FL_DF,1); break;

        case 0xFE:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1);rm_write(&m,1,m.reg==0?do_inc(a,1):do_dec(a,1));}break;
        case 0xFF:{decode_modrm(&m,0);switch(m.reg){
            case 0:rm_write(&m,opsz,do_inc(rm_read(&m,opsz),opsz));break;
            case 1:rm_write(&m,opsz,do_dec(rm_read(&m,opsz),opsz));break;
            case 2:{uint32_t t=rm_read(&m,opsz);cpu_push32(CPU.eip);CPU.eip=t;}break; // CALL r/m
            case 3:{uint32_t t=rm_read(&m,opsz);cpu_push32(CPU.eip);CPU.eip=t;}break; // CALL far (treat near)
            case 4:{CPU.eip=rm_read(&m,opsz);}break; // JMP r/m
            case 5:{CPU.eip=rm_read(&m,opsz);}break; // JMP far (treat near)
            case 6:cpu_push32(rm_read(&m,opsz));break; // PUSH r/m
            }}break;

        case 0x9C: cpu_push32(CPU.eflags); break; // PUSHFD
        case 0x9D: CPU.eflags=(cpu_pop32()&0xcd5)|0x2; break; // POPFD (keep arith/DF/IF-ish)
        case 0x9E:{uint8_t ah=(CPU.r[EAX]>>8)&0xff; CPU.eflags=(CPU.eflags&~0xd5u)|((ah&0xd5)|0x2);}break; // SAHF
        case 0x9F:{uint8_t f=CPU.eflags&0xd5; f|=0x2; CPU.r[EAX]=(CPU.r[EAX]&~0xff00u)|((uint32_t)f<<8);}break; // LAHF
        case 0x60:{uint32_t sp=CPU.r[ESP];cpu_push32(CPU.r[EAX]);cpu_push32(CPU.r[ECX]);cpu_push32(CPU.r[EDX]);cpu_push32(CPU.r[EBX]);cpu_push32(sp);cpu_push32(CPU.r[EBP]);cpu_push32(CPU.r[ESI]);cpu_push32(CPU.r[EDI]);}break; // PUSHAD
        case 0x61:{CPU.r[EDI]=cpu_pop32();CPU.r[ESI]=cpu_pop32();CPU.r[EBP]=cpu_pop32();cpu_pop32();CPU.r[EBX]=cpu_pop32();CPU.r[EDX]=cpu_pop32();CPU.r[ECX]=cpu_pop32();CPU.r[EAX]=cpu_pop32();}break; // POPAD

        // x87 escape range D8..DF
        case 0xD8:case 0xD9:case 0xDA:case 0xDB:case 0xDC:case 0xDD:case 0xDE:case 0xDF:
            if(!do_x87(op)) trap("x87",eip0); break;

        case 0x0F: { uint8_t op2=fetch8();
            if(op2>=0x80 && op2<=0x8F){ int32_t d=(int32_t)(opsz==2?(int16_t)fetch16():(int32_t)fetch32()); if(cond(op2-0x80)) CPU.eip+=d; }
            else if(op2>=0x90 && op2<=0x9F){ decode_modrm(&m,0); rm_write(&m,1,cond(op2-0x90)?1:0); }
            else if(op2>=0x40 && op2<=0x4F){ decode_modrm(&m,0); uint32_t v=rm_read(&m,opsz); if(cond(op2-0x40)) setreg(m.reg,opsz,v); } // CMOVcc
            else switch(op2){
                case 0xB6:{decode_modrm(&m,0);setreg(m.reg,opsz,rm_read(&m,1)&0xff);}break; // MOVZX r,r/m8
                case 0xB7:{decode_modrm(&m,0);setreg(m.reg,opsz,rm_read(&m,2)&0xffff);}break; // MOVZX r,r/m16
                case 0xBE:{decode_modrm(&m,0);setreg(m.reg,opsz,(uint32_t)(int32_t)(int8_t)rm_read(&m,1));}break; // MOVSX r,r/m8
                case 0xBF:{decode_modrm(&m,0);setreg(m.reg,opsz,(uint32_t)(int32_t)(int16_t)rm_read(&m,2));}break; // MOVSX r,r/m16
                case 0xAF:{decode_modrm(&m,0);int32_t a=(int32_t)getreg(m.reg,opsz),b=(int32_t)rm_read(&m,opsz);int64_t r=(int64_t)a*b;setreg(m.reg,opsz,(uint32_t)r);setf(FL_CF,(int64_t)(int32_t)r!=r);setf(FL_OF,(int64_t)(int32_t)r!=r);}break; // IMUL
                case 0xA2: CPU.r[EAX]=CPU.r[EBX]=CPU.r[ECX]=CPU.r[EDX]=0; break; // CPUID stub
                case 0x31: CPU.r[EAX]=0;CPU.r[EDX]=0; break; // RDTSC stub
                // BT/BTS/BTR/BTC with REGISTER bit index. With a MEMORY bit-base the index is a
                // signed bit offset into the bit string at m.addr: byte = addr+(idx>>3), bit = idx&7
                // (idx can exceed 31 - e.g. MSVC strcspn's 256-bit table indexed by a full char byte).
                // With a register bit-base the index is taken modulo the operand size.
                #define BT_SIDX() ((opsz==2)?(int32_t)(int16_t)getreg(m.reg,2):(int32_t)getreg(m.reg,4))
                #define BTNEW (g_newbt && m.is_mem)
                case 0xA3:{decode_modrm(&m,0); if(BTNEW){ int32_t bit=BT_SIDX(); uint8_t byte=rd8(m.addr+(uint32_t)(bit>>3)); setf(FL_CF,(byte>>(bit&7))&1); }
                    else { uint32_t bit=getreg(m.reg,opsz)&(opsz*8-1); uint32_t v=rm_read(&m,opsz); setf(FL_CF,(v>>bit)&1); } }break; // BT
                case 0xAB:{decode_modrm(&m,0); if(BTNEW){ int32_t bit=BT_SIDX(); uint32_t a=m.addr+(uint32_t)(bit>>3); int b=bit&7; uint8_t byte=rd8(a); setf(FL_CF,(byte>>b)&1); wr8(a,byte|(uint8_t)(1u<<b)); }
                    else { uint32_t bit=getreg(m.reg,opsz)&(opsz*8-1); uint32_t v=rm_read(&m,opsz); setf(FL_CF,(v>>bit)&1); rm_write(&m,opsz,v|(1u<<bit)); } }break; // BTS
                case 0xB3:{decode_modrm(&m,0); if(BTNEW){ int32_t bit=BT_SIDX(); uint32_t a=m.addr+(uint32_t)(bit>>3); int b=bit&7; uint8_t byte=rd8(a); setf(FL_CF,(byte>>b)&1); wr8(a,byte&(uint8_t)~(1u<<b)); }
                    else { uint32_t bit=getreg(m.reg,opsz)&(opsz*8-1); uint32_t v=rm_read(&m,opsz); setf(FL_CF,(v>>bit)&1); rm_write(&m,opsz,v&~(1u<<bit)); } }break; // BTR
                case 0xBB:{decode_modrm(&m,0); if(BTNEW){ int32_t bit=BT_SIDX(); uint32_t a=m.addr+(uint32_t)(bit>>3); int b=bit&7; uint8_t byte=rd8(a); setf(FL_CF,(byte>>b)&1); wr8(a,byte^(uint8_t)(1u<<b)); }
                    else { uint32_t bit=getreg(m.reg,opsz)&(opsz*8-1); uint32_t v=rm_read(&m,opsz); setf(FL_CF,(v>>bit)&1); rm_write(&m,opsz,v^(1u<<bit)); } }break; // BTC
                case 0xBA:{decode_modrm(&m,0);uint8_t imm=fetch8();uint32_t bit=imm&(opsz*8-1);uint32_t v=rm_read(&m,opsz);setf(FL_CF,(v>>bit)&1);
                    if(m.reg==5)rm_write(&m,opsz,v|(1u<<bit));else if(m.reg==6)rm_write(&m,opsz,v&~(1u<<bit));else if(m.reg==7)rm_write(&m,opsz,v^(1u<<bit));}break;
                case 0xBC:{decode_modrm(&m,0);uint32_t v=rm_read(&m,opsz);if(v==0){setf(FL_ZF,1);}else{setf(FL_ZF,0);uint32_t i=0;while(!((v>>i)&1))i++;setreg(m.reg,opsz,i);}}break; // BSF
                case 0xBD:{decode_modrm(&m,0);uint32_t v=rm_read(&m,opsz);if(v==0){setf(FL_ZF,1);}else{setf(FL_ZF,0);uint32_t i=opsz*8-1;while(!((v>>i)&1))i--;setreg(m.reg,opsz,i);}}break; // BSR
                case 0xC0:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1),b=getreg(m.reg,1);uint32_t s=do_add(a,b,1);setreg(m.reg,1,a);rm_write(&m,1,s);}break; // XADD r/m8
                case 0xC1:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz),b=getreg(m.reg,opsz);uint32_t s=do_add(a,b,opsz);setreg(m.reg,opsz,a);rm_write(&m,opsz,s);}break; // XADD
                case 0xB0:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1),acc=getreg(EAX,1);do_sub(acc,a,1);if(acc==a)rm_write(&m,1,getreg(m.reg,1));else setreg(EAX,1,a);}break; // CMPXCHG8
                case 0xB1:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz),acc=getreg(EAX,opsz);do_sub(acc,a,opsz);if(acc==a)rm_write(&m,opsz,getreg(m.reg,opsz));else setreg(EAX,opsz,a);}break; // CMPXCHG
                case 0xC8:case 0xC9:case 0xCA:case 0xCB:case 0xCC:case 0xCD:case 0xCE:case 0xCF:{uint32_t v=CPU.r[op2-0xC8];CPU.r[op2-0xC8]=((v&0xff)<<24)|((v&0xff00)<<8)|((v>>8)&0xff00)|((v>>24)&0xff);}break; // BSWAP
                case 0x1F: decode_modrm(&m,0); break; // multi-byte NOP
                case 0x18:case 0x19:case 0x1A:case 0x1B:case 0x1C:case 0x1D:case 0x1E: decode_modrm(&m,0); break; // PREFETCH/NOP hints
                case 0x77: break; // EMMS
                case 0xAE: decode_modrm(&m,0); break; // FXSAVE/LFENCE/etc family - accept+ignore
                default: trap("0F-unknown",eip0); break;
            }
            } break;

        default: trap("opcode",eip0); break;
        }
    }
    return 0;
}

// extra ALU fns referenced by the macro
uint32_t do_logic_or (uint32_t a,uint32_t b,int sz){ return do_logic(a|b,sz); }
uint32_t do_logic_and(uint32_t a,uint32_t b,int sz){ return do_logic(a&b,sz); }
uint32_t do_logic_xor(uint32_t a,uint32_t b,int sz){ return do_logic(a^b,sz); }
