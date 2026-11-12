#ifndef GEN8_ASM_H
#define GEN8_ASM_H
/*
 * A small Gen8 (Broadwell / Cherryview) EU instruction encoder: enough for the
 * pixel shaders the Voodoo2 back end needs. Field positions as in Mesa's
 * src/intel/compiler/elk/elk_inst.h (Gen8 columns); tools/tests/gen8_asm.c
 * checks it against the kernels of IGT GPU Tools bit for bit, and
 * tools/tests/gen8_sim.c executes what it produces.
 *
 * Only the native 128-bit, align1 form is produced: no compaction, no
 * three-source instructions, no control flow (conditions are predicates,
 * selects and masks).
 */
#include <stdint.h>

typedef struct { uint32_t dw[4]; } Gen8Inst;

enum {
    GEN8_OP_MOV = 0x01, GEN8_OP_SEL = 0x02, GEN8_OP_NOT = 0x04, GEN8_OP_AND = 0x05,
    GEN8_OP_OR = 0x06, GEN8_OP_XOR = 0x07, GEN8_OP_SHR = 0x08, GEN8_OP_SHL = 0x09,
    GEN8_OP_ASR = 0x0c, GEN8_OP_CMP = 0x10, GEN8_OP_SEND = 0x31, GEN8_OP_MATH = 0x38,
    GEN8_OP_ADD = 0x40, GEN8_OP_MUL = 0x41, GEN8_OP_FRC = 0x43, GEN8_OP_RNDD = 0x45,
    GEN8_OP_RNDE = 0x46, GEN8_OP_RNDZ = 0x47, GEN8_OP_LZD = 0x4a, GEN8_OP_LINE = 0x59,
    GEN8_OP_PLN = 0x5a, GEN8_OP_MAD = 0x5b, GEN8_OP_LRP = 0x5c, GEN8_OP_NOP = 0x7e,
};

/* register files */
enum { GEN8_ARF = 0, GEN8_GRF = 1, GEN8_IMM = 3 };
/* architecture registers (ARF numbers) */
enum { GEN8_ARF_NULL = 0x00, GEN8_ARF_FLAG = 0x30 };

/* register types; immediates use the same codes plus V (8 x 4-bit signed) */
enum { GEN8_TYPE_UD = 0, GEN8_TYPE_D = 1, GEN8_TYPE_UW = 2, GEN8_TYPE_W = 3, GEN8_TYPE_UB = 4,
       GEN8_TYPE_B = 5, GEN8_TYPE_F = 7, GEN8_IMM_V = 6 };

/* conditional modifiers */
enum { GEN8_COND_NONE = 0, GEN8_COND_Z = 1, GEN8_COND_NZ = 2, GEN8_COND_G = 3, GEN8_COND_GE = 4,
       GEN8_COND_L = 5, GEN8_COND_LE = 6 };

/* math functions (in the conditional-modifier field of MATH) */
enum { GEN8_MATH_INV = 1, GEN8_MATH_LOG = 2, GEN8_MATH_EXP = 3, GEN8_MATH_SQRT = 4 };

/* An operand: a register region or an immediate. */
typedef struct {
    uint8_t file;           /* GEN8_ARF, GEN8_GRF, GEN8_IMM */
    uint8_t reg, subreg;    /* register number, byte offset in it */
    uint8_t type;           /* GEN8_TYPE_x */
    uint8_t vstride, width, hstride;   /* region, as element counts (0,1,2,4,8,16) */
    uint8_t negate, abs;
    uint32_t value;
} Gen8Src;

typedef struct {
    uint8_t file, reg, subreg, type, hstride;
} Gen8Dst;

/* Instruction controls beyond the operands. */
typedef struct {
    uint8_t cond;           /* conditional modifier (or math function for MATH) */
    uint8_t saturate;
    uint8_t predicate;      /* 1: predicated on the flag below (normal) */
    uint8_t pred_inv;
    uint8_t flag_sub;       /* f0.0 or f0.1 for predicate and conditional modifier */
    uint8_t nomask;         /* ignore the execution mask */
} Gen8Ctl;

/* <8;8,1> region of type in reg: a SIMD16 32-bit value is reg and reg + 1 */
Gen8Src gen8_vec(unsigned reg);
Gen8Src gen8_vec_t(unsigned reg, unsigned type);
/* <0;1,0> scalar at reg.subreg (bytes) */
Gen8Src gen8_scalar(unsigned reg, unsigned subreg_bytes);
Gen8Src gen8_scalar_t(unsigned reg, unsigned subreg_bytes, unsigned type);
/* any region */
Gen8Src gen8_region(unsigned reg, unsigned subreg_bytes, unsigned type, unsigned vstride, unsigned width,
                    unsigned hstride);
Gen8Src gen8_immf(float value);
Gen8Src gen8_immud(uint32_t value);
Gen8Src gen8_immd(int32_t value);
Gen8Src gen8_immuw(uint16_t value);
Gen8Src gen8_immv(uint32_t nibbles);    /* 8 signed 4-bit elements, element 0 in bits 3:0 */
Gen8Src gen8_flag(unsigned sub);        /* f0.sub as UW */
Gen8Src gen8_null_src(unsigned type);
Gen8Src gen8_neg(Gen8Src s);

Gen8Dst gen8_dst(unsigned reg, unsigned subreg_bytes, unsigned type);
Gen8Dst gen8_dst_flag(unsigned sub);
Gen8Dst gen8_dst_null(unsigned type);

Gen8Inst gen8_inst(unsigned opcode, unsigned exec_size, Gen8Dst dst, Gen8Src src0, Gen8Src src1, Gen8Ctl ctl);

/* exec_size: 8 or 16. dst is a GRF with stride 1 at dst_reg.dst_sub. */
Gen8Inst gen8_alu(unsigned opcode, unsigned exec_size, unsigned dst_reg, unsigned dst_sub, unsigned dst_type,
                  Gen8Src src0, Gen8Src src1, unsigned cond, unsigned saturate);
/* pln(16) dst<1>F plane<0;1,0>F g2<8;8,1>F: an attribute from the pixel
 * barycentrics in g2-g5 */
Gen8Inst gen8_pln(unsigned dst_reg, unsigned plane_reg, unsigned plane_sub);
/* send(16) with an immediate descriptor; sfid: 2 sampler, 5 render cache */
Gen8Inst gen8_send(unsigned sfid, unsigned dst_reg, unsigned src_reg, uint32_t desc, unsigned eot);

/* message descriptors */
#define GEN8_SFID_SAMPLER 2
#define GEN8_SFID_RENDER 5
/* sampler ld, SIMD16, u and lod (4 registers in), 8 registers back */
#define GEN8_DESC_LD16(bt) ((4u << 25) | (8u << 20) | (2u << 17) | (7u << 12) | (bt))
/* render target write, SIMD16 single source, header + 8 registers */
#define GEN8_DESC_RTW16_HDR(bt, last) ((10u << 25) | (1u << 19) | (12u << 14) | ((last) ? 1u << 12 : 0) | (bt))

#endif
