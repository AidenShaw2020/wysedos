#ifndef GEN8_SIM_H
#define GEN8_SIM_H
/*
 * Host-side interpreter for the Gen8 instructions efi/platform/gen8_asm.c
 * produces: it decodes the 128-bit encodings (so it checks the encoder too)
 * and runs one SIMD16 pixel-shader thread. Messages (SEND) go to a callback.
 * Semantics follow the Gen8 PRM as Mesa uses it; it is a test model, the
 * hardware decides (the physical Wyse tests).
 */
#include <stdbool.h>
#include <stdint.h>
#include "../../efi/platform/gen8_asm.h"

typedef struct {
    uint8_t grf[128][32];
    uint16_t flag[2];          /* f0.0, f0.1 */
    uint16_t exec_mask;        /* dispatched pixels */
    char error[160];
} Gen8Thread;

/* sfid, descriptor (with EOT in bit 31), message registers, answer registers */
typedef void (*Gen8SendFn)(void *ctx, Gen8Thread *thread, unsigned sfid, uint32_t desc, unsigned src,
                           unsigned dst, uint16_t exec_mask);

/* Runs until the EOT send; false (thread->error set) on anything not modelled. */
extern int gen8_sim_trace;   /* print every instruction's result */
bool gen8_sim_run(Gen8Thread *thread, const Gen8Inst *code, unsigned count, Gen8SendFn send, void *ctx);

#endif
