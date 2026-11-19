/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WGPU_CPU_QUERY_H
#define WGPU_CPU_QUERY_H

#include "../../pc/wgpu_cpu.h"

static inline uint32_t wgpu_nt_cpu_caps(void)
{
    uint32_t a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    uint32_t caps = wgpu_cpu_native_caps(b, d);
    if (caps != WGPU_CPU_CAPS) {
        __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                           : "a"(WGPU_CPU_QUERY), "c"(0));
        if (a == WGPU_CPU_SIGNATURE)
            caps |= d & WGPU_CPU_CAPS;
    }
    return caps;
}

#endif
