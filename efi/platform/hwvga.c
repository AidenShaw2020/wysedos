// The Atom's own VGA.
//
// The Cherry Trail graphics still has the legacy VGA core: ports 3C0-3DF and
// memory A0000-BFFFF decode to it, and planes, latches, write modes, chain 4
// and odd/even are all done in hardware. Emulated, every guest store into the
// window was a VM exit or an interpreted instruction - Doom's view, written a
// plane at a time, spent two thirds of the machine there. With this the guest's
// window is mapped straight onto the hardware (a store takes ~125 ns, as on a
// VLB card of the time), and only the register writes still exit: the model
// sees them as before, and the sequencer and graphics controller registers are
// passed on.
//
// The VGA's own display plane stays off (VGACNTRL); the picture is still drawn
// by the model's renderer on core 2, which before each frame reads the VGA's
// memory into vga_ram, laid out the way the model would have stored it.
//
// That memory is the first 256 KB of graphics stolen memory, a dword per
// address with plane p in byte p - which is also the start of the GOP
// framebuffer (a test pattern showed up across the top of the screen). The CPU
// cannot read stolen memory directly, only through the aperture, so:
//  - the GTT entries of the framebuffer's first 256 KB get 256 KB of ordinary
//    RAM instead: the screen and the VGA no longer share;
//  - the VGA's pages are entered again at WIN_PAGE in the aperture, made
//    write-back there by a variable MTRR and flushed line by line before each
//    read: uncached, 256 KB took 87 ms to read; cached, 4.6 ms.
//
// Where the hardware puts a CPU address, measured (and as on IBM's VGA):
// planar at dword A; chain 4 at dword (A & 0xfffc) | A >> 14, plane A & 3
// (Duke Nukem 3D's title showed only its top quarter while this was taken to be
// A & ~3: below 16 KB the two agree); odd/even at dword
// A & ~1 (| 1 when the Misc page bit is clear), plane A & 1. The model keeps
// chain 4 as byte A of vga_ram and odd/even at dword A >> 1.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../include/efi.h"
#include "vga.h"

extern EFI_BOOT_SERVICES *gBS;
bool page_set_type(uint64_t base, uint64_t size, int type);
void wyse_udelay(uint32_t us);

#define WIN_PAGE  0x2000u           // the VGA's pages again, 32 MB into the aperture
#define VGA_BYTES 0x40000u

int g_hwvga_wanted = 1;             // config.ini "hwvga"
int g_hwvga_on;                     // the hardware is in use (hwvga_init succeeded)
int g_hwvga_mtrr_msr = -1;          // the variable MTRR used, for the helper cores (mp.c)
uint64_t g_hwvga_mtrr_base, g_hwvga_mtrr_mask;
uint64_t g_hwvga_fetches, g_hwvga_fetch_tsc;
static const uint32_t *win;         // the VGA's memory, cacheable

static inline uint32_t pci_read(int bus, int dev, int fn, int reg) {
    uint32_t addr = 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xfc);
    __asm__ volatile("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xcf8));
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xcfc));
    return v;
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}

static inline uint64_t rdmsr_(uint32_t m) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
    return (uint64_t)hi << 32 | lo;
}

static inline void wrmsr_(uint32_t m, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

// an index register reads back what was written to it when something decodes it
static bool index_answers(uint16_t port) {
    uint8_t old = inb(port);
    outb(port, 0x05);
    uint8_t a = inb(port);
    outb(port, 0x02);
    uint8_t b = inb(port);
    outb(port, old);
    return (a & 0x1f) == 0x05 && (b & 0x1f) == 0x02;
}

// A variable MTRR, the way the SDM wants it changed: caches off and flushed
static void mtrr_write(int msr, uint64_t base, uint64_t mask) {
    uint64_t flags, cr0;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags));
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0; wbinvd" :: "r"(cr0 | 1ull << 30) : "memory");
    wrmsr_(msr + 1, 0);
    wrmsr_(msr, base);
    wrmsr_(msr + 1, mask);
    __asm__ volatile("wbinvd; mov %0, %%cr0" :: "r"(cr0) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");
}

// On each helper core as it starts: MTRRs are per core
void hwvga_mtrr_apply(void) {
    if (g_hwvga_mtrr_msr >= 0) mtrr_write(g_hwvga_mtrr_msr, g_hwvga_mtrr_base, g_hwvga_mtrr_mask);
}

static bool no(const char *why) {
    printf("hwvga: not used: %s\n", why);
    return false;
}

static inline void flush_window(void) {
    for (uint32_t i = 0; i < VGA_BYTES; i += 64) __asm__ volatile("clflush (%0)" :: "r"((const uint8_t *)win + i));
    __asm__ volatile("mfence" ::: "memory");
}

bool hwvga_init(void) {
    if (!g_hwvga_wanted) return no("hwvga=0");
    if (pci_read(0, 2, 0, 0) != 0x22b08086u) return no("not the Cherry Trail graphics");
    if (pci_read(0, 2, 0, 0x50) & 2) return no("the VGA is disabled (GGC)");
    if ((pci_read(0, 2, 0, 4) & 3) != 3) return no("the graphics does not decode I/O and memory");
    if (!index_answers(0x3c4) || !index_answers(0x3ce)) return no("the VGA registers do not answer");
    uint64_t mmio = ((uint64_t)pci_read(0, 2, 0, 0x14) << 32 | pci_read(0, 2, 0, 0x10)) & ~0xfull;
    uint64_t aper = ((uint64_t)pci_read(0, 2, 0, 0x1c) << 32 | pci_read(0, 2, 0, 0x18)) & ~0xfull;
    if (!mmio || !aper) return no("no graphics BARs");

    // a free variable MTRR
    int slots = (int)(rdmsr_(0xfe) & 0xff), msr = -1;
    for (int i = 0; i < slots && msr < 0; i++)
        if (!(rdmsr_(0x201 + 2 * i) & 0x800)) msr = 0x200 + 2 * i;
    if (msr < 0) return no("no free MTRR");
    EFI_PHYSICAL_ADDRESS ram = 0xffffffffu;
    if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, VGA_BYTES / 4096, &ram)))
        return no("no memory for the top of the screen");
    memset((void *)ram, 0, VGA_BYTES);
    __asm__ volatile("wbinvd");

    // the framebuffer's first 256 KB to the RAM, the VGA's pages to the window
    volatile uint64_t *gtt = (volatile uint64_t *)(mmio + (8u << 20));
    for (uint32_t i = 0; i < VGA_BYTES / 4096; i++) {
        uint64_t pte = gtt[i];
        gtt[WIN_PAGE + i] = pte;
        gtt[i] = (ram + i * 4096u) | (pte & 0xfff);
    }
    (void)gtt[VGA_BYTES / 4096 - 1];
    *(volatile uint32_t *)(mmio + 0x101008) = 1;          // GFX_FLSH_CNTL: the GTT writes take effect
    (void)*(volatile uint32_t *)(mmio + 0x101008);

    // the window write-back (PAT 0 and the MTRR), A0000 uncached for the interpreter
    uint64_t wbase = aper + (uint64_t)WIN_PAGE * 4096;
    if (!page_set_type(wbase, VGA_BYTES, 0) || !page_set_type(0xa0000, 0x20000, 3))
        return no("page attributes");
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000008u), "c"(0));
    uint64_t physmask = ((1ull << (a & 0xff)) - 1) & ~0xfffull;
    g_hwvga_mtrr_base = wbase | 6;
    g_hwvga_mtrr_mask = (physmask & ~(uint64_t)(VGA_BYTES - 1)) | 0x800;
    g_hwvga_mtrr_msr = msr;
    hwvga_mtrr_apply();
    win = (const uint32_t *)wbase;

    // a check: a byte stored at A0000 shows in the window, and not on the screen
    static const uint8_t gr_set[][2] = {{0, 0}, {1, 0}, {3, 0}, {5, 0}, {6, 0x05}, {8, 0xff}};
    outb(0x3c4, 4), outb(0x3c5, 0x06);
    outb(0x3c4, 2), outb(0x3c5, 0x0f);
    for (unsigned i = 0; i < 6; i++) outb(0x3ce, gr_set[i][0]), outb(0x3cf, gr_set[i][1]);
    volatile uint8_t *vga = (volatile uint8_t *)0xa0000;
    vga[0x10] = 0x5a;
    (void)vga[0x10];
    wyse_udelay(20);
    flush_window();
    uint32_t seen = win[0x10], screen = ((volatile uint32_t *)aper)[0x10];
    vga[0x10] = 0;
    printf("hwvga: window %llx (mtrr %x), top of the screen in RAM at %llx, check %08x screen %08x\n",
           (unsigned long long)wbase, msr, (unsigned long long)ram, seen, screen);
    if (seen != 0x5a5a5a5au) return no("the check did not show in the window");
    for (uint32_t i = 0; i < 0x10000; i++) vga[i] = 0;
    (void)vga[0];
    flush_window();
    g_hwvga_on = 1;
    return true;
}

void hwvga_reg(uint16_t port, uint8_t index, uint8_t val) {
    if (port == 0x3c2) {
        outb(0x3c2, val);
        return;
    }
    outb(port, index);
    outb(port + 1, val);
}

// The hardware's index register, for a data port the guest then reaches directly
void hwvga_index(uint16_t port, uint8_t index) { outb(port, index); }

// What the guest wrote straight into a register: the hardware's index must be on it
uint8_t hwvga_read(uint16_t port, uint8_t index) {
    outb(port, index);
    return inb(port + 1);
}

// the registers that decide where a CPU access lands, from the model
void hwvga_sync(VGAState *s) {
    hwvga_reg(0x3c2, 0, s->msr);
    for (int i = 0; i <= 4; i++) hwvga_reg(0x3c4, (uint8_t)i, s->sr[i]);
    for (int i = 0; i <= 8; i++) hwvga_reg(0x3ce, (uint8_t)i, s->gr[i]);
}

// Mode 13h written in vga_ram (byte A = pixel A) into the VGA, where chain 4 puts
// it: plane A & 3 of dword (A & 0xfffc) | A >> 14. For a game that leaves mode
// 13h for a planar one with what it drew still there. The registers are the
// caller's to set again (hwvga_sync).
void hwvga_store_chain4(const uint8_t *src) {
    volatile uint8_t *vga = (volatile uint8_t *)0xa0000;
    outb(0x3c4, 4), outb(0x3c5, 0x06);
    static const uint8_t gr_set[][2] = {{0, 0}, {1, 0}, {3, 0}, {5, 0}, {6, 0x05}, {8, 0xff}};
    for (unsigned i = 0; i < 6; i++) outb(0x3ce, gr_set[i][0]), outb(0x3cf, gr_set[i][1]);
    for (uint32_t p = 0; p < 4; p++) {
        outb(0x3c4, 2), outb(0x3c5, (uint8_t)(1 << p));
        for (uint32_t a = p; a < 0x10000; a += 4) vga[(a & 0xfffc) | a >> 14] = src[a];
    }
}

/* Model's planar layout is four bytes per VGA address. Restore it when
 * leaving the software path; byte stores avoid the Atom's unaligned bug. */
void hwvga_store_planar(const uint8_t *src) {
    volatile uint8_t *vga=(volatile uint8_t *)0xa0000;
    outb(0x3c4,4),outb(0x3c5,0x06);
    static const uint8_t gr_set[][2]={{0,0},{1,0},{3,0},{5,0},{6,5},{8,0xff}};
    for (unsigned i=0;i<6;i++) outb(0x3ce,gr_set[i][0]),outb(0x3cf,gr_set[i][1]);
    for (unsigned p=0;p<4;p++) {
        outb(0x3c4,2),outb(0x3c5,(uint8_t)(1u<<p));
        for (unsigned a=0;a<0x10000;a++) vga[a]=src[a*4+p];
    }
}

// On the display core before a frame: the VGA's memory into vga_ram
void hwvga_fetch(VGAState *s) {
    if (!win || s->vga_ram_size < (int)VGA_BYTES) return;
    uint64_t t0 = __builtin_ia32_rdtsc();
    flush_window();
    uint32_t *dst = (uint32_t *)s->vga_ram;
    const uint32_t *src = win;
    if (s->sr[4] & 0x08) {
        // chain 4: the model's byte A is the hardware's plane A & 3 at dword
        // (A & 0xfffc) | A >> 14 - address bits 14-15 also go to bits 0-1
        for (uint32_t n = 0; n < 0x4000; n++) dst[n] = src[(n << 2 & 0xfffc) | n >> 12];
    } else if ((s->gr[6] & 0x02) || !(s->gr[6] & 0x01)) {
        // odd/even (including text mode temporarily mapped at A0000h with GR6=04h for font load):
        // characters and attributes at every second dword, the fonts planar
        const uint32_t page = (s->gr[6] & 0x02) ? (s->msr & 0x20 ? 0 : 1) : 0;
        for (uint32_t i = 0; i < 0x8000; i++) dst[i] = (src[2 * i + page] & 0xffff) | (src[i] & 0xffff0000u);
        memcpy(dst + 0x8000, src + 0x8000, VGA_BYTES / 2);
    } else {
        memcpy(dst, src, VGA_BYTES);
    }
    g_hwvga_fetches++;
    g_hwvga_fetch_tsc += __builtin_ia32_rdtsc() - t0;
}

// "gtt 1700": the global GTT's entries from there (aperture page numbers), and
// where the entries of the framebuffer (pages 0..2024) stop being consecutive
void hwvga_gtt(uint32_t first) {
    uint64_t mmio = ((uint64_t)pci_read(0, 2, 0, 0x14) << 32 | pci_read(0, 2, 0, 0x10)) & ~0xfull;
    if (!mmio) return;
    volatile uint64_t *gtt = (volatile uint64_t *)(mmio + (8u << 20));
    printf("gtt %u: %llx %llx %llx %llx %llx %llx %llx %llx\n", first, (unsigned long long)gtt[first],
           (unsigned long long)gtt[first + 1], (unsigned long long)gtt[first + 2], (unsigned long long)gtt[first + 3],
           (unsigned long long)gtt[first + 4], (unsigned long long)gtt[first + 5], (unsigned long long)gtt[first + 6],
           (unsigned long long)gtt[first + 7]);
    int breaks = 0;
    for (uint32_t i = 1; i < 2100 && breaks < 8; i++)
        if ((gtt[i] & ~0xfffull) != (gtt[i - 1] & ~0xfffull) + 4096) {
            printf("gtt: break at page %u: %llx after %llx\n", i, (unsigned long long)gtt[i],
                   (unsigned long long)gtt[i - 1]);
            breaks++;
        }
}

// "hwvga 1f40": the VGA's own memory, 16 dwords from there (plane 0 first)
void hwvga_peek(uint32_t dw) {
    if (!win) return;
    flush_window();
    for (int row = 0; row < 2; row++) {
        char buf[200];
        int len = snprintf(buf, sizeof(buf), "hwvga %05x:", dw + row * 8);
        for (int i = 0; i < 8; i++) {
            uint32_t v = win[(dw + row * 8 + i) & 0xffff];
            len += snprintf(buf + len, sizeof(buf) - len, " %02x%02x%02x%02x", v & 0xff, v >> 8 & 0xff,
                            v >> 16 & 0xff, v >> 24);
        }
        printf("%s\n", buf);
    }
}

// "hwvga lag": how long a store at A0000 takes to show in the window, with and
// without a read through A0000 after it. Uses dword 0xf000 (unused in text mode).
void hwvga_lag(void) {
    if (!win) return;
    uint8_t sr[5], gr[9];
    for (int i = 0; i < 5; i++) sr[i] = hwvga_read(0x3c4, (uint8_t)i);
    for (int i = 0; i < 9; i++) gr[i] = hwvga_read(0x3ce, (uint8_t)i);
    static const uint8_t gr_set[][2] = {{0, 0}, {1, 0}, {3, 0}, {5, 0}, {6, 0x05}, {8, 0xff}};
    outb(0x3c4, 4), outb(0x3c5, 0x06);
    outb(0x3c4, 2), outb(0x3c5, 0x0f);
    for (unsigned i = 0; i < 6; i++) outb(0x3ce, gr_set[i][0]), outb(0x3cf, gr_set[i][1]);
    volatile uint8_t *vga = (volatile uint8_t *)0xa0000;
    const volatile uint32_t *w = win + 0xf000;
    uint8_t keep = vga[0xf000];
    uint64_t lag[2];
    for (int pass = 0; pass < 2; pass++) {
        uint8_t v = (uint8_t)(0x31 + pass * 0x22);
        vga[0xf000] = v;
        if (pass) (void)vga[0xf001];             // a read through the VGA after the store
        uint64_t t0 = __builtin_ia32_rdtsc(), t;
        do {
            __asm__ volatile("clflush (%0); mfence" :: "r"(w) : "memory");
            t = __builtin_ia32_rdtsc();
        } while ((*w & 0xff) != v && t - t0 < 2000000000ull);
        lag[pass] = t - t0;
    }
    vga[0xf000] = keep;
    for (int i = 0; i < 5; i++) hwvga_reg(0x3c4, (uint8_t)i, sr[i]);
    for (int i = 0; i < 9; i++) hwvga_reg(0x3ce, (uint8_t)i, gr[i]);
    printf("hwvga: a store shows in the window after %llu ticks, %llu with a read after it\n",
           (unsigned long long)lag[0], (unsigned long long)lag[1]);
}

// "hwvga test": the VGA's operations on the real thing against what a VGA does,
// at dwords 0xf000.. (unused in text mode); the registers are put back after.
static void vset(uint8_t sr4, uint8_t sr2, uint8_t gr0, uint8_t gr1, uint8_t gr3, uint8_t gr4, uint8_t gr5, uint8_t gr8) {
    outb(0x3c4, 4), outb(0x3c5, sr4);
    outb(0x3c4, 2), outb(0x3c5, sr2);
    outb(0x3ce, 0), outb(0x3cf, gr0);
    outb(0x3ce, 1), outb(0x3cf, gr1);
    outb(0x3ce, 3), outb(0x3cf, gr3);
    outb(0x3ce, 4), outb(0x3cf, gr4);
    outb(0x3ce, 5), outb(0x3cf, gr5);
    outb(0x3ce, 6), outb(0x3cf, 0x05);
    outb(0x3ce, 8), outb(0x3cf, gr8);
}

static uint32_t planes_at(volatile uint8_t *vga, uint32_t a) {
    uint32_t v = 0;
    for (int p = 0; p < 4; p++) {
        outb(0x3ce, 4), outb(0x3cf, (uint8_t)p);
        outb(0x3ce, 5), outb(0x3cf, 0);
        v |= (uint32_t)vga[a] << (8 * p);
    }
    return v;
}

static void planes_put(volatile uint8_t *vga, uint32_t a, uint32_t v) {
    vset(0x06, 0x0f, 0, 0, 0, 0, 0, 0xff);
    for (int p = 0; p < 4; p++) {
        outb(0x3c4, 2), outb(0x3c5, (uint8_t)(1 << p));
        vga[a] = (uint8_t)(v >> (8 * p));
    }
}

/* Diagnostic only. Saves/restores registers, indexes and all scratch bytes. */
void hwvga_unaligned(void) {
    if (!g_hwvga_on) { printf("hwvga unaligned: no hardware\n"); return; }
    uint8_t sr[5],gr[9],si=inb(0x3c4),gi=inb(0x3ce);
    for (int i=0;i<5;i++) sr[i]=hwvga_read(0x3c4,(uint8_t)i);
    for (int i=0;i<9;i++) gr[i]=hwvga_read(0x3ce,(uint8_t)i);
    volatile uint8_t *vga=(volatile uint8_t *)0xa0000;
    vset(6,15,0,0,0,0,0,255);
    const unsigned a=0x7300;
    uint32_t saved[16];
    for (unsigned i=0;i<16;i++) saved[i]=planes_at(vga,a+i);
    for (unsigned off=6;off<=7;off++) {
        for (unsigned i=0;i<16;i++) planes_put(vga,a+i,0xcccccccc);
        vset(6,15,0,0,0,0,0,255);
        *(volatile uint16_t *)(vga+a+off)=0x1234;
        uint32_t lo=planes_at(vga,a+off),hi=planes_at(vga,a+off+1);
        printf("hwvga unaligned: planar word at +%u: %08x %08x, expected 34343434 12121212: %s\n",
            off,lo,hi,lo==0x34343434 && hi==0x12121212?"PASS":"DROPPED");
    }
    for (unsigned i=0;i<16;i++) planes_put(vga,a+i,saved[i]);
    for (int i=0;i<5;i++) hwvga_reg(0x3c4,(uint8_t)i,sr[i]);
    for (int i=0;i<9;i++) hwvga_reg(0x3ce,(uint8_t)i,gr[i]);
    outb(0x3c4,si); outb(0x3ce,gi);
}

void hwvga_test(void) {
    uint8_t sr[5], gr[9];
    for (int i = 0; i < 5; i++) sr[i] = hwvga_read(0x3c4, (uint8_t)i);
    for (int i = 0; i < 9; i++) gr[i] = hwvga_read(0x3ce, (uint8_t)i);
    volatile uint8_t *vga = (volatile uint8_t *)0xa0000;
    const uint32_t A = 0xf000, B = 0xf001;
    uint32_t keepA = planes_at(vga, A), keepB = planes_at(vga, B);
    const uint32_t pa = 0x5aa53cc3, pb = 0x0ff0f00f;
    int bad = 0;
#define CHECK(what, expect)                                                                    \
    do {                                                                                       \
        uint32_t got_ = planes_at(vga, B);                                                     \
        printf("hwvga test: %-34s %08x %s %08x\n", what, got_, got_ == (expect) ? "==" : "!=", \
               (uint32_t)(expect));                                                            \
        bad += got_ != (expect);                                                               \
    } while (0)
    // write mode 1: the latches of A into B
    planes_put(vga, A, pa); planes_put(vga, B, pb);
    vset(0x06, 0x0f, 0, 0, 0, 0, 0, 0xff);
    (void)vga[A];
    vset(0x06, 0x0f, 0, 0, 0, 0, 1, 0xff);
    vga[B] = 0x00;
    CHECK("mode 1 latch copy", pa);
    // write mode 0, bit mask 0x0f: low bits from the CPU, high from the latches
    planes_put(vga, A, pa); planes_put(vga, B, pb);
    vset(0x06, 0x0f, 0, 0, 0, 0, 0, 0xff);
    (void)vga[B];
    vset(0x06, 0x0f, 0, 0, 0, 0, 0, 0x0f);
    vga[B] = 0xff;
    CHECK("mode 0 bit mask 0f, ff", (pb & 0xf0f0f0f0u) | 0x0f0f0f0fu);
    // logical ops against the latches (B's own)
    static const char *fn[4] = {"replace", "and", "or", "xor"};
    for (int f = 1; f < 4; f++) {
        planes_put(vga, B, pb);
        vset(0x06, 0x0f, 0, 0, 0, 0, 0, 0xff);
        (void)vga[B];
        vset(0x06, 0x0f, 0, 0, (uint8_t)(f << 3), 0, 0, 0xff);
        vga[B] = 0x3c;
        uint32_t c = 0x3c3c3c3cu, e = f == 1 ? (pb & c) : f == 2 ? (pb | c) : (pb ^ c);
        char what[40];
        snprintf(what, sizeof(what), "mode 0 %s 3c", fn[f]);
        CHECK(what, e);
    }
    // set/reset: planes 0 and 2 from set/reset value 5 (all ones), 1 and 3 from the CPU
    planes_put(vga, B, pb);
    vset(0x06, 0x0f, 0x05, 0x05, 0, 0, 0, 0xff);
    vga[B] = 0x81;
    CHECK("mode 0 set/reset 5 enable 5, 81", 0x81ff81ffu);
    // write mode 2: colour 0x6 into bits under the mask 0xf0
    planes_put(vga, B, pb);
    vset(0x06, 0x0f, 0, 0, 0, 0, 0, 0xff);
    (void)vga[B];
    vset(0x06, 0x0f, 0, 0, 0, 0, 2, 0xf0);
    vga[B] = 0x06;
    CHECK("mode 2 colour 6 mask f0", (pb & 0x0f0f0f0fu) | 0x00f0f000u);
    // write mode 3: set/reset 0xa, the CPU byte (rotated 0) ANDed into the bit mask
    planes_put(vga, B, pb);
    vset(0x06, 0x0f, 0x0a, 0, 0, 0, 0, 0xff);
    (void)vga[B];
    vset(0x06, 0x0f, 0x0a, 0, 0, 0, 3, 0xff);
    vga[B] = 0x3c;
    CHECK("mode 3 sr a, cpu 3c", (pb & ~0x3c3c3c3cu) | 0x3c003c00u);
    // rotate 3 with OR: the CPU byte 0x81 rotated right by 3 = 0x30, ORed into the latches
    planes_put(vga, B, pb);
    vset(0x06, 0x0f, 0, 0, 0, 0, 0, 0xff);
    (void)vga[B];
    vset(0x06, 0x0f, 0, 0, 0x13, 0, 0, 0xff);
    vga[B] = 0x81;
    CHECK("mode 0 rotate 3 or, 81", pb | 0x30303030u);
    // a 16-bit store, mask 0x0f: both bytes take their high bits from the same latches
    planes_put(vga, A, pa); planes_put(vga, B, pb);
    vset(0x06, 0x0f, 0, 0, 0, 0, 0, 0xff);
    (void)vga[A];
    vset(0x06, 0x0f, 0, 0, 0, 0, 0, 0x0f);
    *(volatile uint16_t *)(vga + A) = 0x0505;
    CHECK("16-bit store mask 0f, B byte", (pa & 0xf0f0f0f0u) | 0x05050505u);
    // OR [B], al as one instruction (read: latches; write: CPU's OR result, mode 0)
    planes_put(vga, B, pb);
    vset(0x06, 0x0f, 0, 0, 0, 2, 0, 0xff);       // read map 2
    __asm__ volatile("orb %1, %0" : "+m"(*(volatile uint8_t *)(vga + B)) : "q"((uint8_t)0x11));
    {
        uint8_t r = (uint8_t)((pb >> 16) | 0x11);
        CHECK("or [B], 11 (read map 2)", (uint32_t)r * 0x01010101u);
    }
    // map mask 0x5: only planes 0 and 2
    planes_put(vga, B, pb);
    vset(0x06, 0x05, 0, 0, 0, 0, 0, 0xff);
    vga[B] = 0x99;
    CHECK("map mask 5, 99", (pb & 0xff00ff00u) | 0x00990099u);
    // read mode 1: colour compare 0x5, don't care 0xf, at A
    planes_put(vga, A, pa);
    vset(0x06, 0x0f, 0, 0, 0, 0, 0x08, 0xff);
    outb(0x3ce, 2), outb(0x3cf, 0x05);
    outb(0x3ce, 7), outb(0x3cf, 0x0f);
    uint8_t cc = vga[A];
    uint8_t ecc = 0;
    for (int bit = 0; bit < 8; bit++) {
        int col = 0;
        for (int p = 0; p < 4; p++) col |= ((pa >> (8 * p + bit)) & 1) << p;
        if (col == 5) ecc |= (uint8_t)(1 << bit);
    }
    printf("hwvga test: %-34s %02x %s %02x\n", "read mode 1 compare 5", cc, cc == ecc ? "==" : "!=", ecc);
    bad += cc != ecc;
    outb(0x3ce, 2), outb(0x3cf, gr[2]);
    outb(0x3ce, 7), outb(0x3cf, 0x0f);
    planes_put(vga, A, keepA);
    planes_put(vga, B, keepB);
    for (int i = 0; i < 5; i++) hwvga_reg(0x3c4, (uint8_t)i, sr[i]);
    for (int i = 0; i < 9; i++) hwvga_reg(0x3ce, (uint8_t)i, gr[i]);
    // chain 4 (mode 13h): stores that straddle a dword, and ones that do not
    {
        static const struct { uint32_t a; int size; } st[] = {
            {0xf001, 2}, {0xf003, 2}, {0xf002, 4}, {0xf001, 4}, {0xf007, 2}};
        for (unsigned k = 0; k < sizeof(st) / sizeof(st[0]); k++) {
            vset(0x0e, 0x0f, 0, 0, 0, 0, 0x40, 0xff);
            for (uint32_t i = 0; i < 8; i++) vga[0xf000 + i] = 0;
            uint32_t a = st[k].a;
            if (st[k].size == 2) *(volatile uint16_t *)(vga + a) = 0x6655;
            else *(volatile uint32_t *)(vga + a) = 0x88776655;
            uint8_t got[8];
            for (uint32_t i = 0; i < 8; i++) got[i] = vga[0xf000 + i];
            uint8_t want[8] = {0};
            for (int i = 0; i < st[k].size; i++) want[a - 0xf000 + i] = (uint8_t)(0x55 + 0x11 * i);
            int ok = !memcmp(got, want, 8);
            printf("hwvga test: chain 4 %d-byte store at %05x: %02x %02x %02x %02x %02x %02x %02x %02x %s\n",
                   st[k].size, a, got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7],
                   ok ? "ok" : "WRONG");
            bad += !ok;
            for (uint32_t i = 0; i < 8; i++) vga[0xf000 + i] = 0;
        }
        // and in planar mode: a word at an odd address, both planes 0..3 written
        vset(0x06, 0x0f, 0, 0, 0, 0, 0, 0xff);
        vga[0xf003] = 0; vga[0xf004] = 0;
        *(volatile uint16_t *)(vga + 0xf003) = 0x6655;
        uint32_t p3 = planes_at(vga, 0xf003), p4 = planes_at(vga, 0xf004);
        int ok = p3 == 0x55555555u && p4 == 0x66666666u;
        printf("hwvga test: planar word at 0f003: %08x %08x %s\n", p3, p4, ok ? "ok" : "WRONG");
        bad += !ok;
    }
    printf("hwvga test: %d wrong\n", bad);
#undef CHECK
}

// "hwvga" on the control port
void hwvga_report(void) {
    printf("hwvga: %s, %llu fetches, %llu ticks each\n", g_hwvga_on ? "in use" : "not in use",
           (unsigned long long)g_hwvga_fetches,
           (unsigned long long)(g_hwvga_fetches ? g_hwvga_fetch_tsc / g_hwvga_fetches : 0));
}

//--------------------------------------------------------------------+
// The display engine's own trouble, should it have any: pipe B's FIFO underrun
// flag says whether the plane ran dry. (A band across the bottom of the screen
// in Dračí historie, borders too, looked like that; it was the HDMI grabber's
// MJPEG frames cut short - its decoder's errors start at the band's row - with
// the framebuffer right and no underrun.)
//--------------------------------------------------------------------+

#define VLV_DISPLAY 0x180000u
static volatile uint32_t *disp;     // graphics MMIO
uint32_t g_display_underruns;

static uint32_t punit_read(uint32_t reg) {
    uint32_t mcr = 0x06u << 24 | 0x04u << 16 | (reg & 0xff) << 8 | 0xf0;
    __asm__ volatile("outl %0, %1" :: "a"(0x800000d8u), "Nd"((uint16_t)0xcf8));
    __asm__ volatile("outl %0, %1" :: "a"(reg & 0xffffff00u), "Nd"((uint16_t)0xcfc));
    __asm__ volatile("outl %0, %1" :: "a"(0x800000d0u), "Nd"((uint16_t)0xcf8));
    __asm__ volatile("outl %0, %1" :: "a"(mcr), "Nd"((uint16_t)0xcfc));
    return pci_read(0, 0, 0, 0xd4);
}

static inline uint32_t dreg(uint32_t r) { return disp[(VLV_DISPLAY + r) / 4]; }

void display_watch_init(void) {
    if (pci_read(0, 2, 0, 0) != 0x22b08086u) return;
    uint64_t mmio = ((uint64_t)pci_read(0, 2, 0, 0x14) << 32 | pci_read(0, 2, 0, 0x10)) & ~0xfull;
    if (!mmio) return;
    disp = (volatile uint32_t *)mmio;
    printf("display: pipe B stat %08x, self-refresh %08x, fw %08x %08x %08x %08x %08x %08x, ddl %08x %08x\n",
           dreg(0x71024), dreg(0x6500), dreg(0x70034), dreg(0x70038), dreg(0x7003c), dreg(0x70070), dreg(0x70074),
           dreg(0x70078), dreg(0x70050), dreg(0x70054));
    printf("display: punit ddr setup2 %08x, dsp sspm %08x\n", punit_read(0x139), punit_read(0x36));
}

// every millisecond or so from the main loop
void display_watch_poll(void) {
    if (!disp) return;
    uint32_t st = disp[(VLV_DISPLAY + 0x71024) / 4];
    if (!(st & 0x80000000u)) return;
    disp[(VLV_DISPLAY + 0x71024) / 4] = st;      // the status bits clear by writing them back
    g_display_underruns++;
}

static void punit_write(uint32_t reg, uint32_t val) {
    uint32_t mcr = 0x07u << 24 | 0x04u << 16 | (reg & 0xff) << 8 | 0xf0;
    __asm__ volatile("outl %0, %1" :: "a"(0x800000d4u), "Nd"((uint16_t)0xcf8));
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"((uint16_t)0xcfc));
    __asm__ volatile("outl %0, %1" :: "a"(0x800000d8u), "Nd"((uint16_t)0xcf8));
    __asm__ volatile("outl %0, %1" :: "a"(reg & 0xffffff00u), "Nd"((uint16_t)0xcfc));
    __asm__ volatile("outl %0, %1" :: "a"(0x800000d0u), "Nd"((uint16_t)0xcf8));
    __asm__ volatile("outl %0, %1" :: "a"(mcr), "Nd"((uint16_t)0xcfc));
}

// MB/s of a copy of 8 MB, cache-cold
static uint32_t mem_bandwidth(void) {
    static uint8_t *buf;
    if (!buf) {
        EFI_PHYSICAL_ADDRESS a;
        if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 4096, &a))) return 0;
        buf = (uint8_t *)a;
        memset(buf, 1, 16u << 20);
    }
    uint64_t khz = 0;
    uint64_t wyse_tsc_khz(void);
    khz = wyse_tsc_khz();
    __asm__ volatile("wbinvd");
    uint64_t t0 = __builtin_ia32_rdtsc();
    memcpy(buf + (8u << 20), buf, 8u << 20);
    uint64_t t1 = __builtin_ia32_rdtsc();
    return (uint32_t)(16ull * 1000 * khz / (t1 - t0 + 1));     // bytes read and written
}

// "ddr high" / "ddr low" / "ddr auto": the Punit's DDR frequency request (CHV,
// as i915's chv_set_memory_dvfs). The firmware leaves FORCE_DDR_LOW_FREQ set.
void display_ddr(const char *how) {
    uint32_t before = punit_read(0x139);
    uint32_t bw0 = mem_bandwidth();
    uint32_t v = before & ~3u;
    if (!strncmp(how, "high", 4)) v |= 2;
    else if (!strncmp(how, "low", 3)) v |= 1;
    punit_write(0x139, v | 0x100);
    int waited = 0;
    while ((punit_read(0x139) & 0x100) && waited < 3000) wyse_udelay(10), waited += 10;
    printf("ddr: setup2 %08x -> %08x (ack after %d us), copy %u -> %u MB/s\n", before, punit_read(0x139), waited,
           bw0, mem_bandwidth());
}
