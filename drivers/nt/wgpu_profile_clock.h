/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WGPU_NT_PROFILE_CLOCK_H
#define WGPU_NT_PROFILE_CLOCK_H

/* Wyse-only diagnostic build. Its guest exposes the same TSC to both
 * kernel drivers. LFENCE orders the readings without querying the ACPI
 * timer on every draw. Timing replies use frequency zero for raw ticks. */
static __inline unsigned long long wgpu_profile_ticks(void)
{
    unsigned int lo, hi;
    __asm__ __volatile__("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((unsigned long long)hi << 32) | lo;
}

#endif
