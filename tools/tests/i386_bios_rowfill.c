/* Link the UNMODIFIED pc/i386.c with WYSE_BUILD, not Unicorn or a CPU mock.
 * Replay distributed SeaVGABIOS c000:1d12..1d34, stopping before its epilogue.
 * Independent instruction/state and ordered VGA-write oracles. No hardware.
 * See i386_bios_rowfill.ps1 for the absolute LLVM build/run commands.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "../../pc/i386.h"
#include "../../pc/fpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RAM_SIZE = 2*1024*1024, VGA_BASE = 0xa0000, VGA_SIZE = 0x20000,
       TEXT = 0xb8000, ENTRY = 0x1d12, REP_IP = 0x1d2a, END = 0x1d35 };
/* Verified against the ROM on EVERY run; no hand-assembled substitute. */
static const u8 loop_bytes[] = {
    0x67,0x66,0x89,0x7d,0xec, /* mov [ss:ebp-14h],edi */
    0x66,0x85,0xdb,0x74,0x19, /* test ebx,ebx; jz epilogue */
    0x8e,0xc6,                /* mov es,si */
    0x67,0x66,0x8b,0x4d,0xec, /* mov ecx,[ss:ebp-14h] */
    0x66,0x89,0xd7,           /* mov edi,edx */
    0x67,0x8b,0x45,0xf2,      /* mov ax,[ss:ebp-0eh] */
    0xf3,0xab,0x66,0x4b,      /* rep stosw; dec ebx */
    0x67,0x66,0x03,0x55,0xe8, /* add edx,[ss:ebp-18h] */
    0xeb,0xe2                 /* jmp 1d17 */
};
static int negative_control;
static void unexpected(const char *name) {
    fprintf(stderr,"UNEXPECTED host dependency: %s\n",name); exit(2);
}
/* No FPU/I/O/sleep instruction is part of this fixture. Fail, don't fake it. */
FPU *fpu_new(void) { unexpected("fpu_new"); return NULL; }
void fpu_delete(FPU *f) { (void)f; unexpected("fpu_delete"); }
bool fpu_exec1(FPU *f,void *c,int o,int g,unsigned i) {
    (void)f;(void)c;(void)o;(void)g;(void)i;unexpected("fpu_exec1");return false;
}
bool fpu_exec2(FPU *f,void *c,bool z,int o,int g,int s,u32 a) {
    (void)f;(void)c;(void)z;(void)o;(void)g;(void)s;(void)a;
    unexpected("fpu_exec2");return false;
}
int usleep(unsigned int us) { (void)us;unexpected("usleep");return -1; }
uint32_t time_us_32(void) { static u32 t; return ++t; }
uint64_t time_us_64(void) { return time_us_32(); }
int wyse_rom_write_allowed(uint32_t addr) { (void)addr; return 0; }
void wyse_a20_changed(int enabled) { (void)enabled; }

typedef struct { u32 addr; u16 value; } Write;
typedef struct {
    CPUI386 *cpu;
    u8 vga[VGA_SIZE], expected[VGA_SIZE];
    Write writes[8192];
    unsigned nwrites, consumed, width, rows;
    int stride, callback_error;
    u32 saved_length;
} Bus;
static u8 rd8(void *p,u32 a) { (void)p;(void)a;unexpected("VGA read8");return 0; }
static u16 rd16(void *p,u32 a) { (void)p;(void)a;unexpected("VGA read16");return 0; }
static u32 rd32(void *p,u32 a) { (void)p;(void)a;unexpected("VGA read32");return 0; }
static void wr8(void *p,u32 a,u8 v) {
    (void)p;(void)a;(void)v;unexpected("VGA write8 (expected STOSW)");
}
static void wr32(void *p,u32 a,u32 v) {
    (void)p;(void)a;(void)v;unexpected("VGA write32 (expected STOSW)");
}
static void wr16(void *p,u32 a,u16 v) {
    Bus *b=p;
    b->cpu->burst_mmio++; /* Same activity accounting as Wyse VGA callbacks. */
    if (negative_control==1 && b->nwrites>=80) return;
    if (a < VGA_BASE || a+1 >= VGA_BASE+VGA_SIZE || b->nwrites >= 8192) {
        b->callback_error=1; return;
    }
    b->writes[b->nwrites++]=(Write){a,v};
    b->vga[a-VGA_BASE]=(u8)v; b->vga[a+1-VGA_BASE]=(u8)(v>>8);
}
static void put32(u8 *ram,u32 a,u32 v) { memcpy(ram+a,&v,4); }
static void put16(u8 *ram,u32 a,u16 v) { memcpy(ram+a,&v,2); }

enum { CF_=1, PF_=4, AF_=16, ZF_=64, SF_=128, DF_=1024, OF_=2048 };
static u32 parity(u32 v) {
    v ^= v>>4; v &= 15; return ((0x9669u>>v)&1) ? PF_ : 0;
}
static u32 result_flags(u32 v) {
    return parity(v) | (!v ? ZF_ : 0) | (v&0x80000000u ? SF_ : 0);
}
/* AF is architecturally undefined for TEST; exclude just that bit there. */
static int oracle_step(WyseCpuState *s,Bus *b,u32 *flag_mask) {
    u32 old,v,src; *flag_mask=0xffffffffu;
    switch (s->eip) {
    case 0x1d12: b->saved_length=s->gpr[7]; s->eip=0x1d17; break;
    case 0x1d17:
        s->eflags=(s->eflags & ~(CF_|PF_|ZF_|SF_|OF_)) | result_flags(s->gpr[3]);
        *flag_mask &= ~AF_; s->eip=0x1d1a; break;
    case 0x1d1a: *flag_mask &= ~AF_;
        s->eip=(s->eflags&ZF_) ? END : 0x1d1c; break;
    case 0x1d1c:
        s->seg[SEG_ES].sel=(u16)s->gpr[6];
        s->seg[SEG_ES].base=(s->gpr[6]&0xffffu)<<4;
        s->eip=0x1d1e; *flag_mask &= ~AF_; break;
    case 0x1d1e: s->gpr[1]=b->saved_length; s->eip=0x1d23; *flag_mask &= ~AF_; break;
    case 0x1d23: s->gpr[7]=s->gpr[2]; s->eip=0x1d26; *flag_mask &= ~AF_; break;
    case 0x1d26: s->gpr[0]=(s->gpr[0]&0xffff0000u)|0x0720;
        s->eip=REP_IP; *flag_mask &= ~AF_; break;
    case REP_IP:
        for (unsigned i=0;i<(s->gpr[1]&0xffffu);i++) {
            u32 a=s->seg[SEG_ES].base+(s->gpr[7]&0xffffu);
            if (b->consumed>=b->nwrites || a<VGA_BASE || a+1>=VGA_BASE+VGA_SIZE)
                return 0;
            Write w=b->writes[b->consumed++];
            if (w.addr!=a || w.value!=(u16)s->gpr[0]) {
                fprintf(stderr,"write %u: got %08x/%04x expected %08x/%04x\n",
                        b->consumed-1,w.addr,w.value,a,(u16)s->gpr[0]); return 0;
            }
            b->expected[a-VGA_BASE]=(u8)s->gpr[0];
            b->expected[a+1-VGA_BASE]=(u8)(s->gpr[0]>>8);
            s->gpr[7]=(s->gpr[7]&0xffff0000u)|
                ((s->gpr[7]+((s->eflags&DF_) ? -2 : 2))&0xffffu);
        }
        s->gpr[1]&=0xffff0000u; s->eip=0x1d2c; *flag_mask &= ~AF_; break;
    case 0x1d2c:
        old=s->gpr[3]; v=old-1; s->gpr[3]=v;
        s->eflags=(s->eflags&~(PF_|AF_|ZF_|SF_|OF_))|result_flags(v)|
            (((old^v)&16)?AF_:0)|(old==0x80000000u?OF_:0);
        s->eip=0x1d2e; break;
    case 0x1d2e:
        old=s->gpr[2]; src=(u32)b->stride; v=old+src; s->gpr[2]=v;
        s->eflags=(s->eflags&~(CF_|PF_|AF_|ZF_|SF_|OF_))|result_flags(v)|
            (v<old?CF_:0)|(((old^src^v)&16)?AF_:0)|
            ((~(old^src)&(old^v)&0x80000000u)?OF_:0);
        s->eip=0x1d33; break;
    case 0x1d33: s->eip=0x1d17; break;
    default: return 0;
    }
    return 1;
}
static int same_state(const WyseCpuState *got,const WyseCpuState *want,u32 mask) {
    if (got->eip!=want->eip || ((got->eflags^want->eflags)&mask)) {
        fprintf(stderr,"state IP=%04x/%04x flags=%08x/%08x mask=%08x\n",
                got->eip,want->eip,got->eflags,want->eflags,mask); return 0;
    }
    for (int i=0;i<8;i++) if (got->gpr[i]!=want->gpr[i]) {
        fprintf(stderr,"gpr[%d]=%08x expected %08x at %04x\n",
                i,got->gpr[i],want->gpr[i],got->eip); return 0;
    }
    for (int i=0;i<8;i++) if (got->seg[i].sel!=want->seg[i].sel ||
        got->seg[i].base!=want->seg[i].base || got->seg[i].limit!=want->seg[i].limit ||
        got->seg[i].ar!=want->seg[i].ar) {
        fprintf(stderr,"segment %d mismatch at %04x\n",i,got->eip);return 0;
    }
    return got->cr0==want->cr0 && got->cr2==want->cr2 && got->cr3==want->cr3 &&
        got->cr4==want->cr4 && got->gdt_base==want->gdt_base &&
        got->gdt_limit==want->gdt_limit && got->idt_base==want->idt_base &&
        got->idt_limit==want->idt_limit && got->halted==want->halted;
}
typedef struct {
    const char *name;
    unsigned width,rows,start;
    int stride,dirty,from_rep,separate_ds,reverse;
} Case;
static int run_case(const u8 *rom,size_t rom_size,const Case *c,int mode) {
    static const int budgets[]={1,4,128,128};
    static const int quiet[]={128,128,128,2};
    u8 *ram=calloc(1,RAM_SIZE), *before=malloc(RAM_SIZE);
    Bus *b=calloc(1,sizeof(*b)); CPU_CB *cb;
    if (!ram || !before || !b) unexpected("allocation");
    memcpy(ram+0xc0000,rom,rom_size);
    memset(ram+VGA_BASE,0xcc,VGA_SIZE); /* MUST remain untouched behind MMIO. */
    for (unsigned i=0;i<VGA_SIZE;i++) b->vga[i]=b->expected[i]=(u8)(0xa5^(i*37));
    b->cpu=cpui386_new(5,(char *)ram,RAM_SIZE,&cb);
    b->cpu->burst=0; b->cpu->burst_mmio=0;
    cb->iomem=b; cb->iomem_read8=rd8; cb->iomem_read16=rd16; cb->iomem_read32=rd32;
    cb->iomem_write8=wr8; cb->iomem_write16=wr16; cb->iomem_write32=wr32;
    b->width=c->width; b->rows=c->rows; b->stride=c->stride; b->saved_length=c->width;
    WyseCpuState initial={0},want,got;
    for (int i=0;i<6;i++) {
        initial.seg[i].sel=0xef54; initial.seg[i].base=0xef540;
        initial.seg[i].limit=0xffff; initial.seg[i].ar=0x93;
    }
    initial.seg[SEG_CS].sel=0xc000; initial.seg[SEG_CS].base=0xc0000;
    initial.seg[SEG_CS].ar=0x9b;
    initial.seg[SEG_ES].sel=0xb800; initial.seg[SEG_ES].base=TEXT;
    if (c->separate_ds) { initial.seg[SEG_DS].sel=0x1234; initial.seg[SEG_DS].base=0x12340; }
    initial.seg[SEG_LDT].ar=0x10000;
    initial.seg[SEG_TR].sel=0x28; initial.seg[SEG_TR].ar=0x8b;
    initial.eip=c->from_rep ? REP_IP : ENTRY;
    initial.eflags=0x17002|(c->reverse?DF_:0); initial.cr0=0x10;
    initial.idt_limit=0x3ff;
    initial.gpr[0]=(c->dirty?0xdead0000u:0)|0x0720;
    initial.gpr[1]=(c->dirty?0xa55a0000u:0)|c->width;
    initial.gpr[2]=c->start; initial.gpr[3]=c->rows;
    initial.gpr[4]=c->dirty?0xabcd0184u:0x184;
    /* ef540 + 1019c lands in protected F-segment ROM, not writable RAM.
     * Use 2019c for a valid synthetic dirty upper half (A20 stays on). */
    initial.gpr[5]=c->dirty?0x2019c:0x19c;
    initial.gpr[6]=(c->dirty?0xbeef0000u:0)|0xb800;
    initial.gpr[7]=c->from_rep ? (c->dirty?0xcafe0000u:0)|c->start : c->width;
    u32 locals=initial.seg[SEG_SS].base+initial.gpr[5];
    put32(ram,locals-0x18,(u32)c->stride);
    /* Entry must really execute the saved-length store; do not pre-seed its
     * expected result. A REP-start snapshot already has that local set. */
    put32(ram,locals-0x14,c->from_rep ? c->width : 0xbaadf00du);
    put16(ram,locals-0x0e,0x0720);
    memcpy(before,ram,RAM_SIZE);
    want=initial; cpui386_import_state(b->cpu,&initial);
    memset(&got,0,sizeof(got)); cpui386_export_state(b->cpu,&got);
    int ok=same_state(&got,&want,~0u), steps=0,calls=0;
    u32 mask=~0u;
    while (ok && want.eip!=END && steps<1000) {
        /* Do not let a large budget run the epilogue: stop the burst exactly
         * at the fixture end using its real HLT precheck (ROM copy only). */
        ram[0xc0000+END]=0xf4;
        int n=cpui386_burst(b->cpu,budgets[mode],quiet[mode]); calls++;
        if (n<1 || n>budgets[mode]) { fprintf(stderr,"bad retired count %d\n",n);ok=0;break; }
        for (int i=0;i<n && ok;i++) { ok=oracle_step(&want,b,&mask);steps++; }
        memset(&got,0,sizeof(got)); cpui386_export_state(b->cpu,&got);
        if (negative_control==2 && b->nwrites) got.gpr[2]^=160;
        ok=ok && same_state(&got,&want,mask) && !b->callback_error &&
            b->consumed==b->nwrites && !memcmp(b->vga,b->expected,VGA_SIZE);
        /* Exercise actual native handoff after EVERY burst, including lazy
         * DEC/ADD flags, dirty halves, cached segments and EIP restart. */
        if (ok) { cpui386_import_state(b->cpu,&got); want.eflags=got.eflags; }
    }
    /* Only the saved-length local and our HLT sentinel may alter host RAM. */
    put32(before,locals-0x14,b->saved_length); before[0xc0000+END]=0xf4;
    ok=ok && want.eip==END && b->nwrites==c->width*c->rows &&
        !memcmp(before,ram,RAM_SIZE) && got.gpr[3]==0;
    printf("%s %-22s mode=%d rows=%u width=%u writes=%u steps=%d handoffs=%d EIP=%04x ECX=%08x EDX=%08x EDI=%08x flags=%08x\n",
        ok?"PASS":"FAIL",c->name,mode,c->rows,c->width,b->nwrites,steps,calls,
        got.eip,got.gpr[1],got.gpr[2],got.gpr[7],got.eflags);
    /* cpui386_delete currently leaves its TLB allocation to the caller. */
    free(b->cpu->tlb.tab); cpui386_delete(b->cpu); free(b);free(before);free(ram);
    return ok;
}
int main(int argc,char **argv) {
    const char *path=argc>1?argv[1]:"disks/386/vgabios.bin";
    if (argc>2) {
        if (!strcmp(argv[2],"--drop-after-first-row")) negative_control=1;
        else if (!strcmp(argv[2],"--corrupt-handoff")) negative_control=2;
        else { fprintf(stderr,"Unknown control: %s\n",argv[2]);return 2; }
    }
    FILE *f=fopen(path,"rb"); if (!f) { perror(path);return 2; }
    u8 rom[65536]; size_t size=fread(rom,1,sizeof(rom),f); int extra=fgetc(f);fclose(f);
    unsigned sum=0; for (size_t i=0;i<size;i++) sum+=rom[i];
    if (size<=END || extra!=EOF || rom[0]!=0x55 || rom[1]!=0xaa ||
        size!=(size_t)rom[2]*512 || (sum&255) || memcmp(rom+ENTRY,loop_bytes,sizeof(loop_bytes))) {
        fprintf(stderr,"FAIL: ROM signature/length/checksum/row-loop bytes changed\n");return 2;
    }
    printf("Actual pc/i386.c WYSE_BUILD interpreter; ROM=%s bytes=%zu loop=c000:1d12..1d34\n",path,size);
    const Case cases[]={
        {"snapshot-REP-25x80",80,25,0,160,0,1,0,0},
        {"ROM-entry-25x80",80,25,0,160,0,0,0,0},
        {"dirty-REP-25x80",80,25,0,160,1,1,0,0},
        {"dirty-entry-25x80",80,25,0,160,1,0,0,0},
        {"SS-not-DS-dirty",80,25,0,160,1,0,1,0},
        {"one-row",80,1,0,160,0,0,0,0},
        {"two-rows",80,2,0,160,1,0,0,0},
        {"page1-offset",80,25,4096,160,1,0,0,0},
        {"physical-page-split",80,2,4090,160,1,1,0,0},
        {"unaligned-slow-path",80,25,1,160,1,0,0,0},
        {"partial-with-gaps",17,3,166,160,1,0,0,0},
        {"single-cell-rows",1,25,0,160,1,0,0,0},
        {"zero-length",0,25,0,160,1,0,0,0},
        {"zero-rows",80,0,0,160,1,0,0,0},
        {"reverse-DF",80,25,3998,-160,1,0,0,1}
    };
    if (negative_control) {
        printf("INJECTED NEGATIVE CONTROL %d: expected FAIL/exit 1, not a production defect\n",negative_control);
        return run_case(rom,size,&cases[0],0)?0:1;
    }
    int failures=0;
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++)
        for (int mode=0;mode<4;mode++) failures+=!run_case(rom,size,&cases[i],mode);
    printf("%s: 60 actual-interpreter cases, %d failures; complete aperture/guards, ordered callback writes, RAM, GPRs, flags and native handoffs checked.\n",
           failures?"FAIL":"PASS",failures);
    puts("Scope: exact ROM row-loop, not full INT10 boot, VMX, paging, EMM386 or VGA rendering.");
    return failures?1:0;
}