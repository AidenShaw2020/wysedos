#ifndef WYSE_X87_STORE_H
#define WYSE_X87_STORE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct __attribute__((aligned(16))) {
    uint16_t control, status;
    uint8_t tag, reserved0;
    uint16_t opcode;
    uint64_t ip, dp;
    uint32_t mxcsr, mxcsr_mask;
    uint8_t registers[8][16], xmm[16][16], reserved[96];
} WyseFxState;

_Static_assert(sizeof(WyseFxState) == 512, "FXSAVE area size");

static inline bool wyse_x87_store(const WyseFxState *guest, WyseFxState *next,
                                  uint64_t *bits, bool wide, bool pop)
{
    if (!guest || !next || !bits || (guest->status & ~guest->control & 0x3f))
        return false;
    WyseFxState host;
    *bits = 0;
#define WYSE_X87_STORE(instruction) __asm__ volatile( \
    "fxsave64 %[host]\n\tfxrstor64 %[guest]\n\t" instruction " %[bits]\n\t" \
    "fxsave64 %[next]\n\tfxrstor64 %[host]" \
    : [host] "=m"(host), [next] "=m"(*next), [bits] "=m"(*bits) \
    : [guest] "m"(*guest) : "memory")
    if (wide) {
        if (pop) { WYSE_X87_STORE("fstpl"); }
        else { WYSE_X87_STORE("fstl"); }
    } else {
        if (pop) { WYSE_X87_STORE("fstps"); }
        else { WYSE_X87_STORE("fsts"); }
    }
#undef WYSE_X87_STORE
    return true;
}

#endif