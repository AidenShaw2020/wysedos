// Pico SDK names the PC model uses, mapped onto the Wyse monitor (x86-64).
#pragma once
#include <stdint.h>

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

// The model publishes ring pointers between "cores" with __dmb(); on x86 stores
// are ordered already, a compiler barrier plus mfence keeps the intent.
#ifndef __dmb
#define __dmb() __asm__ __volatile__("mfence" ::: "memory")
#endif

typedef unsigned int uint;
uint32_t time_us_32(void);
uint64_t time_us_64(void);
void sleep_us(uint32_t us);
void sleep_ms(uint32_t ms);
int get_core_num(void);
int32_t __fast_mul(int32_t a, int32_t b);

uint32_t get_uticks(void);

// TSC ticks -> microseconds as 32.32 fixed point, set up by hal.c
extern uint64_t g_uticks_scale;
static inline uint32_t get_uticks_fast(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t c = ((uint64_t)hi << 32) | lo;
    return (uint32_t)(((unsigned __int128)c * g_uticks_scale) >> 32);
}
