#ifndef WYSE_GUEST_CPU_FEATURES_H
#define WYSE_GUEST_CPU_FEATURES_H
#include <stdint.h>

/* Keep enumeration consistent with this single-CPU, legacy-mode VMX backend.
 * In particular CR4.PCIDE/OSXSAVE are unavailable and MTRRs are not emulated. */
static inline void wyse_guest_cpuid_filter(uint32_t leaf, uint32_t *a,
                                          uint32_t *b, uint32_t *c, uint32_t *d)
{
    if (leaf == 0) {
        if (*a > 0xd) *a = 0xd;
    } else if (leaf == 1) {
        *b &= 0x0000ffff;
        *c &= ~((1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) |
                (1u << 12) | (1u << 14) | (1u << 15) | (1u << 17) |
                (1u << 21) | (1u << 24) | (1u << 26) | (1u << 27) |
                (1u << 28) | (1u << 29) | (1u << 31));
        *d &= ~((1u << 3) | (1u << 6) | (1u << 9) | (1u << 12) |
                (1u << 17) | (1u << 21) | (1u << 22) | (1u << 27) |
                (1u << 28) | (1u << 29) | (1u << 31));
    } else if (leaf == 0x80000001) {
        *d &= ~((1u << 11) | (1u << 20) | (1u << 27) | (1u << 29));
        *c &= ~(1u << 0);
    } else if ((leaf >= 0x40000000 && leaf < 0x50000000) ||
               leaf == 4 || leaf == 7 || leaf == 0xb || leaf == 0xd || leaf == 0x1f) {
        /* No extended state, unsupported structured features or host topology. */
        *a = *b = *c = *d = 0;
    }
}

static inline unsigned wyse_guest_cpl(uint64_t cr0, uint64_t flags, uint64_t cs)
{
    if (!(cr0 & 1)) return 0;
    if (flags & (1u << 17)) return 3;
    return (unsigned)cs & 3;
}
#endif
