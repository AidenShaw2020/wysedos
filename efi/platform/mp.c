// The Atom's other three cores.
//
// Each runs a procedure that never returns, like X86Pi's helper cores on the
// Pi: core 1 sound, core 2 display, core 3 the General MIDI synthesiser. The
// firmware's MP Services only start them in blocking mode here (a non-blocking
// StartupThisAP never returns on this AMI firmware), so MP Services is used
// just to learn the APIC IDs, and the cores are started with our own
// INIT-SIPI-SIPI and a trampoline (ap_tramp.S) into long mode on the BSP's
// page tables. Code running there must not call UEFI.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../include/efi.h"

extern EFI_BOOT_SERVICES *gBS;
void wyse_udelay(uint32_t us);

#define EFI_MP_SERVICES_PROTOCOL_GUID \
    {0x3fdda605, 0xa76e, 0x4f46, {0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08}}

typedef void (*AP_PROCEDURE)(void *);

typedef struct EFI_MP_SERVICES_PROTOCOL {
    EFI_STATUS (*GetNumberOfProcessors)(struct EFI_MP_SERVICES_PROTOCOL *, uint64_t *Total, uint64_t *Enabled);
    void *GetProcessorInfo;
    void *StartupAllAPs;
    EFI_STATUS (*StartupThisAP)(struct EFI_MP_SERVICES_PROTOCOL *, AP_PROCEDURE, uint64_t ProcessorNumber,
                                EFI_EVENT WaitEvent, uint64_t TimeoutUs, void *Arg, bool *Finished);
    void *SwitchBSP;
    void *EnableDisableAP;
    EFI_STATUS (*WhoAmI)(struct EFI_MP_SERVICES_PROTOCOL *, uint64_t *ProcessorNumber);
} EFI_MP_SERVICES_PROTOCOL;

extern uint8_t ap_tramp_start[], ap_tramp_end[], ap_tramp_data[];
extern uint32_t ap_tramp_off_gdtr, ap_tramp_off_gdt, ap_tramp_off_far32, ap_tramp_off_pm32;
extern uint32_t ap_tramp_off_far64, ap_tramp_off_lm64;

typedef struct __attribute__((packed)) {
    uint64_t cr3, cr4, efer, cr0, stack, entry, arg;
    uint16_t gdtr_limit;
    uint64_t gdtr_base;
    uint8_t pad1[6];
    uint16_t idtr_limit;
    uint64_t idtr_base;
    uint8_t pad2[6];
    uint16_t cs, ds;
} tramp_data_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} dtr_t;

static EFI_MP_SERVICES_PROTOCOL *mp;
static uint8_t *tramp;              // page below 1 MB
static volatile uint32_t *lapic;
static uint64_t pat_value;
static uint32_t apic_of[4];
static volatile uint32_t core_apic[4] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
static int ncores;

typedef struct {
    void (*fn)(void);
    int core;
} ap_job_t;

static ap_job_t jobs[4];

static inline uint64_t rdmsr_(uint32_t m) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return ((uint64_t)hi << 32) | lo; }
static inline void wrmsr_(uint32_t m, uint64_t v) { __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }

static inline uint32_t apic_id(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return b >> 24;
}

// Which of our cores this is: 0 runs the guest, 1-3 are helpers.
int get_core_num(void) {
    uint32_t id = apic_id();
    for (int i = 1; i < 4; i++)
        if (core_apic[i] == id) return i;
    return 0;
}

static void ap_probe(void *arg) {
    *(volatile uint32_t *)arg = apic_id();
}

// First C code on a started AP (called from the trampoline).
static void ap_main(void *arg) {
    ap_job_t *job = arg;
    // the PAT is per CPU: the framebuffer's write-combining entry has to be set here too
    wrmsr_(0x277, pat_value);
    // and the MTRR that makes the real VGA's memory cacheable for the display (hwvga.c)
    void hwvga_mtrr_apply(void);
    hwvga_mtrr_apply();
    core_apic[job->core] = apic_id();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    job->fn();
    for (;;) __asm__ volatile("pause");
}

int mp_init(void) {
    static const EFI_GUID guid = EFI_MP_SERVICES_PROTOCOL_GUID;
    uint64_t total = 0, enabled = 0;
    if (EFI_ERROR(gBS->LocateProtocol(&guid, NULL, (void **)&mp))) {
        printf("mp: no MP services\n");
        return 1;
    }
    mp->GetNumberOfProcessors(mp, &total, &enabled);
    core_apic[0] = apic_id();
    ncores = (int)(enabled > 4 ? 4 : enabled);
    for (int i = 1; i < ncores; i++) {
        static volatile uint32_t probe;
        probe = 0xffffffff;
        if (EFI_ERROR(mp->StartupThisAP(mp, ap_probe, (uint64_t)i, NULL, 1000000, (void *)&probe, NULL))) {
            ncores = i;
            break;
        }
        apic_of[i] = probe;
    }

    // trampoline page below 1 MB
    EFI_PHYSICAL_ADDRESS page = 0x9ffff;
    if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &page))) {
        printf("mp: no low page for the trampoline\n");
        return 1;
    }
    tramp = (uint8_t *)page;
    uint64_t apic_base = rdmsr_(0x1b);
    if (apic_base & (1 << 10)) {
        printf("mp: x2APIC mode, not supported\n");
        return 1;
    }
    lapic = (volatile uint32_t *)(apic_base & ~0xfffULL);
    printf("mp: %llu processors, apic ids %u %u %u %u, trampoline %llx, lapic %p\n", (unsigned long long)total,
           core_apic[0], apic_of[1], apic_of[2], apic_of[3], (unsigned long long)page, (void *)lapic);
    return ncores;
}

static void lapic_icr(uint32_t dest, uint32_t low) {
    lapic[0x310 / 4] = dest << 24;
    lapic[0x300 / 4] = low;
    for (int i = 0; i < 100000 && (lapic[0x300 / 4] & (1 << 12)); i++) __asm__ volatile("pause");
}

// Run fn() forever on helper core `core` (1..3). Returns false if not possible.
bool mp_start(int core, void (*fn)(void)) {
    if (!tramp || core < 1 || core >= ncores) return false;
    size_t size = ap_tramp_end - ap_tramp_start;
    memcpy(tramp, ap_tramp_start, size);
    uint32_t base = (uint32_t)(uint64_t)tramp;
    *(uint32_t *)(tramp + ap_tramp_off_gdtr + 2) = base + ap_tramp_off_gdt;
    *(uint32_t *)(tramp + ap_tramp_off_far32) = base + ap_tramp_off_pm32;
    *(uint32_t *)(tramp + ap_tramp_off_far64) = base + ap_tramp_off_lm64;

    tramp_data_t *d = (tramp_data_t *)(tramp + (ap_tramp_data - ap_tramp_start));
    uint64_t cr0, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    d->cr0 = cr0;
    d->cr3 = cr3;
    d->cr4 = cr4 & ~(1ULL << 13);               // no VMXE on the helpers
    d->efer = rdmsr_(0xc0000080) & ~(1ULL << 10); // LMA is the CPU's to set
    EFI_PHYSICAL_ADDRESS stack;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 16, &stack))) return false;
    d->stack = stack + 16 * 4096 - 64;
    d->entry = (uint64_t)ap_main;
    jobs[core].fn = fn;
    jobs[core].core = core;
    d->arg = (uint64_t)&jobs[core];
    dtr_t gdtr, idtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));
    d->gdtr_limit = gdtr.limit;
    d->gdtr_base = gdtr.base;
    d->idtr_limit = idtr.limit;
    d->idtr_base = idtr.base;
    uint16_t cs, ds;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile("mov %%ds, %0" : "=r"(ds));
    d->cs = cs;
    d->ds = ds;
    pat_value = rdmsr_(0x277);
    __asm__ volatile("wbinvd");

    uint32_t dest = apic_of[core];
    lapic_icr(dest, 0x4500);                    // INIT
    wyse_udelay(10000);
    for (int i = 0; i < 2; i++) {
        lapic_icr(dest, 0x4600 | (base >> 12)); // STARTUP
        wyse_udelay(200);
    }
    for (int i = 0; i < 1000 && core_apic[core] == 0xffffffff; i++) wyse_udelay(100);
    printf("mp: core %d %s (apic %u)\n", core, core_apic[core] != 0xffffffff ? "running" : "did not start", dest);
    return core_apic[core] != 0xffffffff;
}

// Loop counts of the helper cores' loops, for the hang watch
volatile uint64_t g_core_heartbeat[4];

typedef struct {
    uint64_t last_check, last_hb, since;
    int reported;
} watch_t;

// When a core's loop count has not moved for 3 s, it gets an NMI and where it
// was goes to the log (exc.c records it). A hang with interrupts off is
// otherwise silent - the firmware timer that the hang detector in wyse.c runs
// on never fires, and a helper core has no detector at all.
static void watch(watch_t *w, uint64_t hb, int core, const char *name) {
    extern volatile uint64_t g_nmi_snap[24];
    extern volatile int g_nmi_count;
    extern void *gImageBase;
    uint64_t wyse_tsc_khz(void);
    uint64_t now, khz = wyse_tsc_khz();
    __asm__ volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(now) :: "rdx");
    if (!lapic || core >= ncores || core_apic[core] == 0xffffffff || now - w->last_check < khz * 100) return;
    w->last_check = now;
    if (hb != w->last_hb || !w->since) {    // moving, or the first look
        w->last_hb = hb;
        w->since = now;
        w->reported = 0;
        return;
    }
    if (w->reported >= 3 || now - w->since < khz * 3000 * (uint64_t)(w->reported + 1)) return;
    w->reported++;
    int before = g_nmi_count;
    lapic_icr(core_apic[core], 0x4400);         // NMI
    for (int i = 0; i < 500 && g_nmi_count == before; i++) wyse_udelay(100);
    if (g_nmi_count == before) {
        printf("HANG: %s stopped %d s, no answer to an NMI (SMM?)\n", name, 3 * w->reported);
        return;
    }
    volatile uint64_t *s = g_nmi_snap;
    printf("HANG: %s stopped %d s; NMI at rip %llx (image %p) rsp %llx rflags %llx cs %llx\n", name,
           3 * w->reported, (unsigned long long)s[0], gImageBase, (unsigned long long)s[1],
           (unsigned long long)s[2], (unsigned long long)s[3]);
    printf("HANG:  rax %llx rcx %llx rdx %llx rbx %llx\n", (unsigned long long)s[4], (unsigned long long)s[5],
           (unsigned long long)s[6], (unsigned long long)s[7]);
    for (int r = 0; r < 2; r++)
        printf("HANG:  stack %llx %llx %llx %llx %llx %llx %llx %llx\n", (unsigned long long)s[8 + 8 * r],
               (unsigned long long)s[9 + 8 * r], (unsigned long long)s[10 + 8 * r], (unsigned long long)s[11 + 8 * r],
               (unsigned long long)s[12 + 8 * r], (unsigned long long)s[13 + 8 * r], (unsigned long long)s[14 + 8 * r],
               (unsigned long long)s[15 + 8 * r]);
}

// From core 3's loop: the main loop and the sound core (1)
void mp_hang_watch(void) {
    extern volatile uint64_t g_main_heartbeat;
    static watch_t main_w, sound_w;
    watch(&main_w, g_main_heartbeat, 0, "main loop");
    watch(&sound_w, g_core_heartbeat[1], 1, "sound core");
}

// From core 1's loop: core 3 (headphones, General MIDI)
void mp_hang_watch_core3(void) {
    static watch_t w;
    watch(&w, g_core_heartbeat[3], 3, "core 3");
}
