// Write-combining for the GOP framebuffer.
//
// UEFI maps the framebuffer uncached: every pixel store is a separate bus
// transaction (~110 ns on this Atom), so a full 1440x1080 frame took 180 ms.
// Write-combining batches them. The host page tables are the firmware's
// identity map; the PAT entry selected by PAT=1/PCD=0/PWT=0 (index 4) is
// reprogrammed to WC (the firmware does not use the PAT bit) and the PDEs
// covering the framebuffer get the PAT bit. A 1 GB page is split into 2 MB
// pages first so that neighbouring MMIO keeps its type.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "../include/efi.h"

extern EFI_BOOT_SERVICES *gBS;

static inline uint64_t rdmsr_(uint32_t m) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return ((uint64_t)hi << 32) | lo; }
static inline void wrmsr_(uint32_t m, uint64_t v) { __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }
static inline uint64_t cr3_(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }

#define P_PRESENT   1ULL
#define P_PWT       (1ULL << 3)
#define P_PCD       (1ULL << 4)
#define P_PS        (1ULL << 7)
#define P_PAT_LARGE (1ULL << 12)
#define ADDR_MASK   0x000ffffffffff000ULL

static uint64_t *alloc_table(void) {
    EFI_PHYSICAL_ADDRESS a;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &a))) return NULL;
    return (uint64_t *)a;
}

bool wc_map(uint64_t base, uint64_t size) {
    uint64_t *pml4 = (uint64_t *)(cr3_() & ADDR_MASK);
    uint64_t start = base & ~0x1fffffULL, end = (base + size + 0x1fffff) & ~0x1fffffULL;
    int done = 0;

    // PAT index 4 -> WC (type 1); keep the others
    uint64_t pat = rdmsr_(0x277);
    pat = (pat & ~(0xffULL << 32)) | (1ULL << 32);
    __asm__ volatile("wbinvd");
    wrmsr_(0x277, pat);

    for (uint64_t a = start; a < end; a += 0x200000) {
        uint64_t *e4 = &pml4[(a >> 39) & 511];
        if (!(*e4 & P_PRESENT)) return false;
        uint64_t *pdpt = (uint64_t *)(*e4 & ADDR_MASK);
        uint64_t *e3 = &pdpt[(a >> 30) & 511];
        if (!(*e3 & P_PRESENT)) return false;
        if (*e3 & P_PS) {
            // split the 1 GB page into 2 MB pages with the same attributes
            uint64_t *pd = alloc_table();
            if (!pd) return false;
            uint64_t gbase = *e3 & 0x000fffffc0000000ULL;
            uint64_t attrs = *e3 & 0x8000000000001fffULL;   // NX, PAT(12), flags incl. PS
            for (int i = 0; i < 512; i++) pd[i] = (gbase + ((uint64_t)i << 21)) | attrs;
            *e3 = (uint64_t)pd | (attrs & 0x7) | P_PRESENT;
        }
        uint64_t *pd = (uint64_t *)(*e3 & ADDR_MASK);
        uint64_t *e2 = &pd[(a >> 21) & 511];
        if (!(*e2 & P_PRESENT)) return false;
        if (!(*e2 & P_PS)) {
            // 4 KB pages: PAT is bit 7 in a PTE
            uint64_t *pt = (uint64_t *)(*e2 & ADDR_MASK);
            for (int i = 0; i < 512; i++)
                if (pt[i] & P_PRESENT) pt[i] = (pt[i] & ~(P_PCD | P_PWT)) | (1ULL << 7);
        } else {
            *e2 = (*e2 & ~(P_PCD | P_PWT)) | P_PAT_LARGE;
        }
        done++;
    }
    // flush translations and caches
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    __asm__ volatile("wbinvd");
    printf("wc: %d x 2 MB of framebuffer at %llx write-combining, PAT %016llx\n", done,
           (unsigned long long)base, (unsigned long long)rdmsr_(0x277));
    return true;
}

// The memory type of [base, base+size) in the host's identity map, 4 KB at a time:
// type 0 write-back, 3 uncached (PCD|PWT), 4 write-combining (the PAT entry above).
// 1 GB and 2 MB pages are split where the range does not cover them.
bool page_set_type(uint64_t base, uint64_t size, int type) {
    uint64_t *pml4 = (uint64_t *)(cr3_() & ADDR_MASK);
    uint64_t pte_bits = (type & 1 ? P_PWT : 0) | (type & 2 ? P_PCD : 0) | (type & 4 ? 1ULL << 7 : 0);
    for (uint64_t a = base & ~0xfffULL; a < base + size; a += 4096) {
        uint64_t *e4 = &pml4[(a >> 39) & 511];
        if (!(*e4 & P_PRESENT)) return false;
        uint64_t *pdpt = (uint64_t *)(*e4 & ADDR_MASK);
        uint64_t *e3 = &pdpt[(a >> 30) & 511];
        if (!(*e3 & P_PRESENT)) return false;
        if (*e3 & P_PS) {
            uint64_t *pd = alloc_table();
            if (!pd) return false;
            uint64_t gbase = *e3 & 0x000fffffc0000000ULL;
            uint64_t attrs = *e3 & 0x8000000000001fffULL;
            for (int i = 0; i < 512; i++) pd[i] = (gbase + ((uint64_t)i << 21)) | attrs;
            *e3 = (uint64_t)pd | (attrs & 0x7) | P_PRESENT;
        }
        uint64_t *pd = (uint64_t *)(*e3 & ADDR_MASK);
        uint64_t *e2 = &pd[(a >> 21) & 511];
        if (!(*e2 & P_PRESENT)) return false;
        if (*e2 & P_PS) {
            // a 2 MB page into 4 KB ones: the PAT bit moves from bit 12 to bit 7
            uint64_t *pt = alloc_table();
            if (!pt) return false;
            uint64_t mbase = *e2 & 0x000fffffffe00000ULL;
            uint64_t attrs = (*e2 & 0x800000000000007fULL) & ~P_PS;
            if (*e2 & P_PAT_LARGE) attrs |= 1ULL << 7;
            for (int i = 0; i < 512; i++) pt[i] = (mbase + ((uint64_t)i << 12)) | attrs;
            *e2 = (uint64_t)pt | (*e2 & 0x7) | P_PRESENT;
        }
        uint64_t *pt = (uint64_t *)(*e2 & ADDR_MASK);
        uint64_t *e1 = &pt[(a >> 12) & 511];
        if (!(*e1 & P_PRESENT)) return false;
        *e1 = (*e1 & ~(P_PWT | P_PCD | (1ULL << 7))) | pte_bits;
    }
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    __asm__ volatile("wbinvd");
    return true;
}
