#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../pc/cpu_profile.h"
#include "../../efi/src/guest_cpu_features.h"

struct Registers {
    uint32_t a, b, c, d;
};

static struct Registers read_profile(int profile, uint32_t leaf, uint32_t caps)
{
    struct Registers r;
    pc_cpu_p6_cpuid(profile, leaf, caps, &r.a, &r.b, &r.c, &r.d);
    return r;
}

static void check_profile(int model, uint32_t signature, const char *brand)
{
    struct Registers r = read_profile(model, 0, 0);
    const uint32_t vendor[] = {r.b, r.d, r.c};
    assert(r.a == 2 && !memcmp(vendor, "GenuineIntel", sizeof vendor));
    r = read_profile(model, 0x80000000u, 0);
    assert(r.a == 0x80000004u && !r.b && !r.c && !r.d);
    uint32_t name[12];
    for (unsigned i = 0; i < 3; ++i) {
        r = read_profile(model, 0x80000002u + i, 0);
        const uint32_t chunk[] = {r.a, r.b, r.c, r.d};
        memcpy(&name[i * 4], chunk, sizeof chunk);
    }
    assert(!strcmp((const char *)name, brand));
    r = read_profile(model, 1, UINT32_MAX);
    assert(r.a == signature && !r.b && !r.c);
    const uint32_t sse = (1u << 24) | (1u << 25);
    assert((r.d & sse) == (model == PC_CPU_PENTIUM_III ? sse : 0));
    assert((r.d & ((1u << 11) | (1u << 15) | (1u << 23))) ==
           ((1u << 11) | (1u << 15) | (1u << 23)));
    assert(!(r.d & ((1u << 6) | (1u << 12) | (1u << 26) | (1u << 28))));
    r = read_profile(model, 1, 1u << 25);
    assert(!r.d);
    r = read_profile(model, 7, UINT32_MAX);
    assert(!r.a && !r.b && !r.c && !r.d);
    r = read_profile(model, 0x80000001u, UINT32_MAX);
    assert(!r.a && !r.b && !r.c && !r.d);
}

int main(void)
{
    assert(!pc_cpu_is_p6(PC_CPU_PENTIUM) && !pc_cpu_profile_valid(0));
    check_profile(PC_CPU_PENTIUM_II, 0x0652, "Intel(R) Pentium(R) II");
    check_profile(PC_CPU_PENTIUM_III, 0x06b1, "Intel(R) Pentium(R) III");
    uint32_t a = UINT32_MAX, b = a, c = a, d = a;
    wyse_guest_cpuid_filter(1, &a, &b, &c, &d);
    assert((d & 0x03000000u) == 0x03000000u);
    assert(!(d & ((1u << 6) | (1u << 12))));
    puts("PASS: Pentium II/III signatures, brand, SSE dependencies, unsupported leaves");
    return 0;
}
