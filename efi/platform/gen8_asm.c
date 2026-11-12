/*
 * Gen8 EU instruction encoder; see gen8_asm.h. Bit positions are the Gen8
 * columns of Mesa's elk_inst.h (native 128-bit format, align1).
 */
#include "gen8_asm.h"

static void set(Gen8Inst *in, unsigned hi, unsigned lo, uint32_t value)
{
    for (unsigned bit = lo; bit <= hi; bit++) {
        uint32_t *word = &in->dw[bit / 32];
        uint32_t mask = 1u << (bit % 32);
        if ((value >> (bit - lo)) & 1) *word |= mask;
        else *word &= ~mask;
    }
}

/* element counts to the encoded region fields */
static unsigned vstride_code(unsigned n) { return n == 0 ? 0 : n == 1 ? 1 : n == 2 ? 2 : n == 4 ? 3 : n == 8 ? 4 : n == 16 ? 5 : 6; }
static unsigned width_code(unsigned n) { return n == 1 ? 0 : n == 2 ? 1 : n == 4 ? 2 : n == 8 ? 3 : 4; }
static unsigned hstride_code(unsigned n) { return n == 0 ? 0 : n == 1 ? 1 : n == 2 ? 2 : 3; }
static unsigned exec_code(unsigned n) { return n == 1 ? 0 : n == 2 ? 1 : n == 4 ? 2 : n == 8 ? 3 : 4; }

Gen8Src gen8_region(unsigned reg, unsigned subreg_bytes, unsigned type, unsigned vstride, unsigned width,
                    unsigned hstride)
{
    Gen8Src s = { GEN8_GRF, (uint8_t)reg, (uint8_t)subreg_bytes, (uint8_t)type, (uint8_t)vstride, (uint8_t)width,
                  (uint8_t)hstride, 0, 0, 0 };
    return s;
}

Gen8Src gen8_vec_t(unsigned reg, unsigned type) { return gen8_region(reg, 0, type, 8, 8, 1); }
Gen8Src gen8_vec(unsigned reg) { return gen8_vec_t(reg, GEN8_TYPE_F); }
Gen8Src gen8_scalar_t(unsigned reg, unsigned subreg_bytes, unsigned type)
{
    return gen8_region(reg, subreg_bytes, type, 0, 1, 0);
}
Gen8Src gen8_scalar(unsigned reg, unsigned subreg_bytes) { return gen8_scalar_t(reg, subreg_bytes, GEN8_TYPE_F); }

static Gen8Src imm(unsigned type, uint32_t value)
{
    Gen8Src s = { GEN8_IMM, 0, 0, (uint8_t)type, 0, 0, 0, 0, 0, value };
    return s;
}

Gen8Src gen8_immf(float value)
{
    union { float f; uint32_t u; } v = { value };
    return imm(GEN8_TYPE_F, v.u);
}
Gen8Src gen8_immud(uint32_t value) { return imm(GEN8_TYPE_UD, value); }
Gen8Src gen8_immd(int32_t value) { return imm(GEN8_TYPE_D, (uint32_t)value); }
Gen8Src gen8_immuw(uint16_t value) { return imm(GEN8_TYPE_UW, (uint32_t)value | (uint32_t)value << 16); }
Gen8Src gen8_immv(uint32_t nibbles) { return imm(GEN8_IMM_V, nibbles); }

Gen8Src gen8_flag(unsigned sub)
{
    Gen8Src s = { GEN8_ARF, GEN8_ARF_FLAG, (uint8_t)(sub * 2), GEN8_TYPE_UW, 0, 1, 0, 0, 0, 0 };
    return s;
}

Gen8Src gen8_null_src(unsigned type)
{
    Gen8Src s = { GEN8_ARF, GEN8_ARF_NULL, 0, (uint8_t)type, 0, 1, 0, 0, 0, 0 };
    return s;
}

Gen8Src gen8_neg(Gen8Src s) { s.negate ^= 1; return s; }

Gen8Dst gen8_dst(unsigned reg, unsigned subreg_bytes, unsigned type)
{
    Gen8Dst d = { GEN8_GRF, (uint8_t)reg, (uint8_t)subreg_bytes, (uint8_t)type, 1 };
    return d;
}

Gen8Dst gen8_dst_flag(unsigned sub)
{
    Gen8Dst d = { GEN8_ARF, GEN8_ARF_FLAG, (uint8_t)(sub * 2), GEN8_TYPE_UW, 1 };
    return d;
}

Gen8Dst gen8_dst_null(unsigned type)
{
    Gen8Dst d = { GEN8_ARF, GEN8_ARF_NULL, 0, (uint8_t)type, 1 };
    return d;
}

static void put_src0(Gen8Inst *in, Gen8Src s)
{
    set(in, 42, 41, s.file);
    set(in, 46, 43, s.type);
    if (s.file == GEN8_IMM) {
        in->dw[3] = s.value;
        return;
    }
    set(in, 68, 64, s.subreg);
    set(in, 76, 69, s.reg);
    set(in, 77, 77, s.abs);
    set(in, 78, 78, s.negate);
    set(in, 81, 80, hstride_code(s.hstride));
    set(in, 84, 82, width_code(s.width));
    set(in, 88, 85, vstride_code(s.vstride));
}

static void put_src1(Gen8Inst *in, Gen8Src s)
{
    set(in, 90, 89, s.file);
    set(in, 94, 91, s.type);
    if (s.file == GEN8_IMM) {
        in->dw[3] = s.value;
        return;
    }
    set(in, 100, 96, s.subreg);
    set(in, 108, 101, s.reg);
    set(in, 109, 109, s.abs);
    set(in, 110, 110, s.negate);
    set(in, 113, 112, hstride_code(s.hstride));
    set(in, 116, 114, width_code(s.width));
    set(in, 120, 117, vstride_code(s.vstride));
}

static void put_dst(Gen8Inst *in, Gen8Dst d)
{
    set(in, 36, 35, d.file);
    set(in, 40, 37, d.type);
    set(in, 52, 48, d.subreg);
    set(in, 60, 53, d.reg);
    set(in, 62, 61, hstride_code(d.hstride));
}

static int one_source(unsigned opcode)
{
    return opcode == GEN8_OP_MOV || opcode == GEN8_OP_NOT || opcode == GEN8_OP_FRC || opcode == GEN8_OP_RNDD ||
           opcode == GEN8_OP_RNDE || opcode == GEN8_OP_RNDZ || opcode == GEN8_OP_LZD;
}

Gen8Inst gen8_inst(unsigned opcode, unsigned exec_size, Gen8Dst dst, Gen8Src src0, Gen8Src src1, Gen8Ctl ctl)
{
    Gen8Inst in = { { 0, 0, 0, 0 } };
    set(&in, 6, 0, opcode);
    set(&in, 19, 16, ctl.predicate ? 1 : 0);
    set(&in, 20, 20, ctl.pred_inv);
    set(&in, 23, 21, exec_code(exec_size));
    set(&in, 27, 24, ctl.cond);
    set(&in, 31, 31, ctl.saturate);
    set(&in, 32, 32, ctl.flag_sub);
    set(&in, 34, 34, ctl.nomask);
    put_dst(&in, dst);
    put_src0(&in, src0);
    /* a one-source instruction leaves src1 as ARF null (all zero) */
    if (!one_source(opcode)) put_src1(&in, src1);
    return in;
}

Gen8Inst gen8_alu(unsigned opcode, unsigned exec_size, unsigned dst_reg, unsigned dst_sub, unsigned dst_type,
                  Gen8Src src0, Gen8Src src1, unsigned cond, unsigned saturate)
{
    Gen8Ctl ctl = { (uint8_t)cond, (uint8_t)saturate, 0, 0, 0, 0 };
    return gen8_inst(opcode, exec_size, gen8_dst(dst_reg, dst_sub, dst_type), src0, src1, ctl);
}

Gen8Inst gen8_pln(unsigned dst_reg, unsigned plane_reg, unsigned plane_sub)
{
    return gen8_alu(GEN8_OP_PLN, 16, dst_reg, 0, GEN8_TYPE_F, gen8_scalar(plane_reg, plane_sub), gen8_vec(2),
                    GEN8_COND_NONE, 0);
}

Gen8Inst gen8_send(unsigned sfid, unsigned dst_reg, unsigned src_reg, uint32_t desc, unsigned eot)
{
    Gen8Inst in = { { 0, 0, 0, 0 } };
    set(&in, 6, 0, GEN8_OP_SEND);
    set(&in, 23, 21, exec_code(16));
    set(&in, 27, 24, sfid);
    /* a render-target write answers nothing: null */
    put_dst(&in, eot ? gen8_dst_null(GEN8_TYPE_UW) : gen8_dst(dst_reg, 0, GEN8_TYPE_UW));
    if (eot) set(&in, 60, 53, dst_reg);   /* IGT's kernels keep the number on the null register */
    put_src0(&in, gen8_vec(src_reg));
    set(&in, 90, 89, 3);           /* the descriptor, an immediate dword */
    set(&in, 94, 91, GEN8_TYPE_D);
    in.dw[3] = (desc & 0x7fffffffu) | (eot ? 0x80000000u : 0);
    return in;
}
