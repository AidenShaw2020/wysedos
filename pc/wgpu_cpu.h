/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WGPU_CPU_H
#define WGPU_CPU_H

#include <stdint.h>

/* Private VMX query for WGPU cache-coherence instructions, independent of the
 * emulated CPU's architectural CPUID. EAX identifies the reply and EDX holds
 * the supported instruction groups. EBX and ECX are reserved. */
#define WGPU_CPU_QUERY 0x40005747u
#define WGPU_CPU_SIGNATURE 0x55504757u
#define WGPU_CPU_FENCE (1u << 0)
#define WGPU_CPU_FLUSH64 (1u << 1)
#define WGPU_CPU_CAPS (WGPU_CPU_FENCE | WGPU_CPU_FLUSH64)

static inline uint32_t wgpu_cpu_native_caps(uint32_t ebx, uint32_t edx)
{
    if (!(edx & (1u << 26))) return 0;
    uint32_t caps = WGPU_CPU_FENCE;
    if ((edx & (1u << 19)) && ((ebx >> 8) & 255u) == 8)
        caps |= WGPU_CPU_FLUSH64;
    return caps;
}

#endif
