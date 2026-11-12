/*
 * The Gen8 encoder against kernels that run on the hardware: IGT GPU Tools'
 * blit.g7a pixel shader and its "write 1.0" variant (lib/rendercopy_gen8.c),
 * both physically verified in wyseDOS ("gpu copy", "gpu fill").
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../efi/platform/gen8_asm.h"

static void same(Gen8Inst got, uint32_t a, uint32_t b, uint32_t c, uint32_t d, const char *what)
{
    if (got.dw[0] == a && got.dw[1] == b && got.dw[2] == c && got.dw[3] == d) return;
    printf("%s: got %08x %08x %08x %08x, want %08x %08x %08x %08x\n", what,
           got.dw[0], got.dw[1], got.dw[2], got.dw[3], a, b, c, d);
    assert(0);
}

int main(void)
{
    /* blit.g7a */
    same(gen8_pln(122, 6, 0), 0x0080005a, 0x2f403ae8, 0x3a0000c0, 0x008d0040, "pln u");
    same(gen8_pln(124, 6, 16), 0x0080005a, 0x2f803ae8, 0x3a0000d0, 0x008d0040, "pln v");
    same(gen8_send(2, 113, 122, 0x08840001, 0), 0x02800031, 0x2e203a48, 0x0e8d0f40, 0x08840001, "sample");
    same(gen8_send(5, 0, 113, 0x10031000, 1), 0x05800031, 0x20003a40, 0x0e8d0e20, 0x90031000, "rt write");
    /* the write-1.0 kernel's moves */
    for (unsigned r = 0; r < 8; r++)
        same(gen8_alu(GEN8_OP_MOV, 8, 112 + r, 0, GEN8_TYPE_UD, gen8_immud(0x3f800000), gen8_immud(0), 0, 0),
             0x00600001, 0x2e000608 + (r << 21), 0x00000000, 0x3f800000, "mov 1.0");
    /* two GRF sources: the fields land where the one-source tests put them */
    Gen8Inst mul = gen8_alu(GEN8_OP_MUL, 16, 113, 0, GEN8_TYPE_F, gen8_vec(113), gen8_vec(102), 0, 0);
    assert(mul.dw[0] == 0x00800041 && mul.dw[1] == 0x2e203ae8);
    assert(mul.dw[2] == (0x3a000000u | 0x008d0000u | (113u << 5)));
    assert(mul.dw[3] == (0x008d0000u | (102u << 5)));
    Gen8Inst add = gen8_alu(GEN8_OP_ADD, 16, 113, 0, GEN8_TYPE_F, gen8_vec(113), gen8_immf(1.0f), 0, 1);
    assert(add.dw[0] == 0x80800040 && add.dw[3] == 0x3f800000 && ((add.dw[2] >> 25) & 3) == 3);
    puts("Gen8 encoder reproduces IGT's physically verified kernels: PASS");
    return 0;
}
