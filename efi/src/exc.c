// Host exception reporting.
//
// A fault in the monitor would otherwise end in the firmware's handler, which
// prints to a screen that belongs to the guest and stops. Ours sends the
// registers and a stack excerpt to the UDP log and resets the machine, so the
// next netboot comes up by itself.

#include "lib.h"
#include "net.h"

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vector, error, rip, cs, rflags, rsp, ss;
} exc_frame_t;

#define STUBS(X) X(0) X(1) X(3) X(4) X(5) X(6) X(7) X(8) X(10) X(11) X(12) X(13) X(14) X(16) X(17) X(18) X(19)
#define DECL(n) extern void exc_stub_##n(void);
STUBS(DECL)

typedef struct __attribute__((packed)) {
    uint16_t off_lo, sel;
    uint8_t ist, type;
    uint16_t off_mid;
    uint32_t off_hi, zero;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} dtr_t;

static volatile int in_exception;

void host_exception(exc_frame_t *f) {
    if (in_exception++) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    printf("HOST EXCEPTION %llu err %llx at rip %llx (image %p) cr2 %llx rsp %llx rflags %llx\n",
           (unsigned long long)f->vector, (unsigned long long)f->error, (unsigned long long)f->rip, gImageBase,
           (unsigned long long)cr2, (unsigned long long)f->rsp, (unsigned long long)f->rflags);
    printf("  rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx\n", (unsigned long long)f->rax,
           (unsigned long long)f->rbx, (unsigned long long)f->rcx, (unsigned long long)f->rdx,
           (unsigned long long)f->rsi, (unsigned long long)f->rdi, (unsigned long long)f->rbp);
    printf("  r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx\n", (unsigned long long)f->r8,
           (unsigned long long)f->r9, (unsigned long long)f->r10, (unsigned long long)f->r11,
           (unsigned long long)f->r12, (unsigned long long)f->r13, (unsigned long long)f->r14,
           (unsigned long long)f->r15);
    uint64_t *sp = (uint64_t *)f->rsp;
    printf("  stack %llx %llx %llx %llx %llx %llx %llx %llx\n", (unsigned long long)sp[0], (unsigned long long)sp[1],
           (unsigned long long)sp[2], (unsigned long long)sp[3], (unsigned long long)sp[4], (unsigned long long)sp[5],
           (unsigned long long)sp[6], (unsigned long long)sp[7]);
    // give the log a moment, then start over (netboot picks up the newest build)
    uint64_t t = rdtsc();
    while (rdtsc() - t < 1440ULL * 1000 * 500) __asm__ volatile("pause");
    gST->RuntimeServices->ResetSystem(EfiResetCold, 0, 0, NULL);
}

// Where the main core was when the hang watch's NMI arrived (exc.h)
volatile uint64_t g_nmi_snap[24];
volatile int g_nmi_count;

void host_nmi(exc_frame_t *f) {
    g_nmi_snap[0] = f->rip;
    g_nmi_snap[1] = f->rsp;
    g_nmi_snap[2] = f->rflags;
    g_nmi_snap[3] = f->cs;
    g_nmi_snap[4] = f->rax;
    g_nmi_snap[5] = f->rcx;
    g_nmi_snap[6] = f->rdx;
    g_nmi_snap[7] = f->rbx;
    uint64_t *sp = (uint64_t *)f->rsp;
    for (int i = 0; i < 16; i++) g_nmi_snap[8 + i] = sp[i];
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_nmi_count++;
}

void exc_install(void) {
    extern void nmi_stub(void);
    dtr_t idtr;
    __asm__ volatile("sidt %0" : "=m"(idtr));
    idt_entry_t *idt = (idt_entry_t *)idtr.base;
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
#define SET(n) { uint64_t a = (uint64_t)exc_stub_##n; idt_entry_t *e = &idt[n]; \
        e->off_lo = (uint16_t)a; e->sel = cs; e->ist = 0; e->type = 0x8e; \
        e->off_mid = (uint16_t)(a >> 16); e->off_hi = (uint32_t)(a >> 32); e->zero = 0; }
    STUBS(SET)
    {
        uint64_t a = (uint64_t)nmi_stub;
        idt_entry_t *e = &idt[2];
        e->off_lo = (uint16_t)a; e->sel = cs; e->ist = 0; e->type = 0x8e;
        e->off_mid = (uint16_t)(a >> 16); e->off_hi = (uint32_t)(a >> 32); e->zero = 0;
    }
}
