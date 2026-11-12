// VT-x setup for a real-mode-capable guest (EPT + unrestricted guest).

#include "lib.h"
#include "vm.h"

#define MSR_FEATURE_CONTROL     0x3A
#define MSR_VMX_BASIC           0x480
#define MSR_VMX_CR0_FIXED0      0x486
#define MSR_VMX_CR0_FIXED1      0x487
#define MSR_VMX_CR4_FIXED0      0x488
#define MSR_VMX_CR4_FIXED1      0x489
#define MSR_VMX_PROCBASED2      0x48B
#define MSR_VMX_TRUE_PINBASED   0x48D
#define MSR_VMX_TRUE_PROCBASED  0x48E
#define MSR_VMX_TRUE_EXIT       0x48F
#define MSR_VMX_TRUE_ENTRY      0x490
#define MSR_PAT                 0x277
#define MSR_EFER                0xC0000080
#define MSR_FS_BASE             0xC0000100
#define MSR_GS_BASE             0xC0000101

static uint64_t vmxon_phys, vmcs_phys;
static uint32_t vmcs_revision;

static void *alloc_pages(uint64_t pages, uint64_t align) {
    EFI_PHYSICAL_ADDRESS a;
    uint64_t extra = align > 4096 ? align / 4096 : 0;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages + extra, &a))) return NULL;
    if (align > 4096) a = (a + align - 1) & ~(align - 1);
    memset((void *)a, 0, pages * 4096);
    return (void *)a;
}

static inline __attribute__((unused)) void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline void write_cr0(uint64_t v) { __asm__ volatile("mov %0, %%cr0" :: "r"(v)); }
static inline void write_cr4(uint64_t v) { __asm__ volatile("mov %0, %%cr4" :: "r"(v)); }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }

uint64_t vm_read(uint64_t field) { return vmx_vmread(field); }

static int vmwrite_errors;
void vm_write(uint64_t field, uint64_t value) {
    if (vmx_vmwrite(field, value)) {
        if (vmwrite_errors++ < 8) printf("vmwrite %llx failed\n", field);
    }
}

static uint32_t adjust_ctls(uint32_t want, uint32_t msr) {
    uint64_t v = rdmsr(msr);
    want |= (uint32_t)v;            // allowed-0: bits that must be 1
    want &= (uint32_t)(v >> 32);    // allowed-1: bits that may be 1
    return want;
}

//--------------------------------------------------------------------+
// Host GDT with a TSS: VM exits need a non-zero host TR, UEFI has none
//--------------------------------------------------------------------+

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} dtr_t;

static uint64_t host_gdt[64] __attribute__((aligned(16)));
static uint8_t host_tss[104] __attribute__((aligned(16)));
static uint16_t host_tr_sel;

static void host_gdt_init(void) {
    dtr_t gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    uint32_t n = (gdtr.limit + 1) / 8;
    if (n > 60) n = 60;
    memcpy(host_gdt, (void *)gdtr.base, n * 8);
    // 16-byte TSS descriptor at index n (aligned to 8, fine)
    uint64_t base = (uint64_t)host_tss;
    uint64_t limit = sizeof(host_tss) - 1;
    host_gdt[n] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ULL << 40) |
                  (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
    host_gdt[n + 1] = base >> 32;
    host_tr_sel = (uint16_t)(n * 8);
    dtr_t ng = {(uint16_t)((n + 2) * 8 - 1), (uint64_t)host_gdt};
    __asm__ volatile("lgdt %0" :: "m"(ng));
    __asm__ volatile("ltr %0" :: "r"(host_tr_sel));
}

const char *vmx_host_init(void) {
    uint32_t r[4];
    cpuid(1, 0, r);
    if (!(r[2] & (1 << 5))) return "no VMX";
    uint64_t fc = rdmsr(MSR_FEATURE_CONTROL);
    if (!(fc & 1) || !(fc & 4)) return "VMX disabled in IA32_FEATURE_CONTROL";

    uint64_t basic = rdmsr(MSR_VMX_BASIC);
    vmcs_revision = (uint32_t)basic & 0x7fffffff;

    uint32_t *vmxon = alloc_pages(1, 4096), *vmcs = alloc_pages(1, 4096);
    if (!vmxon || !vmcs) return "out of memory";
    vmxon[0] = vmcs_revision;
    vmcs[0] = vmcs_revision;
    vmxon_phys = (uint64_t)vmxon;
    vmcs_phys = (uint64_t)vmcs;

    host_gdt_init();

    uint64_t cr0 = read_cr0(), cr4 = read_cr4();
    cr0 |= rdmsr(MSR_VMX_CR0_FIXED0);
    cr0 &= rdmsr(MSR_VMX_CR0_FIXED1);
    cr4 |= rdmsr(MSR_VMX_CR4_FIXED0);
    cr4 &= rdmsr(MSR_VMX_CR4_FIXED1);
    write_cr0(cr0);
    write_cr4(cr4);

    if (vmx_vmxon(&vmxon_phys)) return "VMXON failed";
    if (vmx_vmclear(&vmcs_phys)) return "VMCLEAR failed";
    if (vmx_vmptrld(&vmcs_phys)) return "VMPTRLD failed";
    return NULL;
}

//--------------------------------------------------------------------+
// EPT: guest RAM identity-mapped onto the host buffer, 4 KB pages below 2 MB
// (so the VGA window can be carved out), 2 MB pages above.
//--------------------------------------------------------------------+

#define EPT_R       1ULL
#define EPT_W       2ULL
#define EPT_X       4ULL
#define EPT_RWX     (EPT_R | EPT_W | EPT_X)
#define EPT_WB      (6ULL << 3)
#define EPT_LARGE   (1ULL << 7)
#define EPT_UC_IPAT (1ULL << 6)     // memory type 0 (uncached), the guest's PAT ignored

static uint64_t *ept_pml4, *ept_pdpt, *ept_pd[4], *ept_pt_low;
static uint64_t eptp;

static void ept_build(vm_t *vm) {
    ept_pml4 = alloc_pages(1, 4096);
    ept_pdpt = alloc_pages(1, 4096);
    ept_pml4[0] = (uint64_t)ept_pdpt | EPT_RWX;
    for (int i = 0; i < 4; i++) {
        ept_pd[i] = alloc_pages(1, 4096);
        ept_pdpt[i] = (uint64_t)ept_pd[i] | EPT_RWX;
    }
    ept_pt_low = alloc_pages(1, 4096);
    ept_pd[0][0] = (uint64_t)ept_pt_low | EPT_RWX;
    for (uint64_t p = 0; p < 512; p++) {
        uint64_t gpa = p * 4096;
        if (gpa >= 0xA0000 && gpa < 0xC0000) continue;  // VGA window: trapped
        ept_pt_low[p] = ((uint64_t)vm->ram + gpa) | EPT_RWX | EPT_WB;
    }
    for (uint64_t gpa = 0x200000; gpa < vm->ram_size; gpa += 0x200000) {
        uint64_t pdi = gpa >> 21;
        ept_pd[pdi / 512][pdi % 512] = ((uint64_t)vm->ram + gpa) | EPT_RWX | EPT_WB | EPT_LARGE;
    }
    eptp = (uint64_t)ept_pml4 | (3 << 3) | 6;   // 4-level walk, WB paging structures
}

static void ept_flush(void) {
    uint64_t desc[2] = {eptp, 0};
    vmx_invept(1, desc);    // single-context
}

// 4 KB mapping helper: splits a 2 MB page into a page table when needed
static uint64_t *ept_pte(uint64_t gpa) {
    uint64_t pdpti = gpa >> 30, pdi = (gpa >> 21) & 511;
    if (pdpti >= 4) return NULL;
    uint64_t *pde = &ept_pd[pdpti][pdi];
    if (!(*pde & EPT_RWX) || (*pde & EPT_LARGE)) {
        uint64_t old = *pde;
        uint64_t *pt = alloc_pages(1, 4096);
        if (!pt) return NULL;
        if (old & EPT_LARGE) {
            uint64_t base = old & ~0x1fffffULL & 0xfffffffffULL;
            for (int i = 0; i < 512; i++) pt[i] = (base + i * 4096) | (old & (EPT_RWX | EPT_WB));
        }
        *pde = (uint64_t)pt | EPT_RWX;
    }
    uint64_t *pt = (uint64_t *)(*pde & 0xffffffffff000ULL);
    return &pt[(gpa >> 12) & 511];
}

void vm_map(vm_t *vm, uint64_t gpa, void *host, uint64_t size, bool writable) {
    for (uint64_t off = 0; off < size; off += 4096) {
        uint64_t *pte = ept_pte(gpa + off);
        if (!pte) continue;
        *pte = host ? (((uint64_t)host + off) | EPT_R | EPT_X | (writable ? EPT_W : 0) | EPT_WB) : 0;
    }
    ept_flush();
}

static uint8_t *io_bitmap_a, *io_bitmap_b, *msr_bitmap;

void vm_flush_tlb(void) { ept_flush(); }

void vm_io_trap(uint16_t port, bool trap) {
    uint8_t *bm = port < 0x8000 ? io_bitmap_a : io_bitmap_b;
    uint16_t p = port & 0x7fff;
    if (trap) bm[p >> 3] |= (uint8_t)(1 << (p & 7));
    else bm[p >> 3] &= (uint8_t)~(1 << (p & 7));
}

void vm_map_mmio(vm_t *vm, uint64_t gpa, uint64_t hpa, uint64_t size) {
    (void)vm;
    for (uint64_t off = 0; off < size; off += 4096) {
        uint64_t *pte = ept_pte(gpa + off);
        if (pte) *pte = (hpa + off) | EPT_RWX | EPT_UC_IPAT;
    }
    ept_flush();
}

void vm_map_wc(vm_t *vm, uint64_t gpa, void *host, uint64_t size) {
    (void)vm;
    for (uint64_t off = 0; off < size; off += 4096) {
        uint64_t *pte = ept_pte(gpa + off);
        if (pte) *pte = ((uint64_t)host + off) | EPT_RWX | (1ULL << 3) | EPT_UC_IPAT;   // type 1 (WC), PAT ignored
    }
    ept_flush();
}

//--------------------------------------------------------------------+
// VMCS
//--------------------------------------------------------------------+

static WyseFxState guest_fx, host_fx;

static void vmcs_host_state(void) {
    uint16_t cs, ss, ds, es;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile("mov %%ss, %0" : "=r"(ss));
    __asm__ volatile("mov %%ds, %0" : "=r"(ds));
    __asm__ volatile("mov %%es, %0" : "=r"(es));
    vm_write(VMCS_HOST_CS_SEL, cs & 0xf8);
    vm_write(VMCS_HOST_SS_SEL, ss & 0xf8);
    vm_write(VMCS_HOST_DS_SEL, ds & 0xf8);
    vm_write(VMCS_HOST_ES_SEL, es & 0xf8);
    vm_write(VMCS_HOST_FS_SEL, 0);
    vm_write(VMCS_HOST_GS_SEL, 0);
    vm_write(VMCS_HOST_TR_SEL, host_tr_sel);
    vm_write(VMCS_HOST_CR0, read_cr0());
    vm_write(VMCS_HOST_CR3, read_cr3());
    vm_write(VMCS_HOST_CR4, read_cr4());
    vm_write(VMCS_HOST_FS_BASE, rdmsr(MSR_FS_BASE));
    vm_write(VMCS_HOST_GS_BASE, rdmsr(MSR_GS_BASE));
    vm_write(VMCS_HOST_TR_BASE, (uint64_t)host_tss);
    dtr_t gdtr, idtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));
    vm_write(VMCS_HOST_GDTR_BASE, gdtr.base);
    vm_write(VMCS_HOST_IDTR_BASE, idtr.base);
    vm_write(VMCS_HOST_SYSENTER_CS, 0);
    vm_write(VMCS_HOST_SYSENTER_ESP, 0);
    vm_write(VMCS_HOST_SYSENTER_EIP, 0);
    vm_write(VMCS_HOST_PAT, rdmsr(MSR_PAT));
    vm_write(VMCS_HOST_EFER, rdmsr(MSR_EFER));
}

static void vmcs_controls(void) {
    uint32_t pin = adjust_ctls(PIN_EXT_INTR_EXIT | PIN_NMI_EXIT, MSR_VMX_TRUE_PINBASED);
    uint32_t proc = adjust_ctls(PROC_HLT_EXIT | PROC_USE_IO_BITMAPS | PROC_USE_MSR_BITMAPS | PROC_SECONDARY,
                                MSR_VMX_TRUE_PROCBASED);
    uint32_t proc2 = adjust_ctls(PROC2_EPT | PROC2_UNRESTRICTED_GUEST | PROC2_VPID | PROC2_RDTSCP, MSR_VMX_PROCBASED2);
    uint32_t exitc = adjust_ctls(EXIT_HOST_64BIT | EXIT_SAVE_PAT | EXIT_LOAD_PAT | EXIT_SAVE_EFER | EXIT_LOAD_EFER,
                                 MSR_VMX_TRUE_EXIT);
    uint32_t entry = adjust_ctls(ENTRY_LOAD_PAT | ENTRY_LOAD_EFER, MSR_VMX_TRUE_ENTRY);
    vm_write(VMCS_PIN_CTLS, pin);
    vm_write(VMCS_PROC_CTLS, proc);
    vm_write(VMCS_PROC_CTLS2, proc2);
    vm_write(VMCS_EXIT_CTLS, exitc);
    vm_write(VMCS_ENTRY_CTLS, entry);
    vm_write(VMCS_EXCEPTION_BITMAP, 0);
    vm_write(VMCS_PF_ERR_MASK, 0);
    vm_write(VMCS_PF_ERR_MATCH, 0);
    vm_write(VMCS_CR3_TARGET_COUNT, 0);
    vm_write(VMCS_EXIT_MSR_STORE_COUNT, 0);
    vm_write(VMCS_EXIT_MSR_LOAD_COUNT, 0);
    vm_write(VMCS_ENTRY_MSR_LOAD_COUNT, 0);
    vm_write(VMCS_ENTRY_INTR_INFO, 0);
    vm_write(VMCS_IO_BITMAP_A, (uint64_t)io_bitmap_a);
    vm_write(VMCS_IO_BITMAP_B, (uint64_t)io_bitmap_b);
    vm_write(VMCS_MSR_BITMAP, (uint64_t)msr_bitmap);
    vm_write(VMCS_EPT_POINTER, eptp);
    vm_write(VMCS_VPID, 1);
    vm_write(VMCS_LINK_POINTER, ~0ULL);

    // The guest owns CR0/CR4 except for the bits VMX pins: NE (CR0) and VMXE (CR4)
    // are forced on and hidden behind the read shadows; CD/NW are kept off so guest
    // memory stays cached whatever the guest writes there.
    vm_write(VMCS_CR0_MASK, (1 << 5) | (1u << 29) | (1u << 30));
    vm_write(VMCS_CR4_MASK, 1 << 13);
}

void vm_reset_real(vm_t *vm, uint16_t cs, uint16_t ip) {
    memset(&vm->regs, 0, sizeof(vm->regs));
    vm_write(VMCS_GUEST_CR0, 0x00000030);      // ET | NE, real mode, no paging
    vm_write(VMCS_CR0_SHADOW, 0x60000010);     // what a reset CPU shows
    vm_write(VMCS_GUEST_CR3, 0);
    vm_write(VMCS_GUEST_CR4, 1 << 13);
    vm_write(VMCS_CR4_SHADOW, 0);
    vm_write(VMCS_GUEST_DR7, 0x400);
    vm_write(VMCS_GUEST_RSP, 0);
    vm_write(VMCS_GUEST_RIP, ip);
    vm_write(VMCS_GUEST_RFLAGS, 0x2);

    static const uint32_t sel[] = {VMCS_GUEST_ES_SEL, VMCS_GUEST_CS_SEL, VMCS_GUEST_SS_SEL,
                                   VMCS_GUEST_DS_SEL, VMCS_GUEST_FS_SEL, VMCS_GUEST_GS_SEL};
    for (int i = 0; i < 6; i++) {
        uint16_t s = (i == 1) ? cs : 0;
        vm_write(sel[i], s);
        vm_write(VMCS_GUEST_ES_BASE + i * 2, (uint64_t)s << 4);
        vm_write(VMCS_GUEST_ES_LIMIT + i * 2, 0xffff);
        vm_write(VMCS_GUEST_ES_AR + i * 2, i == 1 ? 0x9b : 0x93);
    }
    if (cs == 0xf000) vm_write(VMCS_GUEST_CS_BASE, 0xffff0000);
    vm_write(VMCS_GUEST_LDTR_SEL, 0);
    vm_write(VMCS_GUEST_LDTR_BASE, 0);
    vm_write(VMCS_GUEST_LDTR_LIMIT, 0xffff);
    vm_write(VMCS_GUEST_LDTR_AR, 0x82);
    vm_write(VMCS_GUEST_TR_SEL, 0);
    vm_write(VMCS_GUEST_TR_BASE, 0);
    vm_write(VMCS_GUEST_TR_LIMIT, 0xffff);
    vm_write(VMCS_GUEST_TR_AR, 0x8b);
    vm_write(VMCS_GUEST_GDTR_BASE, 0);
    vm_write(VMCS_GUEST_GDTR_LIMIT, 0xffff);
    vm_write(VMCS_GUEST_IDTR_BASE, 0);
    vm_write(VMCS_GUEST_IDTR_LIMIT, 0xffff);
    vm_write(VMCS_GUEST_INTERRUPTIBILITY, 0);
    vm_write(VMCS_GUEST_ACTIVITY, 0);
    vm_write(VMCS_GUEST_PENDING_DBG, 0);
    vm_write(VMCS_GUEST_DEBUGCTL, 0);
    vm_write(VMCS_GUEST_PAT, 0x0007040600070406ULL);
    vm_write(VMCS_GUEST_EFER, 0);
    vm_write(VMCS_GUEST_SYSENTER_CS, 0);
    vm_write(VMCS_GUEST_SYSENTER_ESP, 0);
    vm_write(VMCS_GUEST_SYSENTER_EIP, 0);
    vm->regs.rdx = 0x0600;   // reset signature, like a 386
}

const char *vm_create(vm_t *vm, void *ram, uint64_t ram_size) {
    memset(vm, 0, sizeof(*vm));
    vm->ram_size = (ram_size + 0x1fffff) & ~0x1fffffULL;
    vm->ram = ram ? ram : alloc_pages(vm->ram_size / 4096, 0x200000);
    if (!vm->ram) return "cannot allocate guest RAM";
    if ((uint64_t)vm->ram & 0x1fffff) return "guest RAM not 2 MB aligned";
    __asm__ volatile("fninit");
    __asm__ volatile("fxsave64 %0" : "=m"(guest_fx));

    io_bitmap_a = alloc_pages(1, 4096);
    io_bitmap_b = alloc_pages(1, 4096);
    msr_bitmap = alloc_pages(1, 4096);
    if (!io_bitmap_a || !io_bitmap_b || !msr_bitmap) return "out of memory";
    memset(io_bitmap_a, 0xff, 4096);    // every port traps
    memset(io_bitmap_b, 0xff, 4096);
    memset(msr_bitmap, 0xff, 4096);     // every MSR traps

    ept_build(vm);
    vmcs_host_state();
    vmcs_controls();
    vm_reset_real(vm, 0xf000, 0xfff0);
    return NULL;
}

bool vm_x87_store_prepare(uint64_t *bits, bool wide, bool pop, WyseFxState *next) {
    if (!wyse_x87_store(&guest_fx, next, bits, wide, pop)) return false;
    next->ip = vm_read(VMCS_GUEST_CS_BASE) + vm_read(VMCS_GUEST_RIP);
    next->dp = vm_read(VMCS_GUEST_LINEAR_ADDR);
    return true;
}

void vm_x87_store_commit(const WyseFxState *next) {
    guest_fx = *next;
}

bool vm_run(vm_t *vm) {
    // the guest's x87/SSE state lives in the hardware registers while it runs
    __asm__ volatile("cli");
    __asm__ volatile("fxsave64 %0" : "=m"(host_fx));
    __asm__ volatile("fxrstor64 %0" :: "m"(guest_fx));
    int failed = vmx_run(&vm->regs, vm->launched);
    __asm__ volatile("fxsave64 %0" : "=m"(guest_fx));
    __asm__ volatile("fxrstor64 %0" :: "m"(host_fx));
    __asm__ volatile("sti");
    if (failed) {
        vm->exit_reason = 0x80000000u | (uint32_t)vm_read(VMCS_VM_INSTR_ERROR);
        return false;
    }
    vm->launched = true;
    vm->exit_reason = (uint32_t)vm_read(VMCS_EXIT_REASON);
    vm->exit_qual = vm_read(VMCS_EXIT_QUALIFICATION);
    vm->exit_instr_len = (uint32_t)vm_read(VMCS_EXIT_INSTR_LEN);
    vm->exit_intr_info = (uint32_t)vm_read(VMCS_EXIT_INTR_INFO);
    uint32_t reason = vm->exit_reason & 0xffff;
    if (reason == EXIT_EPT_VIOLATION || reason == EXIT_EPT_MISCONFIG) {
        vm->exit_gpa = vm_read(VMCS_GUEST_PHYS_ADDR);
        vm->exit_gla = vm_read(VMCS_GUEST_LINEAR_ADDR);
    }
    return true;
}

void vm_skip_instr(vm_t *vm) {
    vm_write(VMCS_GUEST_RIP, vm_read(VMCS_GUEST_RIP) + vm->exit_instr_len);
    // an instruction that completes ends any STI/MOV SS blocking
    uint64_t intr = vm_read(VMCS_GUEST_INTERRUPTIBILITY);
    if (intr & 3) vm_write(VMCS_GUEST_INTERRUPTIBILITY, intr & ~3ULL);
}
