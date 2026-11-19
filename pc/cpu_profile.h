#ifndef WYSE_CPU_PROFILE_H
#define WYSE_CPU_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

/* Values are persisted in [pc] cpu. Both later profiles are CPUID family 6. */
enum {
    PC_CPU_386 = 3,
    PC_CPU_486 = 4,
    PC_CPU_PENTIUM = 5,
    PC_CPU_PENTIUM_II = 6,
    PC_CPU_PENTIUM_III = 7,
};

static inline bool pc_cpu_profile_valid(int profile)
{
    return profile >= PC_CPU_386 && profile <= PC_CPU_PENTIUM_III;
}

static inline bool pc_cpu_is_p6(int profile)
{
    return profile == PC_CPU_PENTIUM_II || profile == PC_CPU_PENTIUM_III;
}

static inline const char *pc_cpu_profile_name(int profile)
{
    switch (profile) {
    case PC_CPU_386:         return "386";
    case PC_CPU_486:         return "486";
    case PC_CPU_PENTIUM:     return "Pentium";
    case PC_CPU_PENTIUM_II:  return "Pentium II";
    case PC_CPU_PENTIUM_III: return "Pentium III";
    default:                return "?";
    }
}

/* Early x86 identities for the P6 profiles. Advertise only capabilities
 * the backend supplies. The extended brand string gives newer guests a
 * familiar name without exposing host leaves or a host clock. */
static inline void pc_cpu_p6_cpuid(int profile, uint32_t leaf, uint32_t supported,
                                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    const bool sse_model = profile == PC_CPU_PENTIUM_III;
    const uint32_t basic_features =
        (1u << 0) | (1u << 4) | (1u << 5) | (1u << 8) | (1u << 11) |
        (1u << 13) | (1u << 15) | (1u << 16) | (1u << 23);
    const uint32_t fxsr = 1u << 24, sse = 1u << 25;
    *a = *b = *c = *d = 0;
    switch (leaf) {
    case 0:
        *a = 2;
        *b = 0x756e6547;  /* GenuineIntel */
        *d = 0x49656e69;
        *c = 0x6c65746e;
        break;
    case 1:
        *a = sse_model ? 0x06b1 : 0x0652;
        *d = supported & basic_features;
        if (sse_model && (supported & (fxsr | sse)) == (fxsr | sse))
            *d |= fxsr | sse;
        break;
    case 2:
        *a = 1;  /* One query, with no synthetic cache descriptors. */
        break;
    case 0x80000000u:
        *a = 0x80000004u;
        break;
    case 0x80000002u:
    case 0x80000003u:
    case 0x80000004u: {
        static const char pentium_ii[] = "Intel(R) Pentium(R) II";
        static const char pentium_iii[] = "Intel(R) Pentium(R) III";
        const char *brand = sse_model ? pentium_iii : pentium_ii;
        const unsigned length = sse_model ? sizeof pentium_iii - 1 : sizeof pentium_ii - 1;
        uint32_t *registers[] = {a, b, c, d};
        unsigned pos = (leaf - 0x80000002u) * 16;
        for (unsigned reg = 0; reg < 4; ++reg)
            for (unsigned byte = 0; byte < 4 && pos < length; ++byte, ++pos)
                *registers[reg] |= (uint32_t)(unsigned char)brand[pos] << (byte * 8);
        break;
    }
    }
}

#endif
