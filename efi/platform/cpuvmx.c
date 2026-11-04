// wyseDOS: the PC model's CPU, run on the real Atom under VT-x.
//
// The X86Pi PC model (pc/) keeps its CPUI386 interpreter object as pc->cpu;
// here that object is only used to emulate the few instructions VT-x hands
// back to us: accesses to emulated memory (VGA window, unmapped space), string
// port I/O and task switches. Everything else runs natively. Port I/O goes to
// the model's callbacks, the 8259 is fed in through VM-entry event injection,
// A20 and the linear framebuffer are EPT mappings.

#include <string.h>
#include <stdio.h>
#include "pc.h"
#include "../src/vm.h"

uint64_t wyse_tsc_khz(void);
static inline uint64_t rdmsr_(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t rdtsc_(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static vm_t vm;
volatile int g_where;   // where the main loop is, for the hang detector: 1 vmx, 2 interpreter, 3 guest, 4 netfs, 5 redraw
static PC *g_pc;
static CPUI386 *icpu;
static volatile bool irq_pending;
static bool halted;
static uint64_t slice_tsc;
static bool a20_on = true;
extern uint32_t g_wyse_vga_rom_size;
static uint64_t rom_write_faults;

/* Unpopulated option-ROM space remains available to the DOS UMB provider.
 * Actual VGA/F-segment ROM shadows obey the chipset's PAM write enables. */
int wyse_rom_write_allowed(uint32_t addr) {
    bool rom=(addr>=0xf0000 && addr<0x100000) ||
        (addr>=0xc0000 && addr<0xc0000+g_wyse_vga_rom_size);
    return !rom || !g_pc || i440fx_pam_write_enabled(g_pc->i440fx,addr);
}
void wyse_cpu_pam_changed(void) {
    if (!vm.ram || !g_pc) return;
    for (uint32_t a=0xc0000;a<0xd0000;a+=4096)
        vm_map(&vm,a,vm.ram+a,4096,wyse_rom_write_allowed(a));
    for (uint32_t a=0xf0000;a<0x100000;a+=4096)
        vm_map(&vm,a,vm.ram+a,4096,wyse_rom_write_allowed(a));
}
void wyse_cpu_rom_report(void) {
    printf("rom: VGA %02x%02x size %u, PAM VGA write %d F write %d, blocked native writes %llu\n",
           vm.ram[0xc0000],vm.ram[0xc0001],g_wyse_vga_rom_size,
           wyse_rom_write_allowed(0xc0000),wyse_rom_write_allowed(0xf0000),
           (unsigned long long)rom_write_faults);
}

// statistics for the log
uint64_t g_task_gate_events;
uint64_t g_last_ept_gpa;        // for the 10 s report     // interrupts delivered through a task gate by the interpreter
int g_redeliver = 1;    // "redeliver 0": an event cut short by an exit is dropped (testing)
uint64_t g_vmx_redelivered, g_vmx_exits[64], g_vmx_bursts, g_vmx_burst_insns, g_vmx_irqs, g_vmx_guest_tsc;
uint64_t g_vmx_emul_tsc, g_vmx_state_tsc, g_vmx_io_tsc;    // time in the interpreter, its state moves, port I/O

// emulated-memory callbacks, counted so a burst knows when the code has moved on
static CPU_CB orig_cb;
// With the real VGA in use (hw_on) the interpreter reaches it too: A0000-BFFFF
// is uncached in the host's map (hwvga.c)
static bool hw_on;
#define HWVGA(a) (hw_on && (uint32_t)(a) - 0xa0000u < 0x20000u)
static u8 cb_mr8(void *o, uword a) {
    icpu->burst_mmio++;
    return HWVGA(a) ? *(volatile u8 *)(uintptr_t)a : orig_cb.iomem_read8(o, a);
}
static void cb_mw8(void *o, uword a, u8 v) {
    icpu->burst_mmio++;
    if (HWVGA(a)) *(volatile u8 *)(uintptr_t)a = v; else orig_cb.iomem_write8(o, a, v);
}
static u16 cb_mr16(void *o, uword a) {
    icpu->burst_mmio++;
    return HWVGA(a) ? *(volatile u16 *)(uintptr_t)a : orig_cb.iomem_read16(o, a);
}
static void cb_mw16(void *o, uword a, u16 v) {
    icpu->burst_mmio++;
    if (HWVGA(a)) *(volatile u16 *)(uintptr_t)a = v; else orig_cb.iomem_write16(o, a, v);
}
static u32 cb_mr32(void *o, uword a) {
    icpu->burst_mmio++;
    return HWVGA(a) ? *(volatile u32 *)(uintptr_t)a : orig_cb.iomem_read32(o, a);
}
static void cb_mw32(void *o, uword a, u32 v) {
    icpu->burst_mmio++;
    if (HWVGA(a)) *(volatile u32 *)(uintptr_t)a = v; else orig_cb.iomem_write32(o, a, v);
}
static bool cb_mws(void *o, uword a, uint8_t *b, int l) {
    icpu->burst_mmio++;
    if (HWVGA(a)) {
        for (int i = 0; i < l; i++) ((volatile u8 *)(uintptr_t)a)[i] = b[i];
        return true;
    }
    return orig_cb.iomem_write_string(o, a, b, l);
}

void wyse_cpu_raise_irq(void *cpu) { (void)cpu; irq_pending = true; }
long wyse_cpu_get_cycle(void *cpu) { (void)cpu; return (long)(rdtsc_() >> 4); }

//--------------------------------------------------------------------+
// guest state <-> interpreter
//--------------------------------------------------------------------+

static uint64_t gpr_get(int i) {
    switch (i) {
    case 0: return vm.regs.rax; case 1: return vm.regs.rcx; case 2: return vm.regs.rdx; case 3: return vm.regs.rbx;
    case 4: return vm_read(VMCS_GUEST_RSP); case 5: return vm.regs.rbp; case 6: return vm.regs.rsi; default: return vm.regs.rdi;
    }
}

static void gpr_set(int i, uint64_t v) {
    switch (i) {
    case 0: vm.regs.rax = v; break; case 1: vm.regs.rcx = v; break; case 2: vm.regs.rdx = v; break;
    case 3: vm.regs.rbx = v; break; case 4: vm_write(VMCS_GUEST_RSP, v); break; case 5: vm.regs.rbp = v; break;
    case 6: vm.regs.rsi = v; break; default: vm.regs.rdi = v; break;
    }
}

static uint32_t guest_cr0(void) {
    uint64_t mask = vm_read(VMCS_CR0_MASK);
    return (uint32_t)((vm_read(VMCS_GUEST_CR0) & ~mask) | (vm_read(VMCS_CR0_SHADOW) & mask));
}

static void set_guest_cr0(uint32_t v) {
    vm_write(VMCS_CR0_SHADOW, v);
    vm_write(VMCS_GUEST_CR0, (v | (1u << 5)) & ~((1u << 29) | (1u << 30)));
}

static void state_from_vmcs(WyseCpuState *s) {
    for (int i = 0; i < 8; i++) s->gpr[i] = (u32)gpr_get(i);
    s->eip = (u32)vm_read(VMCS_GUEST_RIP);
    s->eflags = (u32)vm_read(VMCS_GUEST_RFLAGS);
    for (int i = 0; i < 8; i++) {
        s->seg[i].sel = (u16)vm_read(VMCS_GUEST_ES_SEL + 2 * i);
        s->seg[i].base = (u32)vm_read(VMCS_GUEST_ES_BASE + 2 * i);
        s->seg[i].limit = (u32)vm_read(VMCS_GUEST_ES_LIMIT + 2 * i);
        s->seg[i].ar = (u32)vm_read(VMCS_GUEST_ES_AR + 2 * i);
    }
    s->gdt_base = (u32)vm_read(VMCS_GUEST_GDTR_BASE);
    s->gdt_limit = (u32)vm_read(VMCS_GUEST_GDTR_LIMIT);
    s->idt_base = (u32)vm_read(VMCS_GUEST_IDTR_BASE);
    s->idt_limit = (u32)vm_read(VMCS_GUEST_IDTR_LIMIT);
    s->cr0 = guest_cr0();
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    s->cr2 = (u32)cr2;
    s->cr3 = (u32)vm_read(VMCS_GUEST_CR3);
    s->cr4 = (u32)vm_read(VMCS_CR4_SHADOW);
    s->halted = false;
}

static void state_to_vmcs(const WyseCpuState *s) {
    for (int i = 0; i < 8; i++) gpr_set(i, s->gpr[i]);
    vm_write(VMCS_GUEST_RIP, s->eip);
    vm_write(VMCS_GUEST_RFLAGS, s->eflags | 2);
    for (int i = 0; i < 8; i++) {
        vm_write(VMCS_GUEST_ES_SEL + 2 * i, s->seg[i].sel);
        vm_write(VMCS_GUEST_ES_BASE + 2 * i, s->seg[i].base);
        vm_write(VMCS_GUEST_ES_LIMIT + 2 * i, s->seg[i].limit);
        vm_write(VMCS_GUEST_ES_AR + 2 * i, s->seg[i].ar);
    }
    vm_write(VMCS_GUEST_GDTR_BASE, s->gdt_base);
    vm_write(VMCS_GUEST_GDTR_LIMIT, s->gdt_limit);
    vm_write(VMCS_GUEST_IDTR_BASE, s->idt_base);
    vm_write(VMCS_GUEST_IDTR_LIMIT, s->idt_limit);
    if (s->cr0 != guest_cr0()) set_guest_cr0(s->cr0);
    uint64_t cr2 = s->cr2;
    __asm__ volatile("mov %0, %%cr2" :: "r"(cr2));
    // A CR3 the interpreter loaded - a task switch loads the new task's -
    // has to empty the TLB as the CPU's own load would. Written to the VMCS
    // alone, the guest kept the old task's translations: DOS/4GW
    // Professional's pager runs as a task, and Warcraft II, its pages moved
    // underneath stale entries, faulted at 260:0000000D.
    if ((uint32_t)vm_read(VMCS_GUEST_CR3) != s->cr3) {
        vm_write(VMCS_GUEST_CR3, s->cr3);
        vm_flush_tlb();
    }
    vm_write(VMCS_GUEST_INTERRUPTIBILITY, 0);
}

static int emulate(int max, int quiet) {
    WyseCpuState s;
    uint64_t t0 = rdtsc_();
    state_from_vmcs(&s);
    cpui386_import_state(icpu, &s);
    uint64_t t1 = rdtsc_();
    g_where = 2;
    int n = cpui386_burst(icpu, max, quiet);
    g_where = 1;
    uint64_t t2 = rdtsc_();
    cpui386_export_state(icpu, &s);
    state_to_vmcs(&s);
    uint64_t t3 = rdtsc_();
    g_vmx_emul_tsc += t2 - t1;
    g_vmx_state_tsc += (t1 - t0) + (t3 - t2);
    g_vmx_bursts++;
    g_vmx_burst_insns += n;
    return n;
}

//--------------------------------------------------------------------+
// memory map
//--------------------------------------------------------------------+

void wyse_a20_changed(int enabled) {
    if (!vm.ram || enabled == a20_on) return;
    a20_on = enabled;
    // with A20 off, 1 MB..1 MB+64 KB wraps to the bottom of memory
    vm_map(&vm, 0x100000, vm.ram + (enabled ? 0x100000 : 0), 0x10000, true);
}

void wyse_map_edid(uint32_t addr,int enabled) {
    const uint8_t *guest_edid_page(void);
    static uint32_t old;
    if (!vm.ram) return;
    if (old) vm_map(&vm,old,NULL,4096,false);
    old=0;
    if (enabled && addr && !(addr&4095) && (uint64_t)addr>=vm.ram_size) {
        vm_map(&vm,addr,(void *)guest_edid_page(),4096,false);old=addr;
        printf("display: EDID read-only BAR at %08x\n",addr);
    }
}

void wyse_map_lfb(uint32_t addr, uint32_t len, void *ptr, int enabled) {
    static uint32_t mapped_addr, mapped_len;
    if (!vm.ram) return;
    // The VGA BIOS's mode information gives programs the Bochs VBE address
    // E0000000 for the linear framebuffer, whatever the PCI BAR says: PC Player
    // Benchmark drew there at 640x480, through the interpreter and into the
    // wrong place - the first 64 KB of the picture, then stripes. So the
    // framebuffer is at both.
    const uint32_t bochs = 0xe0000000u;
    if (mapped_len) {
        vm_map(&vm, mapped_addr, NULL, mapped_len, false);
        if (mapped_addr != bochs) vm_map(&vm, bochs, NULL, mapped_len, false);
    }
    mapped_len = 0;
    __atomic_store_n(&g_pc->vga->direct_vram, false, __ATOMIC_RELEASE);
    if (enabled && addr && ptr && len) {
        __atomic_store_n(&g_pc->vga->direct_vram, true, __ATOMIC_RELEASE);
        vm_map(&vm, addr, ptr, len, true);
        if (addr != bochs && (uint64_t)g_pc->phys_mem_size <= bochs) vm_map(&vm, bochs, ptr, len, true);
        mapped_addr = addr;
        mapped_len = len;
        printf("vmx: linear framebuffer at %08x and %08x, %u KB\n", addr, bochs, len >> 10);
    }
}

//--------------------------------------------------------------------+
// One VGA plane written natively. The model keeps the four planes
// interleaved (byte 4n+p is plane p's byte n), which no page mapping can show
// as one plane. But while the VGA is set so that a CPU byte lands unchanged
// in one plane (vga_simple_plane(): Doom draws every column of its view that
// way), the A0000 window is mapped to plane_buf, that plane's bytes laid out
// in a row: a page is gathered from the model when the guest first touches
// it, and scattered back when the settings change (a port write: an exit
// anyway), and every few milliseconds for the display.
//--------------------------------------------------------------------+

static int plane_cur = -1;              // the plane mapped, -1 none
static uint32_t plane_base;             // its offset at A0000 (the VBE bank)
static uint8_t *plane_buf;              // 64 KB, page aligned
static uint32_t plane_mapped;           // pages of the window mapped, bit per 4 KB
static uint64_t plane_flushed_at;
uint64_t g_plane_faults, g_plane_switches;
uint32_t g_vga_keys[8], g_vga_key_n[8];     // VGA settings at interpreted window accesses (report)

static void plane_scatter(void) {
    uint8_t *vram = g_pc->vga->vga_ram;
    for (int pg = 0; pg < 16; pg++) {
        if (!(plane_mapped & (1u << pg))) continue;
        uint8_t *dst = vram + ((uint64_t)plane_base + pg * 4096u) * 4 + plane_cur;
        const uint8_t *src = plane_buf + pg * 4096;
        for (int i = 0; i < 4096; i++) dst[4 * i] = src[i];
    }
}

static void plane_release(void) {
    if (plane_cur < 0) return;
    if (plane_mapped) {
        plane_scatter();
        vm_map(&vm, 0xa0000, NULL, 0x10000, false);     // back to trapping
    }
    plane_mapped = 0;
    plane_cur = -1;
}

//--------------------------------------------------------------------+
// The Atom's own VGA (hwvga.c): while the Bochs VBE extensions are off, the
// whole window A0000-BFFFF goes straight to it.
//--------------------------------------------------------------------+

extern int g_hwvga_on;
void hwvga_sync(VGAState *s);
void hwvga_index(uint16_t port, uint8_t index);
uint8_t hwvga_read(uint16_t port, uint8_t index);

// Registers the display never needs - the map mask (SR2) and the graphics
// controller's set/reset, compare, rotate, read map and bit mask - are written
// thousands of times a frame: Doom switches the map mask for every column it
// draws, 90,000 exits a second at 5 us each, half the machine. While the guest
// has one of them selected, its data port goes to the VGA without an exit; the
// index ports still exit, and on leaving the register its value is read back.
static int pass_sr = -1, pass_gr = -1;     // the index passed through, -1 none
uint64_t g_io_pass_changes;

static void io_pass(int *cur, int want, uint16_t port, uint8_t *regs) {
    if (want == *cur) return;
    if (*cur >= 0) regs[*cur] = hwvga_read(port, (uint8_t)*cur);   // what the guest wrote meanwhile
    if (want >= 0) hwvga_index(port, (uint8_t)want);
    vm_io_trap(port + 1, want < 0);
    *cur = want;
    g_io_pass_changes++;
}

int g_io_pass = 1;      // "iopass 0": every VGA register access exits again (testing)

static void io_pass_update(void) {
    VGAState *v = g_pc->vga;
    int sr = g_io_pass && hw_on && v->sr_index == 2 ? 2 : -1;
    int g = v->gr_index;
    int gr = g_io_pass && hw_on && (g <= 4 || g == 7 || g == 8) ? g : -1;
    io_pass(&pass_sr, sr, 0x3c4, v->sr);
    io_pass(&pass_gr, gr, 0x3ce, v->gr);
}

static void io_pass_off(void) {
    VGAState *v = g_pc->vga;
    io_pass(&pass_sr, -1, 0x3c4, v->sr);
    io_pass(&pass_gr, -1, 0x3ce, v->gr);
}

// Where the window A0000-BFFFF goes:
//  WIN_EMU  trapped, the model's VGA (Bochs VBE modes; anything without the real VGA)
//  WIN_RAM  mode 13h with nothing in the way (vga_chain4_simple): A0000-AFFFF is
//           vga_ram itself, a pixel a byte, at the speed of memory. The real VGA
//           drops a store that straddles 8 bytes - a word at ...7 - and 13h
//           games make those: Prince of Persia lost two pixels of the middle
//           life triangle and pieces of the prince as he got up.
//  WIN_HW   the real VGA: planar and text modes, latches and all
enum { WIN_EMU, WIN_RAM, WIN_HW };
static int win_mode = WIN_EMU;
static bool ram_13h;        // vga_ram holds a mode 13h picture (not a VBE one)
static bool ram_planar;
static int safe_planar = 1;
void hwvga_fetch(VGAState *s);
void hwvga_store_chain4(const uint8_t *src);
void hwvga_store_planar(const uint8_t *src);

static void hw_update(bool resync) {
    VGAState *v = g_pc->vga;
    int want = vga_vbe_on(v) || (safe_planar && vga_native_planar_needs_software(v)) ? WIN_EMU
             : (v->sr[4] & 0x08) ? (vga_chain4_simple(v) ? WIN_RAM : WIN_EMU)
             : g_hwvga_on ? WIN_HW : WIN_EMU;
    if (want == win_mode) {
        if (want == WIN_HW && resync) {
            io_pass_off();
            hwvga_sync(v);
        }
        return;
    }
    io_pass_off();
    plane_release();
    if (win_mode == WIN_HW) {
        // what the guest drew, into vga_ram the way it is laid out from now on
        pc_vga_hold(1);
        hwvga_fetch(v);
        v->hw_mem = 0;
        pc_vga_hold(0);
    } else if (want == WIN_HW && ram_13h && v->vga_ram_size >= 0x10000) {
        // leaving mode 13h: its pixels where chain 4 would have put them
        pc_vga_hold(1);
        hwvga_store_chain4(v->vga_ram);
        pc_vga_hold(0);
    } else if (want == WIN_HW && ram_planar && v->vga_ram_size >= 0x40000) {
        pc_vga_hold(1);
        hwvga_store_planar(v->vga_ram);
        pc_vga_hold(0);
    }
    if (want == WIN_HW) {
        hwvga_sync(v);
        vm_map_mmio(&vm, 0xa0000, 0xa0000, 0x20000);
    } else {
        vm_map(&vm, 0xa0000, NULL, 0x20000, false);
        if (want == WIN_RAM) vm_map(&vm, 0xa0000, v->vga_ram, 0x10000, true);
    }
    if (want != WIN_HW) ram_13h = (v->sr[4] & 0x08) && !vga_vbe_on(v);
    ram_planar = want == WIN_EMU && !vga_vbe_on(v) && !(v->sr[4]&8) && !(v->gr[6]&2);
    win_mode = want;
    hw_on = want == WIN_HW;
    v->hw_mem = hw_on;
    printf("vmx: VGA window %s\n", want == WIN_HW ? "on the real VGA" : want == WIN_RAM ? "in memory (13h)" : "emulated");
}

void wyse_cpu_vga_changed(void) { hw_update(true); }
void wyse_cpu_vga_safety(int enabled) { safe_planar=enabled!=0; hw_update(true); }

// After anything that may have changed the VGA's settings
static void plane_update(void) {
    hw_update(false);
    io_pass_update();
    if (hw_on) return;
    uint32_t base = 0;
    /* A direct page mapping cannot load the VGA latches on reads. Windows'
     * planar driver reads pixels in mode 0 before using those latches for
     * masked drawing and screen copies. Keep that path fully interpreted. */
    int p = safe_planar && vga_native_planar_needs_software(g_pc->vga) ? -1 : vga_simple_plane(g_pc->vga, &base);
    if (p == plane_cur && (p < 0 || base == plane_base)) return;
    plane_release();
    if (p >= 0) {
        plane_cur = p;
        plane_base = base;
        g_plane_switches++;
    }
}

// An access to the window: map its page to the plane, if the VGA allows it
static bool plane_fault(uint64_t gpa) {
    if (plane_cur < 0 || gpa < 0xa0000 || gpa >= 0xb0000 || !plane_buf) return false;
    int pg = (int)((gpa - 0xa0000) >> 12);
    if (plane_mapped & (1u << pg)) return false;
    const uint8_t *src = g_pc->vga->vga_ram + ((uint64_t)plane_base + pg * 4096u) * 4 + plane_cur;
    uint8_t *dst = plane_buf + pg * 4096;
    for (int i = 0; i < 4096; i++) dst[i] = src[4 * i];
    vm_map(&vm, 0xa0000 + pg * 4096u, dst, 4096, true);
    plane_mapped |= 1u << pg;
    g_plane_faults++;
    return true;
}

//--------------------------------------------------------------------+
// exits
//--------------------------------------------------------------------+

static void inject(uint32_t vector, uint32_t type, bool has_err, uint32_t err) {
    vm_write(VMCS_ENTRY_INTR_INFO, (1u << 31) | (type << 8) | vector | (has_err ? (1u << 11) : 0));
    if (has_err) vm_write(VMCS_ENTRY_EXC_ERRCODE, err);
}

static void inject_ud(void) { inject(6, 3, false, 0); }

// "trapexc 1": every guest exception but #DB, #BP and #PF exits ("trapexc 3":
// #PF too); the last 32 outside V86 mode (EMM386 reflects INTs there) are kept
// and logged at a triple fault or with "trapexc 2". For #PF, err is the error
// code and sp the faulting address.
struct exc_rec { uint32_t vec, err, ip, flags, sp; uint16_t cs, ss; };
static struct exc_rec exc_ring[32];
static uint32_t exc_n;

static void exc_dump(void) {
    for (uint32_t i = exc_n > 32 ? exc_n - 32 : 0; i < exc_n; i++) {
        const struct exc_rec *e = &exc_ring[i & 31];
        printf("vmx: exception %u err %x at %04x:%08x flags %x ss:sp %04x:%08x\n", e->vec, e->err, e->cs, e->ip,
               e->flags, e->ss, e->sp);
    }
}

static bool exc_trace;          // trapexc 1/3: interrupts and task switches go in too

static void exc_note(uint32_t vec, uint32_t err) {
    if (vec == 13 && (err & 3) == 2) return;
    if (vm_read(VMCS_GUEST_RFLAGS) & (1u << 17)) return;
    exc_ring[exc_n++ & 31] = (struct exc_rec){vec, err, (uint32_t)vm_read(VMCS_GUEST_RIP),
                                              (uint32_t)vm_read(VMCS_GUEST_RFLAGS),
                                              vec == 14 ? (uint32_t)vm.exit_qual : (uint32_t)vm_read(VMCS_GUEST_RSP),
                                              (uint16_t)vm_read(VMCS_GUEST_CS_SEL),
                                              (uint16_t)vm_read(VMCS_GUEST_SS_SEL)};
}

uint32_t g_gwatch, g_gwatch_left = 2;
void wyse_cpu_gmem(uint32_t lin, uint32_t n);

// "gmem 100c2470 64": guest linear memory through its page tables (32-bit, 4 KB
// and 4 MB pages), a line of 32 bytes at a time
static bool lin_to_phys(uint32_t lin, uint32_t *phys) {
    if (!(guest_cr0() & 0x80000000u)) { *phys = lin; return true; }
    uint32_t cr3 = (uint32_t)vm_read(VMCS_GUEST_CR3) & ~0xfffu;
    if ((uint64_t)cr3 + 4096 > vm.ram_size) return false;
    uint32_t pde = *(uint32_t *)(vm.ram + cr3 + (lin >> 22) * 4);
    if (!(pde & 1)) return false;
    if ((pde & 0x80) && (vm_read(VMCS_CR4_SHADOW) & 0x10)) { *phys = (pde & 0xffc00000u) | (lin & 0x3fffff); return true; }
    uint32_t pt = pde & ~0xfffu;
    if ((uint64_t)pt + 4096 > vm.ram_size) return false;
    uint32_t pte = *(uint32_t *)(vm.ram + pt + ((lin >> 12) & 1023) * 4);
    if (!(pte & 1)) return false;
    *phys = (pte & ~0xfffu) | (lin & 0xfff);
    return true;
}

void wyse_cpu_gmem(uint32_t lin, uint32_t n) {
    if (n > 512) n = 512;
    for (uint32_t off = 0; off < n; off += 32) {
        char buf[160];
        int len = snprintf(buf, sizeof(buf), "gmem %08x:", lin + off);
        for (uint32_t i = 0; i < 32; i++) {
            uint32_t p;
            if (lin_to_phys(lin + off + i, &p) && p < vm.ram_size)
                len += snprintf(buf + len, sizeof(buf) - len, "%02x", vm.ram[p]);
            else
                len += snprintf(buf + len, sizeof(buf) - len, "..");
        }
        printf("%s\n", buf);
    }
    printf("gmem: cr3 %08x idtr %08x/%x gdtr %08x tr %04x\n", (unsigned)vm_read(VMCS_GUEST_CR3),
           (unsigned)vm_read(VMCS_GUEST_IDTR_BASE), (unsigned)vm_read(VMCS_GUEST_IDTR_LIMIT),
           (unsigned)vm_read(VMCS_GUEST_GDTR_BASE), (unsigned)vm_read(VMCS_GUEST_TR_SEL));
}

void wyse_cpu_trap_exceptions(int on) {
    if (on == 2) {          // "trapexc 2": the last ones now
        exc_dump();
        return;
    }
    exc_n = 0;
    exc_trace = on != 0;
    uint32_t bm = ~((1u << 1) | (1u << 3) | (1u << 14));
    if (on == 3) bm |= 1u << 14;
    vm_write(VMCS_EXCEPTION_BITMAP, on ? bm : 0);
}

// "mtf <eip> <n>": from the n-th interrupt injected at eip on, the guest runs
// one instruction per entry (monitor trap flag); the last 256 are kept and
// logged when 32-bit protected-mode code lands below 1000h (a jump to the IVT)
struct mtf_rec { uint32_t ip, sp, flags, code; uint16_t cs, ss; };
static struct mtf_rec mtf_ring[256];
static uint32_t mtf_n, mtf_at, mtf_skip;
static bool mtf_on;

void wyse_cpu_mtf(uint32_t eip, uint32_t nth) {
    mtf_at = eip;
    mtf_skip = nth ? nth - 1 : 0;
    mtf_n = 0;
    mtf_on = false;
    vm_write(VMCS_PROC_CTLS, vm_read(VMCS_PROC_CTLS) & ~PROC_MONITOR_TRAP);
}

static void mtf_dump(void) {
    uint32_t n = mtf_n < 256 ? mtf_n : 256;
    printf("mtf: %u steps, the last %u\n", mtf_n, n);
    for (uint32_t i = mtf_n - n; i < mtf_n; i++) {
        const struct mtf_rec *r = &mtf_ring[i & 255];
        printf("mtf %04x:%08x %08x ss:sp %04x:%08x fl %x\n", r->cs, r->ip,
               __builtin_bswap32(r->code), r->ss, r->sp, r->flags);
    }
}

static void mtf_step(void) {
    uint32_t ip = (uint32_t)vm_read(VMCS_GUEST_RIP), fl = (uint32_t)vm_read(VMCS_GUEST_RFLAGS);
    uint32_t lin = (uint32_t)vm_read(VMCS_GUEST_CS_BASE) + ip, code = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t p;
        if (lin_to_phys(lin + i, &p) && p < vm.ram_size) code |= (uint32_t)vm.ram[p] << (8 * i);
    }
    mtf_ring[mtf_n++ & 255] = (struct mtf_rec){ip, (uint32_t)vm_read(VMCS_GUEST_RSP), fl, code,
                                               (uint16_t)vm_read(VMCS_GUEST_CS_SEL),
                                               (uint16_t)vm_read(VMCS_GUEST_SS_SEL)};
    bool pm32 = (guest_cr0() & 1) && !(fl & (1u << 17)) && (vm_read(VMCS_GUEST_CS_AR) & (1u << 14));
    if ((pm32 && ip < 0x1000) || mtf_n >= 20000000) {
        vm_write(VMCS_PROC_CTLS, vm_read(VMCS_PROC_CTLS) & ~PROC_MONITOR_TRAP);
        mtf_on = false;
        mtf_at = 0;
        mtf_dump();
    }
}

static void do_cpuid(void) {
    uint32_t leaf = (uint32_t)vm.regs.rax, sub = (uint32_t)vm.regs.rcx;
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(sub));
    if (leaf == 0) {
        if (a > 0xd) a = 0xd;
    } else if (leaf == 1) {
        b &= 0x0000ffff;        // one logical CPU, APIC ID 0
        c &= ~((1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 14) | (1u << 15) |
               (1u << 21) | (1u << 24) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 31));
        d &= ~((1u << 3) | (1u << 6) | (1u << 9) | (1u << 17) | (1u << 21) | (1u << 22) |
               (1u << 27) | (1u << 28) | (1u << 29) | (1u << 31));
    } else if (leaf == 0x80000001) {
        d &= ~((1u << 11) | (1u << 20) | (1u << 27) | (1u << 29));     // no syscall, NX, rdtscp, long mode
        c &= ~(1u << 0);
    } else if (leaf >= 0x40000000 && leaf < 0x50000000) {
        a = b = c = d = 0;
    } else if (leaf == 0xb || leaf == 0x1f || leaf == 4) {
        a = b = c = d = 0;
    }
    vm.regs.rax = a; vm.regs.rbx = b; vm.regs.rcx = c; vm.regs.rdx = d;
}

static void do_cr_access(void) {
    uint64_t q = vm.exit_qual;
    int cr = q & 15, type = (q >> 4) & 3, reg = (q >> 8) & 15;
    if (type == 0) {        // mov to cr
        uint32_t v = (uint32_t)gpr_get(reg);
        if (cr == 0) {
            set_guest_cr0(v);
        } else if (cr == 4) {
            vm_write(VMCS_CR4_SHADOW, v);
            vm_write(VMCS_GUEST_CR4, (v | (1u << 13)) & ~((1u << 5) | (1u << 17) | (1u << 18)));
        }
    } else if (type == 1) { // mov from cr (cr3 store exiting is off; nothing else exits)
        gpr_set(reg, cr == 0 ? guest_cr0() : cr == 4 ? vm_read(VMCS_CR4_SHADOW) : vm_read(VMCS_GUEST_CR3));
    } else if (type == 3) { // lmsw
        uint32_t src = (q >> 16) & 0xffff;
        uint32_t cr0 = guest_cr0();
        set_guest_cr0((cr0 & ~0xeu) | (src & 0xf) | (cr0 & 1));
    }
}

uint32_t g_io_count[2][0x400];     // exits per port (reads, writes), for the report

static uint16_t iot_lo[8], iot_hi[8];
static uint32_t iot_ranges;
static void iot_note(int port, bool write, int size, uint32_t val);

static void do_io(void) {
    uint64_t q = vm.exit_qual;
    g_io_count[!(q & 8)][(q >> 16) & 0x3ff]++;
    if (q & (1 << 4)) {         // string I/O (ins/outs, rep): the interpreter does it whole
        emulate(1, 1);
        return;
    }
    int size = (q & 7) + 1;
    int port = (q >> 16) & 0xffff;
    CPU_CB *cb = &icpu->cb;
    if (q & 8) {
        uint64_t v;
        if (size == 1) v = cb->io_read8(cb->io, port), vm.regs.rax = (vm.regs.rax & ~0xffULL) | v;
        else if (size == 2) v = cb->io_read16(cb->io, port), vm.regs.rax = (vm.regs.rax & ~0xffffULL) | v;
        else v = cb->io_read32(cb->io, port), vm.regs.rax = v & 0xffffffffULL;
    } else {
        if (size == 1) cb->io_write8(cb->io, port, (u8)vm.regs.rax);
        else if (size == 2) cb->io_write16(cb->io, port, (u16)vm.regs.rax);
        else cb->io_write32(cb->io, port, (u32)vm.regs.rax);
    }
    if (iot_ranges) iot_note(port, !(q & 8), size, (uint32_t)vm.regs.rax);
    vm_skip_instr(&vm);
}

// "iotrace 220-22f 0-f c0-df": the last 16384 port accesses in up to eight
// ranges are kept (port, value, CS:IP); "iotrace" logs them, "iotrace off" stops
struct iot_rec { uint32_t ip, val; uint16_t cs, port; uint8_t write, size; };
static struct iot_rec iot_ring[16384];
static uint32_t iot_n;

static void iot_note(int port, bool write, int size, uint32_t val) {
    for (uint32_t i = 0; i < iot_ranges; i++) {
        if (port < iot_lo[i] || port > iot_hi[i]) continue;
        iot_ring[iot_n++ & 16383] = (struct iot_rec){(uint32_t)vm_read(VMCS_GUEST_RIP), val,
                                                    (uint16_t)vm_read(VMCS_GUEST_CS_SEL), (uint16_t)port, write,
                                                    (uint8_t)size};
        return;
    }
}

static void iot_dump(void) {
    uint32_t n = iot_n < 16384 ? iot_n : 16384;
    printf("iotrace: %u accesses, the last %u\n", iot_n, n);
    for (uint32_t i = iot_n - n; i < iot_n; i++) {
        const struct iot_rec *r = &iot_ring[i & 16383];
        printf("iot %04x:%08x %s%u %03x %0*x\n", r->cs, r->ip, r->write ? "w" : "r", r->size, r->port,
               r->size * 2, r->val & (r->size == 4 ? 0xffffffffu : (1u << (8 * r->size)) - 1));
    }
}

void wyse_cpu_iotrace(const char *args) {
    while (*args == ' ') args++;
    if (!*args) {
        iot_dump();
        return;
    }
    iot_ranges = 0;
    iot_n = 0;
    if (!strncmp(args, "off", 3)) return;
    char *end;
    while (iot_ranges < 8) {
        uint32_t lo = (uint32_t)strtoul(args, &end, 16);
        if (end == args) break;
        uint32_t hi = lo;
        if (*end == '-') hi = (uint32_t)strtoul(end + 1, &end, 16);
        iot_lo[iot_ranges] = (uint16_t)lo;
        iot_hi[iot_ranges] = (uint16_t)hi;
        iot_ranges++;
        args = end;
        while (*args == ' ') args++;
    }
    printf("iotrace: %u ranges\n", iot_ranges);
}

static void dump_guest(const char *why) {
    printf("vmx: %s; exit %u qual %llx cs:ip %04llx:%08llx (base %08llx) cr0 %llx rflags %llx gpa %llx\n", why,
           vm.exit_reason & 0xffff, (unsigned long long)vm.exit_qual,
           (unsigned long long)vm_read(VMCS_GUEST_CS_SEL), (unsigned long long)vm_read(VMCS_GUEST_RIP),
           (unsigned long long)vm_read(VMCS_GUEST_CS_BASE), (unsigned long long)vm_read(VMCS_GUEST_CR0),
           (unsigned long long)vm_read(VMCS_GUEST_RFLAGS), (unsigned long long)vm.exit_gpa);
    for (int i = 0; i < 8; i++)
        printf("  seg%d sel %04llx base %08llx limit %08llx ar %05llx\n", i,
               (unsigned long long)vm_read(VMCS_GUEST_ES_SEL + 2 * i), (unsigned long long)vm_read(VMCS_GUEST_ES_BASE + 2 * i),
               (unsigned long long)vm_read(VMCS_GUEST_ES_LIMIT + 2 * i), (unsigned long long)vm_read(VMCS_GUEST_ES_AR + 2 * i));
}

// Offer the 8259's interrupt if the guest can take it now; otherwise ask for an
// exit the moment it can.
static void deliver_irq(void) {
    if (!irq_pending) return;
    uint64_t rflags = vm_read(VMCS_GUEST_RFLAGS);
    uint64_t intr = vm_read(VMCS_GUEST_INTERRUPTIBILITY);
    uint32_t proc = (uint32_t)vm_read(VMCS_PROC_CTLS);
    if ((rflags & 0x200) && !(intr & 3) && !(vm_read(VMCS_ENTRY_INTR_INFO) & (1u << 31))) {
        irq_pending = false;
        int vec = icpu->cb.pic_read_irq(icpu->cb.pic);
        if (vec >= 0) {
            if (exc_trace) exc_note(0x100 + (uint32_t)vec, 0);     // 1xx: an IRQ injected
            if (g_gwatch && (uint32_t)vm_read(VMCS_GUEST_RIP) == g_gwatch && g_gwatch_left) {
                // "gwatch <eip>": the code and stack where an interrupt arrives there
                g_gwatch_left--;
                uint32_t rip = (uint32_t)vm_read(VMCS_GUEST_RIP), rsp = (uint32_t)vm_read(VMCS_GUEST_RSP);
                printf("gwatch: irq %d at %08x esp %08x eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x\n",
                       vec, rip, rsp, (uint32_t)vm.regs.rax, (uint32_t)vm.regs.rbx, (uint32_t)vm.regs.rcx,
                       (uint32_t)vm.regs.rdx, (uint32_t)vm.regs.rsi, (uint32_t)vm.regs.rdi, (uint32_t)vm.regs.rbp);
                wyse_cpu_gmem(rip - 0x60, 128);
                wyse_cpu_gmem(rsp, 128);
            }
            if (mtf_at && !mtf_on && (uint32_t)vm_read(VMCS_GUEST_RIP) == mtf_at) {
                if (mtf_skip) {
                    mtf_skip--;
                } else {
                    mtf_on = true;
                    printf("mtf: on at irq %d, esp %08x\n", vec, (uint32_t)vm_read(VMCS_GUEST_RSP));
                    vm_write(VMCS_PROC_CTLS, vm_read(VMCS_PROC_CTLS) | PROC_MONITOR_TRAP);
                }
            }
            inject((uint32_t)vec, 0, false, 0);
            g_vmx_irqs++;
            halted = false;
        }
        if (proc & PROC_INTR_WINDOW_EXIT)
            vm_write(VMCS_PROC_CTLS, vm_read(VMCS_PROC_CTLS) & ~PROC_INTR_WINDOW_EXIT);
    } else if (!(proc & PROC_INTR_WINDOW_EXIT)) {
        vm_write(VMCS_PROC_CTLS, proc | PROC_INTR_WINDOW_EXIT);
    }
}

//--------------------------------------------------------------------+
// CPU speed, counted in the guest's instructions. The Atom's fixed counter 0
// counts what retires; held to a 386, 486 or Pentium the guest may retire per
// millisecond what that part would, earned by the wall clock and spent in VM
// entries. Exits retire nothing in the guest, so a game polling a status port
// (each read an exit) is charged for its few instructions, not for VT-x: a
// time-based share charged those exits and Wolfenstein's start-up, millions
// of reads of 3DAh, took most of a minute at Pentium 133.
//
// The rate comes from Turbo Pascal's Delay calibration loop, the one that
// stops with "Runtime error 200" on a machine that is too fast: its five
// instructions take 22 clocks on a 386, 8 on a 486, 4 on a Pentium, and the
// Atom's own rate for it bounds how long an entry may run.
//--------------------------------------------------------------------+

static uint64_t host_ipms;                  // instructions per ms the Atom runs that loop at
static uint64_t target_ipms;                // the chosen part's; 0 = no limit
static int64_t credit;                      // guest instructions earned
static uint64_t credit_at;
static uint64_t run_overhead = UINT64_MAX;  // instructions an entry and exit count with no guest work

static inline void wrmsr_(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline uint64_t instr_retired(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(1u << 30));    // fixed counter 0
    return (uint64_t)hi << 32 | lo;
}

static void calibrate_speed(void) {
    // fixed counter 0 counting in every ring, globally on, frozen in SMM
    wrmsr_(0x38d, (rdmsr_(0x38d) & ~0xfull) | 3);
    wrmsr_(0x38f, rdmsr_(0x38f) | (1ull << 32));
    wrmsr_(0x1d9, rdmsr_(0x1d9) | (1ull << 14));
    static volatile uint8_t mem = 0x5a;
    const uint32_t n = 20000000;
    uint64_t t0 = rdtsc_(), i0 = instr_retired();
    __asm__ volatile(
        "mov %[n], %%eax\n\t"
        "xor %%edx, %%edx\n\t"
        "mov $0x5a, %%bl\n"
        "1:\n\t"
        "sub $1, %%eax\n\t"
        "sbb $0, %%edx\n\t"
        "jc 2f\n\t"
        "cmp (%[m]), %%bl\n\t"
        "je 1b\n"
        "2:\n"
        :: [n] "r"(n), [m] "r"(&mem) : "rax", "rdx", "rbx", "cc", "memory");
    uint64_t ticks = rdtsc_() - t0, instr = instr_retired() - i0;
    host_ipms = (uint64_t)n * 5 * wyse_tsc_khz() / (ticks ? ticks : 1);
    printf("vmx: speed calibration %llu instructions per ms natively (counter saw %llu of %llu)\n",
           (unsigned long long)host_ipms, (unsigned long long)instr, (unsigned long long)n * 5);
}

void wyse_cpu_set_speed(int mhz, int gen) {
    static const int loop_clocks[] = {22, 22, 22, 22, 8, 4};   // by generation (3 386, 4 486, 5 Pentium)
    if (!host_ipms) calibrate_speed();
    target_ipms = 0;
    if (mhz > 0) {
        target_ipms = (uint64_t)mhz * 1000 * 5 / loop_clocks[gen < 3 ? 3 : gen > 5 ? 5 : gen];
        if (target_ipms >= host_ipms) target_ipms = 0;
    }
    credit = 0;
    credit_at = rdtsc_();
    printf("vmx: speed %d MHz (%s), %llu instructions per ms of the Atom's %llu\n", mhz,
           gen == 3 ? "386" : gen == 4 ? "486" : "Pentium", (unsigned long long)target_ipms,
           (unsigned long long)host_ipms);
}

static bool dead;   // VM entry failed: the guest stays stopped until reset

void wyse_cpu_step(PC *pc) {
    (void)pc;
    if (dead) return;
    if (halted && !irq_pending) return;     // HLT: wait for the devices to raise something

    // A slice ends at least twice per timer period: the 8254 model raises one
    // edge per poll, and the polls come between slices - at 200 us a game
    // running the timer at 8 kHz (E-Body's sampled sound) got 4.9 kHz.
    uint64_t slice = slice_tsc;
    {
        int cnt = pit_get_initial_count(pc->pit, 0);
        uint64_t period = (uint64_t)(cnt ? cnt : 65536) * wyse_tsc_khz() / 1193;     // TSC ticks
        if (period / 2 < slice) slice = period / 2;
        if (slice < slice_tsc / 20) slice = slice_tsc / 20;
    }
    uint64_t start = rdtsc_(), deadline = start + slice;
    for (;;) {
        deliver_irq();
        uint64_t now = rdtsc_();
        uint64_t left = deadline > now ? deadline - now : 0;
        bool limited = target_ipms != 0;
        if (limited) {
            uint64_t khz = wyse_tsc_khz();
            credit += (int64_t)((now - credit_at) * target_ipms / khz);
            credit_at = now;
            // runs of at least 50 us of the part's work and at most 1 ms: in
            // smaller ones the entries and exits cost more than the running
            // (4 us pieces made 250,000 preemption exits a second)
            if (credit > (int64_t)target_ipms) credit = (int64_t)target_ipms;
            if (credit < (int64_t)(target_ipms / 20)) break;     // its share of this moment is spent
            // no longer than the earned instructions take at the Atom's pace
            uint64_t t = (uint64_t)credit * khz / host_ipms + 1;
            if (left > t) left = t;
        }
        vm_write(VMCS_PREEMPTION_TIMER, (left >> 6) + 1);
        g_where = 3;
        uint64_t i_in = instr_retired();
        bool entered = vm_run(&vm);
        if (limited) {
            // what the guest retired, less what getting in and out counts (the
            // fewest any entry has counted); SMM is not counted at all
            uint64_t used = instr_retired() - i_in;
            if (used < run_overhead) run_overhead = used;
            credit -= (int64_t)(used - run_overhead);
        }
        g_where = 1;
        if (!entered) {
            dump_guest("VM entry failed");
            printf("vmx: instruction error %u\n", vm.exit_reason & 0xffff);
            dead = true;
            return;
        }
        uint32_t reason = vm.exit_reason & 0xffff;
        if (reason < 64) g_vmx_exits[reason]++;
        {
            // An exit in the middle of delivering an event (an interrupt from
            // the 8259, an exception) leaves it undelivered: it has to be
            // offered again on the next entry. Dropped, the 8259 still holds
            // that IRQ in service - acknowledged, never ended - so it and every
            // lower one (the keyboard, the PS/2 mouse behind IRQ 2) were dead
            // for good while the timer went on: the keyboard that stopped.
            uint32_t vect = (uint32_t)vm_read(VMCS_IDT_VECTORING_INFO);
            if ((vect & (1u << 31)) && g_redeliver) {
                g_vmx_redelivered++;
                vm_write(VMCS_ENTRY_INTR_INFO, vect & 0x80000fffu);
                if (vect & (1u << 11)) vm_write(VMCS_ENTRY_EXC_ERRCODE, vm_read(VMCS_IDT_VECTORING_ERRCODE));
                uint32_t type = (vect >> 8) & 7;
                if (type == 4 || type == 5 || type == 6) vm_write(VMCS_ENTRY_INSTR_LEN, vm.exit_instr_len);
            }
        }
        if (vm.exit_reason & 0x80000000u) {
            dump_guest("VM entry failed (invalid guest state)");
            dead = true;
            return;
        }
        switch (reason) {
        case EXIT_EXTERNAL_INTR:
        case EXIT_INTR_WINDOW:
        case EXIT_PREEMPTION_TIMER:
            break;
        case EXIT_HLT:
            vm_skip_instr(&vm);
            if (!irq_pending) {
                halted = true;
                g_vmx_guest_tsc += rdtsc_() - start;
                return;
            }
            break;
        case EXIT_IO: {
            uint64_t ti = rdtsc_();
            do_io();
            g_vmx_io_tsc += rdtsc_() - ti;
            uint32_t port = (uint32_t)(vm.exit_qual >> 16) & 0xffff;
            if ((port >= 0x3c4 && port <= 0x3cf) || port == 0x1ce || port == 0x1cf || (vm.exit_qual & (1 << 4)))
                plane_update();
            break;
        }
        case EXIT_EPT_VIOLATION:
            g_last_ept_gpa = vm.exit_gpa;
            if (!wyse_rom_write_allowed((uint32_t)vm.exit_gpa)) {
                rom_write_faults++;
                if (rom_write_faults<=8)
                    printf("rom: blocked native write at %llx from %04x:%08x\n",
                        (unsigned long long)vm.exit_gpa,(unsigned)vm_read(VMCS_GUEST_CS_SEL),
                        (unsigned)vm_read(VMCS_GUEST_RIP));
            }
            if (plane_fault(vm.exit_gpa)) break;    // the page now goes to one plane natively
            {
                // which VGA settings still go through the interpreter, for the log
                VGAState *v = g_pc->vga;
                uint32_t key = (uint32_t)v->sr[2] << 24 | (uint32_t)v->gr[5] << 16 | (uint32_t)v->gr[8] << 8 |
                               (uint32_t)(v->gr[3] & 0x1f) << 3 | (v->gr[4] & 3) << 1 | !!(v->sr[4] & 8);
                int slot = -1;
                for (int i = 0; i < 8 && slot < 0; i++)
                    if (g_vga_keys[i] == key || !g_vga_key_n[i]) slot = i;
                if (slot >= 0) { g_vga_keys[slot] = key; g_vga_key_n[slot]++; }
            }
            // A burst pays off for code drawing into video memory. A probe of
            // the APIC's page (DOS/4GW Professional reads FEE00030 while it
            // switches modes) is one instruction, not a run into that switch.
            if (vm.exit_gpa >= 0xfec00000u && vm.exit_gpa < 0xff000000u) emulate(1, 1);
            else emulate(4096, 32);
            plane_update();                         // the burst may have set the VGA up
            break;
        case EXIT_CPUID:
            do_cpuid();
            vm_skip_instr(&vm);
            break;
        case EXIT_RDMSR:
            if ((uint32_t)vm.regs.rcx == 0x10) {
                uint64_t t = rdtsc_();
                vm.regs.rax = (uint32_t)t;
                vm.regs.rdx = t >> 32;
            } else {
                vm.regs.rax = vm.regs.rdx = 0;
            }
            vm_skip_instr(&vm);
            break;
        case EXIT_WRMSR:
        case EXIT_INVD:
        case EXIT_WBINVD:
            vm_skip_instr(&vm);
            break;
        case EXIT_CR_ACCESS:
            do_cr_access();
            vm_skip_instr(&vm);
            break;
        case EXIT_TASK_SWITCH: {
            // A task switch always exits. From CALL, JMP or IRET the
            // interpreter runs that instruction. From an interrupt through a
            // task gate in the IDT there is no instruction to run: the
            // interpreter delivers the event. Running the instruction instead
            // lost the interrupt and left a task half switched - DOS/4GW
            // Professional takes the hardware interrupts that way under
            // EMM386, and Warcraft II died of a #TS and a triple fault.
            uint32_t vect = (uint32_t)vm_read(VMCS_IDT_VECTORING_INFO);
            // 2xx: a task switch (2 = CALL, 1 IRET, 2 JMP, 3 IDT in the tens), err the new TSS
            if (exc_trace) exc_note(0x200 + (uint32_t)((vm.exit_qual >> 30) & 3) * 0x10, (uint32_t)(vm.exit_qual & 0xffff));
            {
                static int logged;
                if (logged < 20) {
                    logged++;
                    static const char *src[4] = {"CALL", "IRET", "JMP", "IDT"};
                    printf("vmx: task switch by %s to tss %04x at %04x:%08x, event %08x, tr %04x\n",
                           src[(vm.exit_qual >> 30) & 3], (unsigned)(vm.exit_qual & 0xffff),
                           (unsigned)vm_read(VMCS_GUEST_CS_SEL), (unsigned)vm_read(VMCS_GUEST_RIP), vect,
                           (unsigned)vm_read(VMCS_GUEST_TR_SEL));
                }
            }
            if (((vm.exit_qual >> 30) & 3) == 3 && (vect & (1u << 31))) {
                vm_write(VMCS_ENTRY_INTR_INFO, 0);      // delivered here, not again
                uint32_t type = (vect >> 8) & 7;
                WyseCpuState s;
                state_from_vmcs(&s);
                if (type == 4 || type == 5 || type == 6) s.eip += vm.exit_instr_len;    // INT n, INT3, INTO
                cpui386_import_state(icpu, &s);
                bool ok = cpui386_deliver(icpu, (int)(vect & 0xff), (vect & (1u << 11)) != 0,
                                          (uint32_t)vm_read(VMCS_IDT_VECTORING_ERRCODE),
                                          type == 0 || type == 2 || type == 3);
                cpui386_export_state(icpu, &s);
                state_to_vmcs(&s);
                g_task_gate_events++;
                if (!ok) {
                    dump_guest("triple fault in a task gate's delivery, resetting");
                    pc->reset_request = 1;
                    g_vmx_guest_tsc += rdtsc_() - start;
                    return;
                }
            } else {
                emulate(1, 1);
            }
            break;
        }
        case EXIT_MONITOR_TRAP:
            if (mtf_on) mtf_step();
            break;
        case EXIT_TRIPLE_FAULT:
            exc_dump();
            dump_guest("triple fault, resetting");
            pc->reset_request = 1;
            g_vmx_guest_tsc += rdtsc_() - start;
            return;
        case EXIT_XSETBV:
        case EXIT_VMCALL:
            inject_ud();
            break;
        case EXIT_EXCEPTION_NMI: {
            // with "trapexc 1" (debugging): the guest's exceptions, logged and
            // delivered as they were; otherwise only host NMIs come here
            uint32_t info = vm.exit_intr_info;
            if ((info & 0x80000000u) && ((info >> 8) & 7) == 3) {
                uint32_t vec = info & 0xff, err = (uint32_t)vm_read(VMCS_EXIT_INTR_ERRCODE);
                exc_note(vec, (info & (1u << 11)) ? err : 0);
                if (vec == 14) {
                    uint64_t cr2 = vm.exit_qual;
                    __asm__ volatile("mov %0, %%cr2" :: "r"(cr2));
                }
                inject(vec, 3, (info & (1u << 11)) != 0, err);
            }
            break;
        }
        default:
            dump_guest("unhandled exit");
            inject_ud();
            break;
        }
        if (rdtsc_() >= deadline || pc->reset_request) break;
    }
    // what the guest wrote into a mapped plane, to the model for the display
    if (plane_mapped) {
        uint64_t t = rdtsc_();
        if (t - plane_flushed_at > wyse_tsc_khz() * 4) {
            plane_scatter();
            plane_flushed_at = t;
        }
    }
    // Native LFB stores bypass g_vram_writes. direct_vram makes the renderer
    // check its row shadow independently of this epilogue: HLT and other
    // early exits can occur after drawing a cursor without reaching here.
    g_vmx_guest_tsc += rdtsc_() - start;
}

void wyse_cpu_reset(PC *pc) {
    (void)pc;
    i440fx_pam_reset(pc->i440fx);
    wyse_cpu_pam_changed();
    plane_release();
    hw_update(true);
    vm_reset_real(&vm, 0xf000, 0xfff0);
    vm_write(VMCS_GUEST_CS_BASE, 0xf0000);
    vm_write(VMCS_ENTRY_INTR_INFO, 0);
    uint32_t proc = (uint32_t)vm_read(VMCS_PROC_CTLS);
    vm_write(VMCS_PROC_CTLS, proc & ~PROC_INTR_WINDOW_EXIT);
    halted = false;
    dead = false;
    irq_pending = false;
    a20_on = true;
    vm_map(&vm, 0x100000, vm.ram + 0x100000, 0x10000, true);
    printf("vmx: guest reset to F000:FFF0\n");
}

const char *wyse_cpu_init(PC *pc) {
    g_pc = pc;
    icpu = pc->cpu;
    const char *err = vmx_host_init();
    if (err) return err;
    err = vm_create(&vm, pc->phys_mem, pc->phys_mem_capacity);
    if (err) return err;
    wyse_cpu_pam_changed();

    // guest RAM above what the machine has is not there: unmap it so it reads as open bus
    if ((uint64_t)pc->phys_mem_size < vm.ram_size)
        vm_map(&vm, pc->phys_mem_size, NULL, vm.ram_size - pc->phys_mem_size, false);
    // the BIOS is also visible just below 4 GB, where a real reset vector points
    vm_map(&vm, 0xfffe0000, vm.ram + 0xe0000, 0x20000, false);

    // pin-based: add the preemption timer that ends each slice
    uint64_t pinmsr = rdmsr_(0x48D);
    uint32_t pin = (uint32_t)vm_read(VMCS_PIN_CTLS) | PIN_PREEMPTION_TIMER;
    pin &= (uint32_t)(pinmsr >> 32);
    vm_write(VMCS_PIN_CTLS, pin);
    uint64_t exitmsr = rdmsr_(0x48F);
    uint32_t exitc = (uint32_t)vm_read(VMCS_EXIT_CTLS) | EXIT_SAVE_PREEMPT;
    exitc &= (uint32_t)(exitmsr >> 32);
    vm_write(VMCS_EXIT_CTLS, exitc);

    slice_tsc = wyse_tsc_khz() * 200 / 1000;    // 200 us
    {
        void *pcmalloc(long size);      // page aligned (wyse.c)
        plane_buf = pcmalloc(0x10000);
    }

    orig_cb = icpu->cb;
    icpu->cb.iomem_read8 = cb_mr8;
    icpu->cb.iomem_write8 = cb_mw8;
    icpu->cb.iomem_read16 = cb_mr16;
    icpu->cb.iomem_write16 = cb_mw16;
    icpu->cb.iomem_read32 = cb_mr32;
    icpu->cb.iomem_write32 = cb_mw32;
    icpu->cb.iomem_write_string = cb_mws;
    return NULL;
}
