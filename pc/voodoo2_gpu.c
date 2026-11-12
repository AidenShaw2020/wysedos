/*
 * Voodoo2 triangles on an Intel Gen8 GPU, portable half; see voodoo2_gpu.h.
 *
 * The pixel shader below is generated per mode (the registers that choose
 * code paths) and follows voodoo_half_triangle() in
 * pc/voodoo/86box/vid_voodoo_render.c step by step; the 86Box statements are
 * quoted where the translation is not obvious. Everything per pixel is
 * computed for all 16 pixels of a thread; conditions are masks (0 or -1 in a
 * dword per pixel) and "goto skip_pixel" clears the pixel's bit in "alive".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voodoo2_gpu.h"
#include "voodoo/voodoo_port.h"
#include <86box/vid_voodoo_common.h>
#include <86box/vid_voodoo_regs.h>
#include <86box/vid_voodoo_render.h>
#include <86box/vid_voodoo_texture.h>
#include <86box/vid_voodoo_dither.h>
#include "../efi/platform/gen8_asm.h"

/* ---- the per-draw record (push constants) ---- */

enum {
    R_XA = 0,            /* column the parameters start from: ((vertexAx << 12) + 0x7000) >> 16 */
    R_YREF = 1,          /* line they start from (before clipping) */
    R_YSTART = 2,        /* lines drawn: [YSTART, YEND) */
    R_YEND = 3,
    R_SPAN = 4,          /* data index of the span of line YSTART: x_lo | x_hi << 16 */
    R_YFLIP = 5,         /* y origin: frame-buffer row = YFLIP - line */
    R_COLROW = 6,        /* frame-buffer element (16 bits) of draw-buffer row 0 */
    R_AUXROW = 7,
    R_ROWEL = 8,         /* elements per row */
    R_PLANE = 9,         /* r, g, b, a, z: start, dX, dY (integer, 3 dwords each) */
    R_W = 24,            /* W: start, dX, dY (float, units of 2^32) */
    R_TMU = 27,          /* per unit 9 floats: s, t, w planes */
    R_COLOR0 = 45, R_COLOR1 = 46, R_ZACOLOR = 47, R_ZBIAS = 48, R_CHROMA = 49, R_FOGCOLOR = 50, R_AREF = 51,
    R_LOD = 52,          /* per unit: triangle base LOD (8.8, voodoo_triangle_lod_base) */
    R_LODBIAS = 54, R_LODMIN = 56, R_LODMAX = 58,
    R_LODTAB = 60,       /* per unit: DATA4 index of 9 entries {w_mask, h_mask, tex_lod, texel index} */
    R_FOGTAB = 62,       /* DATA index of 64 entries fog | dfog << 8 */
    R_TMUCONFIG = 63,
    R_DETAILMAX = 64, R_DETAILBIAS = 66, R_DETAILSCALE = 68,
    R_COUNT = 70,
};

_Static_assert(R_COUNT <= V2G_RECORD_DWORDS + 16, "record");

/* Registers: g0-g1 payload, g2-g5 barycentrics, g6-g15 record, g16-g17 the one
 * (unused) attribute, g96-g99 ld message, g100-g107 ld answer, g112-g121
 * render-target write; values (pairs) in g18-g95, g108-g111 and g122-g127. */
#define REC_GRF 6
#define VAL_FIRST 18
#define VAL_LAST 94
#define LD_MSG 96
#define LD_ANSWER 100
#define RT_MSG 112

#define TABLE_RB 0
#define TABLE_G 4096
#define TABLE_RB2 8192
#define TABLE_G2 9216
#define TABLE_SUB_RB 10240
#define TABLE_SUB_G 14336
#define TABLE_SUB_RB2 18432
#define TABLE_SUB_G2 19456
#define TABLE_BYTES 20480

#define MAX_CODE 1600
#define KERNEL_SLOTS 64
#define SPAN_LINES 4096
#define COVER_WIDTH 2048
#define COVER_WORDS (COVER_WIDTH / 64)
#define COVER_ROWS 4096

typedef struct {
    uint32_t fbzMode, fbzColorPath, alphaMode, fogMode, textureMode[2], tLOD_mirror[2], flags;
} KernelKey;

typedef struct {
    KernelKey key;
    bool used;
    unsigned id, count;
    Gen8Inst *code;
} Kernel;

struct V2gState {
    V2gBackend backend;
    Kernel kernels[KERNEL_SLOTS];
    unsigned next_slot, next_id;
    uint8_t texture_used[2][TEX_CACHE_MAX];   /* refcount_r increments owed when the GPU is done */
    uint32_t texture_owed[2][TEX_CACHE_MAX];
    bool pending, owed;
    uint32_t cpu_lo, cpu_hi;
    /* pixels written since the last barrier, rows [cover_y0, cover_y1] dirty */
    uint64_t *cover;
    int cover_y0, cover_y1;
    V2gStats stats;
    /* reuse of the last LOD and fog tables within a batch */
    uint32_t lodtab_key[2][36], lodtab_index[2];
    bool lodtab_valid[2];
    uint8_t fogtab_key[128];
    uint32_t fogtab_index;
    bool fogtab_valid;
};

static uint8_t tables[TABLE_BYTES];
static bool tables_ready;

const uint8_t *voodoo2_gpu_tables(unsigned *bytes)
{
    if (!tables_ready) {
        memcpy(tables + TABLE_RB, dither_rb, 4096);
        memcpy(tables + TABLE_G, dither_g, 4096);
        memcpy(tables + TABLE_RB2, dither_rb2x2, 1024);
        memcpy(tables + TABLE_G2, dither_g2x2, 1024);
        memcpy(tables + TABLE_SUB_RB, dithersub_rb, 4096);
        memcpy(tables + TABLE_SUB_G, dithersub_g, 4096);
        memcpy(tables + TABLE_SUB_RB2, dithersub_rb2x2, 1024);
        memcpy(tables + TABLE_SUB_G2, dithersub_g2x2, 1024);
        tables_ready = true;
    }
    if (bytes) *bytes = TABLE_BYTES;
    return tables;
}

/* ================= code generation ================= */

typedef struct {
    Gen8Inst *code;
    unsigned n;
    bool fail;
    uint8_t busy[128];
} G;

typedef int V;    /* a SIMD16 dword value: registers v and v + 1 */

static void emit(G *g, Gen8Inst in)
{
    if (g->n >= MAX_CODE) { g->fail = true; return; }
    g->code[g->n++] = in;
}

static bool value_register(int r)
{
    return (r >= VAL_FIRST && r <= VAL_LAST) || r == 108 || r == 110 || r == 122 || r == 124 || r == 126;
}

static V val(G *g)
{
    for (int r = VAL_FIRST; r < 128; r += 2)
        if (value_register(r) && !g->busy[r]) { g->busy[r] = 1; return r; }
    g->fail = true;
    return VAL_FIRST;
}

static void drop(G *g, V v) { if (v >= 0 && value_register(v)) g->busy[v] = 0; }

static const Gen8Ctl plain = { 0, 0, 0, 0, 0, 0 };

static Gen8Src D(V v) { return gen8_vec_t(v, GEN8_TYPE_D); }
static Gen8Src UD(V v) { return gen8_vec_t(v, GEN8_TYPE_UD); }
static Gen8Src F(V v) { return gen8_vec_t(v, GEN8_TYPE_F); }
/* the low words of a dword value: the 16-bit operand of a 32 x 16 multiply */
static Gen8Src W(V v) { return gen8_region(v, 0, GEN8_TYPE_W, 16, 8, 2); }
static Gen8Src K(int dw) { return gen8_scalar_t(REC_GRF + dw / 8, (dw % 8) * 4, GEN8_TYPE_D); }
static Gen8Src KF(int dw) { return gen8_scalar_t(REC_GRF + dw / 8, (dw % 8) * 4, GEN8_TYPE_F); }
static Gen8Src I(int32_t v) { return gen8_immd(v); }

static void op(G *g, unsigned opcode, V dst, unsigned type, Gen8Src a, Gen8Src b, Gen8Ctl ctl)
{
    emit(g, gen8_inst(opcode, 16, gen8_dst(dst, 0, type), a, b, ctl));
}

/* d = a OP b (dwords) */
static void opd(G *g, unsigned opcode, V dst, Gen8Src a, Gen8Src b) { op(g, opcode, dst, GEN8_TYPE_D, a, b, plain); }
static V newop(G *g, unsigned opcode, Gen8Src a, Gen8Src b) { V v = val(g); opd(g, opcode, v, a, b); return v; }

static V mov(G *g, Gen8Src a) { return newop(g, GEN8_OP_MOV, a, a); }
static V add(G *g, Gen8Src a, Gen8Src b) { return newop(g, GEN8_OP_ADD, a, b); }
static V sub(G *g, Gen8Src a, Gen8Src b)
{
    if (a.file == GEN8_IMM) return newop(g, GEN8_OP_ADD, gen8_neg(b), a);
    return newop(g, GEN8_OP_ADD, a, gen8_neg(b));
}
static V and_(G *g, Gen8Src a, Gen8Src b) { return newop(g, GEN8_OP_AND, a, b); }
static V or_(G *g, Gen8Src a, Gen8Src b) { return newop(g, GEN8_OP_OR, a, b); }
static V xor_(G *g, Gen8Src a, Gen8Src b) { return newop(g, GEN8_OP_XOR, a, b); }
static V asr(G *g, Gen8Src a, Gen8Src b) { return newop(g, GEN8_OP_ASR, a, b); }
static V shl(G *g, Gen8Src a, Gen8Src b) { return newop(g, GEN8_OP_SHL, a, b); }
static V shr(G *g, Gen8Src a, Gen8Src b)
{
    V v = val(g);
    op(g, GEN8_OP_SHR, v, GEN8_TYPE_UD, gen8_region(a.reg, a.subreg, GEN8_TYPE_UD, a.vstride, a.width, a.hstride), b,
       plain);
    return v;
}
/* a (dword) times b (a 16-bit operand: W() or IW()) */
static V mul(G *g, Gen8Src a, Gen8Src b) { return newop(g, GEN8_OP_MUL, a, b); }

static V sel(G *g, unsigned cond, Gen8Src a, Gen8Src b)
{
    V v = val(g);
    Gen8Ctl c = plain;
    c.cond = (uint8_t)cond;
    op(g, GEN8_OP_SEL, v, GEN8_TYPE_D, a, b, c);
    return v;
}
static V min_(G *g, Gen8Src a, Gen8Src b) { return sel(g, GEN8_COND_L, a, b); }
static V max_(G *g, Gen8Src a, Gen8Src b) { return sel(g, GEN8_COND_GE, a, b); }

/* mask: -1 where a cond b */
static V cmp(G *g, unsigned cond, Gen8Src a, Gen8Src b)
{
    V v = val(g);
    Gen8Ctl c = plain;
    c.cond = (uint8_t)cond;
    op(g, GEN8_OP_CMP, v, GEN8_TYPE_D, a, b, c);
    return v;
}

/* mask ? a : b (an immediate can only be the second source: swap or load it) */
static V pick(G *g, V mask, Gen8Src a, Gen8Src b)
{
    Gen8Ctl c = plain;
    c.cond = GEN8_COND_NZ;
    V loaded = -1;
    Gen8Ctl p = plain;
    p.predicate = 1;
    if (a.file == GEN8_IMM && b.file == GEN8_IMM) {
        loaded = val(g);
        opd(g, GEN8_OP_MOV, loaded, a, a);
        a = D(loaded);
    } else if (a.file == GEN8_IMM) {
        Gen8Src t = a;
        a = b;
        b = t;
        p.pred_inv = 1;
    }
    emit(g, gen8_inst(GEN8_OP_CMP, 16, gen8_dst_null(GEN8_TYPE_D), D(mask), I(0), c));
    V v = val(g);
    op(g, GEN8_OP_SEL, v, GEN8_TYPE_D, a, b, p);
    drop(g, loaded);
    return v;
}

/* replace *v by f(*v) and free the old value */
static void take(G *g, V *v, V next) { drop(g, *v); *v = next; }

static V clamp255(G *g, V v)
{
    V a = max_(g, D(v), I(0));
    V b = min_(g, D(a), I(255));
    drop(g, a);
    return b;
}

static V clamp16(G *g, V v)
{
    V a = max_(g, D(v), I(0));
    V b = min_(g, D(a), I(0xffff));
    drop(g, a);
    return b;
}

/* alive &= mask */
static void keep(G *g, V *alive, V mask)
{
    take(g, alive, and_(g, D(*alive), D(mask)));
    drop(g, mask);
}

/* x / 255 for 0 <= x <= 65025: (x + 1 + (x >> 8)) >> 8 */
static V div255(G *g, V x)
{
    V a = asr(g, D(x), I(8));
    V b = add(g, D(x), D(a));
    drop(g, a);
    V c = add(g, D(b), I(1));
    drop(g, b);
    V d = asr(g, D(c), I(8));
    drop(g, c);
    return d;
}

/* float helpers */
static V fop(G *g, unsigned opcode, Gen8Src a, Gen8Src b)
{
    V v = val(g);
    op(g, opcode, v, GEN8_TYPE_F, a, b, plain);
    return v;
}
static V fmov(G *g, Gen8Src a) { return fop(g, GEN8_OP_MOV, a, a); }
static V math(G *g, unsigned function, V a)
{
    V v = val(g);
    Gen8Ctl c = plain;
    c.cond = (uint8_t)function;
    op(g, GEN8_OP_MATH, v, GEN8_TYPE_F, F(a), gen8_null_src(GEN8_TYPE_F), c);
    return v;
}
/* plane: k0 + k1 * dx + k2 * dy (floats in the record) */
static V plane(G *g, int k, V dxf, V dyf)
{
    V a = fop(g, GEN8_OP_MUL, F(dxf), KF(k + 1));
    V b = fop(g, GEN8_OP_ADD, F(a), KF(k));
    drop(g, a);
    V c = fop(g, GEN8_OP_MUL, F(dyf), KF(k + 2));
    V d = fop(g, GEN8_OP_ADD, F(b), F(c));
    drop(g, b);
    drop(g, c);
    return d;
}
/* float to dword (rounds toward zero) */
static V ftoi(G *g, V f)
{
    V v = val(g);
    op(g, GEN8_OP_MOV, v, GEN8_TYPE_D, F(f), F(f), plain);
    return v;
}

/* ld from a buffer surface: R (and, for four, G, B, A) of element index */
static void ld(G *g, unsigned bt, V index, V *out, unsigned count)
{
    opd(g, GEN8_OP_MOV, LD_MSG, D(index), D(index));
    emit(g, gen8_send(GEN8_SFID_SAMPLER, LD_ANSWER, LD_MSG, GEN8_DESC_LD16(bt), 0));
    for (unsigned c = 0; c < count; c++) out[c] = mov(g, D(LD_ANSWER + 2 * c));
}

static V ld1(G *g, unsigned bt, V index)
{
    V v;
    ld(g, bt, index, &v, 1);
    return v;
}

/* 86Box: dest_r = (dat >> 8) & 0xf8; dest_r |= dest_r >> 5 (and g, b likewise) */
static V expand(G *g, V dat, int shift_right, int mask, int fill_shift)
{
    V a = shift_right > 0 ? asr(g, D(dat), I(shift_right)) : shl(g, D(dat), I(-shift_right));
    V b = and_(g, D(a), I(mask));
    drop(g, a);
    V c = asr(g, D(b), I(fill_shift));
    V d = or_(g, D(b), D(c));
    drop(g, b);
    drop(g, c);
    return d;
}

typedef struct {
    const voodoo_t *voodoo;
    const voodoo_params_t *params;
    G *g;
    V x, row, dxf, dyf, pos4, pos2;     /* pos4: (row & 3) * 4 + (x & 3), pos2 likewise for 2x2 */
} Ctx;

typedef struct { V r, g, b, a, lod, lod_frac; } Tex;

/* wrap or clamp a texel coordinate to [0, mask] (86Box tex_read / tex_read_4) */
static V texel_coordinate(G *g, V v, V mask, bool clamp)
{
    if (!clamp) return and_(g, D(v), D(mask));
    V a = max_(g, D(v), I(0));
    V b = min_(g, D(a), D(mask));
    drop(g, a);
    return b;
}

static void unpack(G *g, V dat, V out[4])
{
    out[0] = and_(g, D(dat), I(0xff));                 /* b */
    V t = shr(g, D(dat), I(8));
    out[1] = and_(g, D(t), I(0xff));                   /* g */
    drop(g, t);
    t = shr(g, D(dat), I(16));
    out[2] = and_(g, D(t), I(0xff));                   /* r */
    drop(g, t);
    out[3] = shr(g, D(dat), I(24));                    /* a */
}

/* voodoo_tmu_fetch() + voodoo_get_texture(): the texel colour of one unit */
static Tex tmu_fetch(Ctx *c, int tmu)
{
    G *g = c->g;
    const voodoo_params_t *params = c->params;
    const voodoo_t *voodoo = c->voodoo;
    const int k = R_TMU + tmu * 9;
    Tex out;
    V s = plane(g, k, c->dxf, c->dyf), t = plane(g, k + 3, c->dxf, c->dyf);
    V tex_s, tex_t, lod;
    if (params->textureMode[tmu] & TEXTUREMODE_TPERSP_ST) {
        V w = plane(g, k + 6, c->dxf, c->dyf);
        /* _w = (1 << 48) / W (W with 32 fraction bits): 2^16 / w here;
         * tex_s = (((s + (1 << 13)) >> 14) * _w + (1 << 29)) >> 30: round(16 * s / w) */
        V inv = math(g, GEN8_MATH_INV, w);
        V ts = fop(g, GEN8_OP_MUL, F(s), F(inv)), tt = fop(g, GEN8_OP_MUL, F(t), F(inv));
        V a = fop(g, GEN8_OP_MUL, F(ts), gen8_immf(16.0f));
        V b = fop(g, GEN8_OP_ADD, F(a), gen8_immf(0.5f));
        take(g, &b, fop(g, GEN8_OP_RNDD, F(b), F(b)));
        drop(g, a);
        V ss = ftoi(g, b);
        drop(g, b);
        a = fop(g, GEN8_OP_MUL, F(tt), gen8_immf(16.0f));
        b = fop(g, GEN8_OP_ADD, F(a), gen8_immf(0.5f));
        take(g, &b, fop(g, GEN8_OP_RNDD, F(b), F(b)));
        drop(g, a);
        V st = ftoi(g, b);
        drop(g, b);
        /* W <= 0: _w is 0 or negative, the texel 0 and fastlog() 0x80000000: lod_max */
        V bad = val(g);
        {
            Gen8Ctl cc = plain;
            cc.cond = GEN8_COND_LE;
            op(g, GEN8_OP_CMP, bad, GEN8_TYPE_F, F(w), gen8_immf(0.0f), cc);
        }
        V zero = val(g);
        {
            Gen8Ctl cc = plain;
            cc.cond = GEN8_COND_Z;
            op(g, GEN8_OP_CMP, zero, GEN8_TYPE_F, F(w), gen8_immf(0.0f), cc);
        }
        tex_s = pick(g, zero, I(0), D(ss));
        tex_t = pick(g, zero, I(0), D(st));
        drop(g, ss);
        drop(g, st);
        drop(g, zero);
        /* the base LOD: voodoo_span_part_lod_base() at this pixel */
        V base;
        {
            V dsdx = fop(g, GEN8_OP_MUL, F(ts), KF(k + 7)), dtdx = fop(g, GEN8_OP_MUL, F(tt), KF(k + 7));
            take(g, &dsdx, fop(g, GEN8_OP_ADD, KF(k + 1), gen8_neg(F(dsdx))));
            take(g, &dtdx, fop(g, GEN8_OP_ADD, KF(k + 4), gen8_neg(F(dtdx))));
            V dsdy = fop(g, GEN8_OP_MUL, F(ts), KF(k + 8)), dtdy = fop(g, GEN8_OP_MUL, F(tt), KF(k + 8));
            take(g, &dsdy, fop(g, GEN8_OP_ADD, KF(k + 2), gen8_neg(F(dsdy))));
            take(g, &dtdy, fop(g, GEN8_OP_ADD, KF(k + 5), gen8_neg(F(dtdy))));
            V gx = fop(g, GEN8_OP_MUL, F(dsdx), F(dsdx));
            V q = fop(g, GEN8_OP_MUL, F(dtdx), F(dtdx));
            take(g, &gx, fop(g, GEN8_OP_ADD, F(gx), F(q)));
            drop(g, q);
            V gy = fop(g, GEN8_OP_MUL, F(dsdy), F(dsdy));
            q = fop(g, GEN8_OP_MUL, F(dtdy), F(dtdy));
            take(g, &gy, fop(g, GEN8_OP_ADD, F(gy), F(q)));
            drop(g, q);
            drop(g, dsdx); drop(g, dtdx); drop(g, dsdy); drop(g, dtdy);
            V gm = val(g);
            {
                Gen8Ctl cc = plain;
                cc.cond = GEN8_COND_GE;
                op(g, GEN8_OP_SEL, gm, GEN8_TYPE_F, F(gx), F(gy), cc);
            }
            drop(g, gx);
            drop(g, gy);
            V positive = val(g);
            {
                Gen8Ctl cc = plain;
                cc.cond = GEN8_COND_G;
                op(g, GEN8_OP_CMP, positive, GEN8_TYPE_F, F(gm), gen8_immf(0.0f), cc);
            }
            V lg = math(g, GEN8_MATH_LOG, gm);
            drop(g, gm);
            /* (int)(log2(g) / 2 * 256) */
            take(g, &lg, fop(g, GEN8_OP_MUL, F(lg), gen8_immf(128.0f)));
            take(g, &lg, fop(g, GEN8_OP_RNDZ, F(lg), F(lg)));
            V li = ftoi(g, lg);
            drop(g, lg);
            base = pick(g, positive, D(li), I(-(64 << 8)));
            drop(g, li);
            drop(g, positive);
            /* span_middle_w <= 0: the triangle's base */
            take(g, &base, pick(g, bad, K(R_LOD + tmu), D(base)));
        }
        /* + fastlog(_w) - (16 << 8): floor(256 * log2(65536 / w)) - 4096 */
        a = fop(g, GEN8_OP_MUL, F(inv), gen8_immf(65536.0f));
        take(g, &a, math(g, GEN8_MATH_LOG, a));
        take(g, &a, fop(g, GEN8_OP_MUL, F(a), gen8_immf(256.0f)));
        take(g, &a, fop(g, GEN8_OP_RNDD, F(a), F(a)));
        V wl = ftoi(g, a);
        drop(g, a);
        lod = add(g, D(base), D(wl));
        drop(g, base);
        drop(g, wl);
        take(g, &lod, add(g, D(lod), K(R_LODBIAS + tmu)));
        take(g, &lod, add(g, D(lod), I(-(16 << 8))));
        take(g, &lod, pick(g, bad, K(R_LODMAX + tmu), D(lod)));
        drop(g, bad);
        drop(g, inv);
        drop(g, ts);
        drop(g, tt);
        drop(g, w);
    } else {
        /* tex_s = (int32_t)(tmu_s >> (14 + 14)): floor(16 * s) */
        V a = fop(g, GEN8_OP_MUL, F(s), gen8_immf(16.0f));
        take(g, &a, fop(g, GEN8_OP_RNDD, F(a), F(a)));
        tex_s = ftoi(g, a);
        drop(g, a);
        a = fop(g, GEN8_OP_MUL, F(t), gen8_immf(16.0f));
        take(g, &a, fop(g, GEN8_OP_RNDD, F(a), F(a)));
        tex_t = ftoi(g, a);
        drop(g, a);
        lod = add(g, K(R_LOD + tmu), K(R_LODBIAS + tmu));
    }
    drop(g, s);
    drop(g, t);
    /* clamp, lod_frac, >> 8 */
    take(g, &lod, max_(g, D(lod), K(R_LODMIN + tmu)));
    take(g, &lod, min_(g, D(lod), K(R_LODMAX + tmu)));
    out.lod_frac = and_(g, D(lod), I(0xff));
    take(g, &lod, asr(g, D(lod), I(8)));
    out.lod = lod;
    /* the LOD's {w_mask, h_mask, tex_lod, texel index} */
    V entry_index = add(g, D(lod), K(R_LODTAB + tmu));
    V e[4];
    ld(g, V2G_BT_DATA4, entry_index, e, 4);
    drop(g, entry_index);
    V w_mask = e[0], h_mask = e[1], tex_lod = e[2], texel = e[3];
    V shift = add(g, gen8_neg(D(tex_lod)), I(8));
    if (params->tLOD[tmu] & LOD_TMIRROR_S) {
        V bit = and_(g, D(tex_s), I(0x1000));
        V inverted = xor_(g, D(tex_s), I(-1));
        take(g, &tex_s, pick(g, bit, D(inverted), D(tex_s)));
        drop(g, bit);
        drop(g, inverted);
    }
    if (params->tLOD[tmu] & LOD_TMIRROR_T) {
        V bit = and_(g, D(tex_t), I(0x1000));
        V inverted = xor_(g, D(tex_t), I(-1));
        take(g, &tex_t, pick(g, bit, D(inverted), D(tex_t)));
        drop(g, bit);
        drop(g, inverted);
    }
    const bool clamp_s = params->textureMode[tmu] & TEXTUREMODE_TCLAMPS;
    const bool clamp_t = params->textureMode[tmu] & TEXTUREMODE_TCLAMPT;
    if (voodoo->bilinear_enabled && (params->textureMode[tmu] & 6)) {
        /* tex_s -= 1 << (3 + tex_lod); s = tex_s >> tex_lod; _ds = s & 0xf; s >>= 4 */
        V one = mov(g, I(8));
        V half = shl(g, D(one), D(tex_lod));
        drop(g, one);
        take(g, &tex_s, sub(g, D(tex_s), D(half)));
        take(g, &tex_t, sub(g, D(tex_t), D(half)));
        drop(g, half);
        V si = asr(g, D(tex_s), D(tex_lod)), ti = asr(g, D(tex_t), D(tex_lod));
        V ds = and_(g, D(si), I(0xf)), dt = and_(g, D(ti), I(0xf));
        take(g, &si, asr(g, D(si), I(4)));
        take(g, &ti, asr(g, D(ti), I(4)));
        V ids = add(g, gen8_neg(D(ds)), I(16)), idt = add(g, gen8_neg(D(dt)), I(16));
        V d[4] = { mul(g, D(ids), W(idt)), mul(g, D(ds), W(idt)), mul(g, D(ids), W(dt)), mul(g, D(ds), W(dt)) };
        drop(g, ds); drop(g, dt); drop(g, ids); drop(g, idt);
        V s1 = add(g, D(si), I(1)), t1 = add(g, D(ti), I(1));
        V cs[2] = { texel_coordinate(g, si, w_mask, clamp_s), texel_coordinate(g, s1, w_mask, clamp_s) };
        V ct[2] = { texel_coordinate(g, ti, h_mask, clamp_t), texel_coordinate(g, t1, h_mask, clamp_t) };
        drop(g, si); drop(g, ti); drop(g, s1); drop(g, t1);
        V acc[4] = { -1, -1, -1, -1 };
        for (int i = 0; i < 4; i++) {
            V row = shl(g, D(ct[i >> 1]), D(shift));
            V index = add(g, D(row), D(cs[i & 1]));
            drop(g, row);
            take(g, &index, add(g, D(index), D(texel)));
            V dat = ld1(g, V2G_BT_TEX, index);
            drop(g, index);
            V ch[4];
            unpack(g, dat, ch);
            drop(g, dat);
            for (int k2 = 0; k2 < 4; k2++) {
                V p = mul(g, D(ch[k2]), W(d[i]));
                drop(g, ch[k2]);
                if (acc[k2] < 0) acc[k2] = p;
                else { take(g, &acc[k2], add(g, D(acc[k2]), D(p))); drop(g, p); }
            }
        }
        for (int i = 0; i < 4; i++) drop(g, d[i]);
        drop(g, cs[0]); drop(g, cs[1]); drop(g, ct[0]); drop(g, ct[1]);
        out.b = asr(g, D(acc[0]), I(8));
        out.g = asr(g, D(acc[1]), I(8));
        out.r = asr(g, D(acc[2]), I(8));
        out.a = asr(g, D(acc[3]), I(8));
        for (int i = 0; i < 4; i++) drop(g, acc[i]);
    } else {
        /* s = tex_s >> (4 + tex_lod) */
        V sh = add(g, D(tex_lod), I(4));
        V si = asr(g, D(tex_s), D(sh)), ti = asr(g, D(tex_t), D(sh));
        drop(g, sh);
        take(g, &si, texel_coordinate(g, si, w_mask, clamp_s));
        take(g, &ti, texel_coordinate(g, ti, h_mask, clamp_t));
        V row = shl(g, D(ti), D(shift));
        V index = add(g, D(row), D(si));
        drop(g, row);
        drop(g, si);
        drop(g, ti);
        take(g, &index, add(g, D(index), D(texel)));
        V dat = ld1(g, V2G_BT_TEX, index);
        drop(g, index);
        V ch[4];
        unpack(g, dat, ch);
        drop(g, dat);
        out.b = ch[0]; out.g = ch[1]; out.r = ch[2]; out.a = ch[3];
    }
    drop(g, tex_s);
    drop(g, tex_t);
    drop(g, shift);
    drop(g, w_mask);
    drop(g, h_mask);
    drop(g, tex_lod);
    drop(g, texel);
    return out;
}

static void tex_drop(G *g, Tex *t)
{
    drop(g, t->r); drop(g, t->g); drop(g, t->b); drop(g, t->a); drop(g, t->lod); drop(g, t->lod_frac);
}

/* (detail_bias - lod) << detail_scale, at most detail_max */
static V detail_factor(G *g, int tmu, V lod)
{
    V a = sub(g, K(R_DETAILBIAS + tmu), D(lod));
    V b = shl(g, D(a), K(R_DETAILSCALE + tmu));
    drop(g, a);
    V c = min_(g, D(b), K(R_DETAILMAX + tmu));
    drop(g, b);
    return c;
}

/* (v * ((reverse ? factor ^ 0xff : factor) + 1)) >> 8, reverse a mask */
static V blend_factor_mul(G *g, V v, V factor, V reverse)
{
    V inverted = xor_(g, D(factor), I(0xff));
    V f = pick(g, reverse, D(inverted), D(factor));
    drop(g, inverted);
    take(g, &f, add(g, D(f), I(1)));
    V p = mul(g, D(v), W(f));
    drop(g, f);
    V r = asr(g, D(p), I(8));
    drop(g, p);
    return r;
}

/* c_reverse: (TRILINEAR && (lod & 1)) ? reverse_bit : !reverse_bit, as a mask */
static V reverse_mask(G *g, bool trilinear, bool reverse_bit, V lod)
{
    if (!trilinear) return mov(g, I(reverse_bit ? 0 : -1));
    V odd = and_(g, D(lod), I(1));
    V m = pick(g, odd, I(reverse_bit ? -1 : 0), I(reverse_bit ? 0 : -1));
    drop(g, odd);
    return m;
}

/* voodoo_tmu_fetch_and_blend() */
static Tex fetch_and_blend(Ctx *c)
{
    G *g = c->g;
    const voodoo_params_t *params = c->params;
    Tex t1 = tmu_fetch(c, 1);
    /* TMU1 with its own bits */
    const bool tri1 = params->textureMode[1] & TEXTUREMODE_TRILINEAR;
    if (tc_sub_clocal_1) {
        V factor[3];
        switch (tc_mselect_1) {
        case TC_MSELECT_CLOCAL: factor[0] = mov(g, D(t1.r)); factor[1] = mov(g, D(t1.g)); factor[2] = mov(g, D(t1.b)); break;
        case TC_MSELECT_ALOCAL: factor[0] = mov(g, D(t1.a)); factor[1] = mov(g, D(t1.a)); factor[2] = mov(g, D(t1.a)); break;
        case TC_MSELECT_DETAIL: factor[0] = detail_factor(g, 1, t1.lod); factor[1] = mov(g, D(factor[0])); factor[2] = mov(g, D(factor[0])); break;
        case TC_MSELECT_LOD_FRAC: factor[0] = mov(g, D(t1.lod_frac)); factor[1] = mov(g, D(t1.lod_frac)); factor[2] = mov(g, D(t1.lod_frac)); break;
        default: factor[0] = mov(g, I(0)); factor[1] = mov(g, I(0)); factor[2] = mov(g, I(0)); break;
        }
        V rev = reverse_mask(g, tri1, tc_reverse_blend_1, t1.lod);
        V *ch[3] = { &t1.r, &t1.g, &t1.b };
        V result[3];
        for (int i = 0; i < 3; i++) {
            /* r = (-tex_r[1] * (factor + 1)) >> 8 */
            V neg = sub(g, I(0), D(*ch[i]));
            V r = blend_factor_mul(g, neg, factor[i], rev);
            drop(g, neg);
            if (tc_add_clocal_1) take(g, &r, add(g, D(r), D(*ch[i])));
            else if (tc_add_alocal_1) take(g, &r, add(g, D(r), D(t1.a)));
            result[i] = clamp255(g, r);
            drop(g, r);
            drop(g, factor[i]);
        }
        drop(g, rev);
        for (int i = 0; i < 3; i++) take(g, ch[i], result[i]);
    }
    if (tca_sub_clocal_1) {
        V factor;
        switch (tca_mselect_1) {
        case TCA_MSELECT_CLOCAL: case TCA_MSELECT_ALOCAL: factor = mov(g, D(t1.a)); break;
        case TCA_MSELECT_DETAIL: factor = detail_factor(g, 1, t1.lod); break;
        case TCA_MSELECT_LOD_FRAC: factor = mov(g, D(t1.lod_frac)); break;
        default: factor = mov(g, I(0)); break;
        }
        V rev = reverse_mask(g, tri1, tca_reverse_blend_1, t1.lod);
        V neg = sub(g, I(0), D(t1.a));
        V a = blend_factor_mul(g, neg, factor, rev);
        drop(g, neg);
        drop(g, factor);
        drop(g, rev);
        if (tca_add_clocal_1 || tca_add_alocal_1) take(g, &a, add(g, D(a), D(t1.a)));
        take(g, &t1.a, clamp255(g, a));
        drop(g, a);
    }
    Tex t0 = tmu_fetch(c, 0);
    const bool tri0 = params->textureMode[0] & TEXTUREMODE_TRILINEAR;
    V crev = reverse_mask(g, tri0, tc_reverse_blend, t0.lod);
    V arev = reverse_mask(g, tri0, tca_reverse_blend, t0.lod);
    V col[3];
    V *other[3] = { &t1.r, &t1.g, &t1.b }, *local[3] = { &t0.r, &t0.g, &t0.b };
    V factor[3];
    switch (tc_mselect) {
    case TC_MSELECT_CLOCAL: for (int i = 0; i < 3; i++) factor[i] = mov(g, D(*local[i])); break;
    case TC_MSELECT_AOTHER: for (int i = 0; i < 3; i++) factor[i] = mov(g, D(t1.a)); break;
    case TC_MSELECT_ALOCAL: for (int i = 0; i < 3; i++) factor[i] = mov(g, D(t0.a)); break;
    case TC_MSELECT_DETAIL: factor[0] = detail_factor(g, 0, t0.lod); factor[1] = mov(g, D(factor[0])); factor[2] = mov(g, D(factor[0])); break;
    case TC_MSELECT_LOD_FRAC: for (int i = 0; i < 3; i++) factor[i] = mov(g, D(t0.lod_frac)); break;
    default: for (int i = 0; i < 3; i++) factor[i] = mov(g, I(0)); break;
    }
    for (int i = 0; i < 3; i++) {
        V v = tc_zero_other ? mov(g, I(0)) : mov(g, D(*other[i]));
        if (tc_sub_clocal) take(g, &v, sub(g, D(v), D(*local[i])));
        V r = blend_factor_mul(g, v, factor[i], crev);
        drop(g, v);
        drop(g, factor[i]);
        if (tc_add_clocal) take(g, &r, add(g, D(r), D(*local[i])));
        else if (tc_add_alocal) take(g, &r, add(g, D(r), D(t0.a)));
        col[i] = r;
    }
    V fa;
    switch (tca_mselect) {
    case TCA_MSELECT_CLOCAL: case TCA_MSELECT_ALOCAL: fa = mov(g, D(t0.a)); break;
    case TCA_MSELECT_AOTHER: fa = mov(g, D(t1.a)); break;
    case TCA_MSELECT_DETAIL: fa = detail_factor(g, 0, t0.lod); break;
    case TCA_MSELECT_LOD_FRAC: fa = mov(g, D(t0.lod_frac)); break;
    default: fa = mov(g, I(0)); break;
    }
    V a = tca_zero_other ? mov(g, I(0)) : mov(g, D(t1.a));
    if (tca_sub_clocal) take(g, &a, sub(g, D(a), D(t0.a)));
    V ar = blend_factor_mul(g, a, fa, arev);
    drop(g, a);
    drop(g, fa);
    if (tca_add_clocal || tca_add_alocal) take(g, &ar, add(g, D(ar), D(t0.a)));
    drop(g, crev);
    drop(g, arev);
    Tex out;
    out.r = clamp255(g, col[0]);
    out.g = clamp255(g, col[1]);
    out.b = clamp255(g, col[2]);
    out.a = clamp255(g, ar);
    drop(g, col[0]); drop(g, col[1]); drop(g, col[2]); drop(g, ar);
    if (tc_invert_output) {
        take(g, &out.r, xor_(g, D(out.r), I(0xff)));
        take(g, &out.g, xor_(g, D(out.g), I(0xff)));
        take(g, &out.b, xor_(g, D(out.b), I(0xff)));
    }
    if (tca_invert_output) take(g, &out.a, xor_(g, D(out.a), I(0xff)));
    out.lod = t0.lod;
    out.lod_frac = t0.lod_frac;
    t0.lod = t0.lod_frac = -1;
    tex_drop(g, &t1);
    drop(g, t0.r); drop(g, t0.g); drop(g, t0.b); drop(g, t0.a);
    return out;
}

/* integer parameter p (0 r, 1 g, 2 b, 3 a, 4 z): start + dX * dx + dY * dy */
static V iterated(Ctx *c, int p, V dx, V dy)
{
    G *g = c->g;
    int k = R_PLANE + p * 3;
    V a = mul(g, K(k + 1), W(dx));
    V b = add(g, D(a), K(k));
    drop(g, a);
    V d = mul(g, K(k + 2), W(dy));
    V e = add(g, D(b), D(d));
    drop(g, b);
    drop(g, d);
    return e;
}

/* CLAMP(v >> shift) */
static V clamp_shift(G *g, V v, int shift)
{
    V a = asr(g, D(v), I(shift));
    V b = clamp255(g, a);
    drop(g, a);
    return b;
}

static V table_lookup(Ctx *c, int table, V value, bool two)
{
    G *g = c->g;
    V a = shl(g, D(value), I(two ? 2 : 4));
    if (c->pos4 < 0) {
        V xa = and_(g, D(c->x), I(3)), ya = and_(g, D(c->row), I(3));
        V t = shl(g, D(ya), I(2));
        c->pos4 = add(g, D(t), D(xa));
        drop(g, t); drop(g, xa); drop(g, ya);
        xa = and_(g, D(c->x), I(1));
        ya = and_(g, D(c->row), I(1));
        t = shl(g, D(ya), I(1));
        c->pos2 = add(g, D(t), D(xa));
        drop(g, t); drop(g, xa); drop(g, ya);
    }
    V b = add(g, D(a), D(two ? c->pos2 : c->pos4));
    drop(g, a);
    take(g, &b, add(g, D(b), I(table)));
    V r = ld1(g, V2G_BT_TABLES, b);
    drop(g, b);
    return r;
}

static V compare_mask(G *g, int func, V a, Gen8Src b)
{
    static const uint8_t cond[8] = { 0, GEN8_COND_L, GEN8_COND_Z, GEN8_COND_LE, GEN8_COND_G, GEN8_COND_NZ,
                                     GEN8_COND_GE, 0 };
    if (func == 0) return mov(g, I(0));
    if (func == 7) return mov(g, I(-1));
    return cmp(g, cond[func], D(a), b);
}

static bool generate(const voodoo_t *voodoo, const voodoo_params_t *params, G *g)
{
    Ctx ctx = { voodoo, params, g, 0, 0, 0, 0, 0, 0 };
    Ctx *c = &ctx;
    for (int r = 0; r < 128; r++) g->busy[r] = 0;
    /* lod 0 of every ld */
    opd(g, GEN8_OP_MOV, LD_MSG + 2, I(0), I(0));
    /* pixel coordinates: g1.2-g1.5 hold the subspans' X (low word) and Y */
    V xw = val(g), yw = val(g);
    op(g, GEN8_OP_ADD, xw, GEN8_TYPE_UW, gen8_region(1, 8, GEN8_TYPE_UW, 2, 4, 0), gen8_immv(0x10101010), plain);
    op(g, GEN8_OP_ADD, yw, GEN8_TYPE_UW, gen8_region(1, 10, GEN8_TYPE_UW, 2, 4, 0), gen8_immv(0x11001100), plain);
    c->x = mov(g, gen8_vec_t(xw, GEN8_TYPE_UW));
    c->row = mov(g, gen8_vec_t(yw, GEN8_TYPE_UW));
    drop(g, xw);
    drop(g, yw);
    /* the line: real_y = y_origin - y with the origin at the bottom */
    V y = (params->fbzMode & (1 << 17)) ? sub(g, K(R_YFLIP), D(c->row)) : mov(g, D(c->row));
    V dx = sub(g, D(c->x), K(R_XA)), dy = sub(g, D(y), K(R_YREF));
    c->dxf = fmov(g, D(dx));
    c->dyf = fmov(g, D(dy));
    /* the span */
    V alive = cmp(g, GEN8_COND_GE, D(y), K(R_YSTART));
    keep(g, &alive, cmp(g, GEN8_COND_L, D(y), K(R_YEND)));
    V line = sub(g, D(y), K(R_YSTART));
    take(g, &line, add(g, D(line), K(R_SPAN)));
    V span = ld1(g, V2G_BT_DATA, line);
    drop(g, line);
    V lo = and_(g, D(span), I(0xffff)), hi = shr(g, D(span), I(16));
    drop(g, span);
    keep(g, &alive, cmp(g, GEN8_COND_GE, D(c->x), D(lo)));
    keep(g, &alive, cmp(g, GEN8_COND_LE, D(c->x), D(hi)));
    drop(g, lo);
    drop(g, hi);
    drop(g, y);
    c->pos4 = c->pos2 = -1;
    /* frame-buffer elements of this pixel */
    V rowel = mul(g, K(R_ROWEL), W(c->row));
    take(g, &rowel, add(g, D(rowel), D(c->x)));

    const uint32_t fbz = params->fbzMode;
    const bool need_w_depth = (fbz & FBZ_W_BUFFER) ||
        ((params->fogMode & FOG_ENABLE) && !(params->fogMode & FOG_CONSTANT) && !(params->fogMode & (FOG_Z | FOG_ALPHA)));
    const bool need_w = need_w_depth ||
        ((params->fogMode & FOG_ENABLE) && !(params->fogMode & FOG_CONSTANT) && (params->fogMode & (FOG_Z | FOG_ALPHA)) == FOG_W);
    V wf = -1, w_depth = -1;
    if (need_w) wf = plane(g, R_W, c->dxf, c->dyf);
    if (need_w_depth) {
        /* if (w & 0xffff00000000) 0; else if (!(w & 0xffff0000)) 0xf001; else
         * exp = fls(w >> 16); mant = (~w >> (19 - exp)) & 0xfff; min((exp << 12) + mant + 1, 0xffff) */
        V in_range = val(g);
        {
            Gen8Ctl cc = plain;
            cc.cond = GEN8_COND_GE;
            op(g, GEN8_OP_CMP, in_range, GEN8_TYPE_F, F(wf), gen8_immf(0.0f), cc);
            V below = val(g);
            cc.cond = GEN8_COND_L;
            op(g, GEN8_OP_CMP, below, GEN8_TYPE_F, F(wf), gen8_immf(1.0f), cc);
            take(g, &in_range, and_(g, D(in_range), D(below)));
            drop(g, below);
        }
        V scaled = fop(g, GEN8_OP_MUL, F(wf), gen8_immf(4294967296.0f));
        V wu = val(g);
        op(g, GEN8_OP_MOV, wu, GEN8_TYPE_UD, F(scaled), F(scaled), plain);
        drop(g, scaled);
        V high = shr(g, UD(wu), I(16));
        V lz = val(g);
        op(g, GEN8_OP_LZD, lz, GEN8_TYPE_UD, UD(high), UD(high), plain);
        V exp = add(g, D(lz), I(-16));
        drop(g, lz);
        V sh = add(g, gen8_neg(D(exp)), I(19));
        V inv = xor_(g, D(wu), I(-1));
        V m = shr(g, UD(inv), UD(sh));
        drop(g, inv);
        drop(g, sh);
        take(g, &m, and_(g, D(m), I(0xfff)));
        V e = shl(g, D(exp), I(12));
        drop(g, exp);
        V wd = add(g, D(e), D(m));
        drop(g, e);
        drop(g, m);
        take(g, &wd, add(g, D(wd), I(1)));
        take(g, &wd, min_(g, D(wd), I(0xffff)));
        V zero_high = cmp(g, GEN8_COND_Z, D(high), I(0));
        drop(g, high);
        drop(g, wu);
        take(g, &wd, pick(g, zero_high, I(0xf001), D(wd)));
        drop(g, zero_high);
        w_depth = pick(g, in_range, D(wd), I(0));
        drop(g, wd);
        drop(g, in_range);
    }
    V z = iterated(c, 4, dx, dy);
    V new_depth;
    if (fbz & FBZ_W_BUFFER) new_depth = mov(g, D(w_depth));
    else {
        V a = asr(g, D(z), I(12));
        new_depth = clamp16(g, a);
        drop(g, a);
    }
    if (fbz & FBZ_DEPTH_BIAS) {
        take(g, &new_depth, add(g, D(new_depth), K(R_ZBIAS)));
        take(g, &new_depth, clamp16(g, new_depth));
    }
    if (fbz & FBZ_DEPTH_ENABLE) {
        V index = add(g, D(rowel), K(R_AUXROW));
        V old_depth = ld1(g, V2G_BT_FB, index);
        drop(g, index);
        int depth_op_ = (fbz >> 5) & 7;
        V pass;
        if (fbz & FBZ_DEPTH_SOURCE) {
            V source = and_(g, K(R_ZACOLOR), I(0xffff));
            pass = compare_mask(g, depth_op_, source, D(old_depth));
            drop(g, source);
        } else {
            pass = compare_mask(g, depth_op_, new_depth, D(old_depth));
        }
        drop(g, old_depth);
        keep(g, &alive, pass);
    }
    const bool blend = params->alphaMode & (1 << 4);
    V dest_r = -1, dest_g = -1, dest_b = -1, dest_a = -1;

    /* texture */
    Tex tex = { -1, -1, -1, -1, -1, -1 };
    const bool textured = params->fbzColorPath & FBZCP_TEXTURE_ENABLED;
    if (textured) {
        if ((params->textureMode[0] & TEXTUREMODE_LOCAL_MASK) == TEXTUREMODE_LOCAL || !voodoo->dual_tmus) {
            tex = tmu_fetch(c, 0);
        } else if ((params->textureMode[0] & TEXTUREMODE_MASK) == TEXTUREMODE_PASSTHROUGH) {
            tex = tmu_fetch(c, 1);
        } else {
            tex = fetch_and_blend(c);
        }
    }
    if (!textured) {
        /* state->tex_x[0] keeps whatever the last textured pixel left: 0 here */
        tex.r = mov(g, I(0));
        tex.g = mov(g, I(0));
        tex.b = mov(g, I(0));
        tex.a = mov(g, I(0));
    }
    if (voodoo->trexInit1[0] & (1 << 18)) {
        /* tex_r = tex_g = 0, tex_b = tmuConfig; tex_a stays */
        drop(g, tex.r); drop(g, tex.g); drop(g, tex.b);
        tex.r = mov(g, I(0));
        tex.g = mov(g, I(0));
        tex.b = mov(g, K(R_TMUCONFIG));
    }
    drop(g, tex.lod);
    drop(g, tex.lod_frac);
    tex.lod = tex.lod_frac = -1;

    /* iterated colours */
    V ir = iterated(c, 0, dx, dy), ig = iterated(c, 1, dx, dy), ib = iterated(c, 2, dx, dy), ia = iterated(c, 3, dx, dy);
    drop(g, dx);
    drop(g, dy);
    V iter[3] = { clamp_shift(g, ir, 12), clamp_shift(g, ig, 12), clamp_shift(g, ib, 12) };
    drop(g, ir); drop(g, ig); drop(g, ib);
    V iter_a = clamp_shift(g, ia, 12);

    /* clocal */
    V clocal[3];
    {
        V c0[3];
        V col0r = shr(g, K(R_COLOR0), I(16));
        c0[0] = and_(g, D(col0r), I(0xff));
        drop(g, col0r);
        V col0g = shr(g, K(R_COLOR0), I(8));
        c0[1] = and_(g, D(col0g), I(0xff));
        drop(g, col0g);
        c0[2] = and_(g, K(R_COLOR0), I(0xff));
        if (cc_localselect_override) {
            V bit = and_(g, D(tex.a), I(0x80));
            for (int i = 0; i < 3; i++) clocal[i] = pick(g, bit, D(c0[i]), D(iter[i]));
            drop(g, bit);
        } else {
            for (int i = 0; i < 3; i++) clocal[i] = cc_localselect ? mov(g, D(c0[i])) : mov(g, D(iter[i]));
        }
        for (int i = 0; i < 3; i++) drop(g, c0[i]);
    }
    /* cother */
    V cother[3];
    switch (_rgb_sel) {
    case CC_LOCALSELECT_ITER_RGB: for (int i = 0; i < 3; i++) cother[i] = mov(g, D(iter[i])); break;
    case CC_LOCALSELECT_TEX: cother[0] = mov(g, D(tex.r)); cother[1] = mov(g, D(tex.g)); cother[2] = mov(g, D(tex.b)); break;
    case CC_LOCALSELECT_COLOR1: {
        V t = shr(g, K(R_COLOR1), I(16));
        cother[0] = and_(g, D(t), I(0xff));
        drop(g, t);
        t = shr(g, K(R_COLOR1), I(8));
        cother[1] = and_(g, D(t), I(0xff));
        drop(g, t);
        cother[2] = and_(g, K(R_COLOR1), I(0xff));
        break;
    }
    default: for (int i = 0; i < 3; i++) cother[i] = mov(g, I(0)); break;   /* LFB: src_r is still 0 */
    }
    for (int i = 0; i < 3; i++) drop(g, iter[i]);
    if (fbz & FBZ_CHROMAKEY) {
        /* cother == chromaKey: skip */
        V ck = mov(g, I(-1));
        for (int i = 0; i < 3; i++) {
            V key = shr(g, K(R_CHROMA), I(16 - 8 * i));
            take(g, &key, and_(g, D(key), I(0xff)));
            V eq = cmp(g, GEN8_COND_Z, D(cother[i]), D(key));
            drop(g, key);
            take(g, &ck, and_(g, D(ck), D(eq)));
            drop(g, eq);
        }
        take(g, &ck, xor_(g, D(ck), I(-1)));
        keep(g, &alive, ck);
    }
    /* alocal */
    V alocal;
    switch (cca_localselect) {
    case CCA_LOCALSELECT_ITER_A: alocal = mov(g, D(iter_a)); break;
    case CCA_LOCALSELECT_COLOR0: alocal = shr(g, K(R_COLOR0), I(24)); break;
    case CCA_LOCALSELECT_ITER_Z: alocal = clamp_shift(g, z, 20); break;
    default: alocal = mov(g, I(0xff)); break;
    }
    V aother;
    switch ((params->fbzColorPath >> 2) & 3) {
    case A_SEL_ITER_A: aother = mov(g, D(iter_a)); break;
    case A_SEL_TEX: aother = mov(g, D(tex.a)); break;
    case A_SEL_COLOR1: aother = shr(g, K(R_COLOR1), I(24)); break;
    default: aother = mov(g, I(0)); break;
    }
    if (fbz & FBZ_ALPHA_MASK) keep(g, &alive, and_(g, D(aother), I(1)));
    /* colour combine */
    V src[3], src_a;
    for (int i = 0; i < 3; i++) src[i] = cc_zero_other ? mov(g, I(0)) : mov(g, D(cother[i]));
    src_a = cca_zero_other ? mov(g, I(0)) : mov(g, D(aother));
    if (cc_sub_clocal) for (int i = 0; i < 3; i++) take(g, &src[i], sub(g, D(src[i]), D(clocal[i])));
    if (cca_sub_clocal) take(g, &src_a, sub(g, D(src_a), D(alocal)));
    V msel[3], msel_a;
    switch (cc_mselect) {
    case CC_MSELECT_CLOCAL: for (int i = 0; i < 3; i++) msel[i] = mov(g, D(clocal[i])); break;
    case CC_MSELECT_AOTHER: for (int i = 0; i < 3; i++) msel[i] = mov(g, D(aother)); break;
    case CC_MSELECT_ALOCAL: for (int i = 0; i < 3; i++) msel[i] = mov(g, D(alocal)); break;
    case CC_MSELECT_TEX: for (int i = 0; i < 3; i++) msel[i] = mov(g, D(tex.a)); break;
    case CC_MSELECT_TEXRGB: msel[0] = mov(g, D(tex.r)); msel[1] = mov(g, D(tex.g)); msel[2] = mov(g, D(tex.b)); break;
    default: for (int i = 0; i < 3; i++) msel[i] = mov(g, I(0)); break;
    }
    switch (cca_mselect) {
    case CCA_MSELECT_ALOCAL: case CCA_MSELECT_ALOCAL2: msel_a = mov(g, D(alocal)); break;
    case CCA_MSELECT_AOTHER: msel_a = mov(g, D(aother)); break;
    case CCA_MSELECT_TEX: msel_a = mov(g, D(tex.a)); break;
    default: msel_a = mov(g, I(0)); break;
    }
    tex_drop(g, &tex);
    for (int i = 0; i < 3; i++) {
        if (!cc_reverse_blend) take(g, &msel[i], xor_(g, D(msel[i]), I(0xff)));
        take(g, &msel[i], add(g, D(msel[i]), I(1)));
        V p = mul(g, D(src[i]), W(msel[i]));
        drop(g, msel[i]);
        take(g, &src[i], asr(g, D(p), I(8)));
        drop(g, p);
    }
    if (!cca_reverse_blend) take(g, &msel_a, xor_(g, D(msel_a), I(0xff)));
    take(g, &msel_a, add(g, D(msel_a), I(1)));
    {
        V p = mul(g, D(src_a), W(msel_a));
        drop(g, msel_a);
        take(g, &src_a, asr(g, D(p), I(8)));
        drop(g, p);
    }
    switch (cc_add) {
    case CC_ADD_CLOCAL: for (int i = 0; i < 3; i++) take(g, &src[i], add(g, D(src[i]), D(clocal[i]))); break;
    case CC_ADD_ALOCAL: for (int i = 0; i < 3; i++) take(g, &src[i], add(g, D(src[i]), D(alocal))); break;
    default: break;
    }
    if (cca_add) take(g, &src_a, add(g, D(src_a), D(alocal)));
    for (int i = 0; i < 3; i++) { take(g, &src[i], clamp255(g, src[i])); drop(g, clocal[i]); drop(g, cother[i]); }
    take(g, &src_a, clamp255(g, src_a));
    drop(g, alocal);
    drop(g, aother);
    if (cc_invert_output) for (int i = 0; i < 3; i++) take(g, &src[i], xor_(g, D(src[i]), I(0xff)));
    if (cca_invert_output) take(g, &src_a, xor_(g, D(src_a), I(0xff)));
    V colbfog[3] = { -1, -1, -1 };
    const int dest_afunc_ = (params->alphaMode >> 12) & 0xf;
    if (blend && dest_afunc_ == AFUNC_ACOLORBEFOREFOG)
        for (int i = 0; i < 3; i++) colbfog[i] = mov(g, D(src[i]));

    /* fog (APPLY_FOG) */
    if (params->fogMode & FOG_ENABLE) {
        if (params->fogMode & FOG_CONSTANT) {
            for (int i = 0; i < 3; i++) {
                V f = shr(g, K(R_FOGCOLOR), I(16 - 8 * i));
                take(g, &f, and_(g, D(f), I(0xff)));
                take(g, &src[i], add(g, D(src[i]), D(f)));
                drop(g, f);
            }
        } else {
            V fog[3];
            for (int i = 0; i < 3; i++) {
                if (params->fogMode & FOG_ADD) fog[i] = mov(g, I(0));
                else {
                    V f = shr(g, K(R_FOGCOLOR), I(16 - 8 * i));
                    fog[i] = and_(g, D(f), I(0xff));
                    drop(g, f);
                }
                if (!(params->fogMode & FOG_MULT)) take(g, &fog[i], sub(g, D(fog[i]), D(src[i])));
            }
            V fog_a;
            switch (params->fogMode & (FOG_Z | FOG_ALPHA)) {
            case 0: {
                V idx = asr(g, D(w_depth), I(10));
                take(g, &idx, and_(g, D(idx), I(0x3f)));
                take(g, &idx, add(g, D(idx), K(R_FOGTAB)));
                V entry = ld1(g, V2G_BT_DATA, idx);
                drop(g, idx);
                fog_a = and_(g, D(entry), I(0xff));
                V dfog = shr(g, D(entry), I(8));
                drop(g, entry);
                V frac = asr(g, D(w_depth), I(2));
                take(g, &frac, and_(g, D(frac), I(0xff)));
                V p = mul(g, D(dfog), W(frac));
                drop(g, dfog);
                drop(g, frac);
                take(g, &p, asr(g, D(p), I(10)));
                take(g, &fog_a, add(g, D(fog_a), D(p)));
                drop(g, p);
                break;
            }
            case FOG_Z: {
                V a = asr(g, D(z), I(20));
                fog_a = and_(g, D(a), I(0xff));
                drop(g, a);
                break;
            }
            case FOG_ALPHA: fog_a = mov(g, D(iter_a)); break;
            default: {
                /* CLAMP((w >> 32) & 0xff): the integer part of W */
                V f = fop(g, GEN8_OP_RNDD, F(wf), F(wf));
                V wi = ftoi(g, f);
                drop(g, f);
                fog_a = and_(g, D(wi), I(0xff));
                drop(g, wi);
                break;
            }
            }
            take(g, &fog_a, add(g, D(fog_a), I(1)));
            for (int i = 0; i < 3; i++) {
                V p = mul(g, D(fog[i]), W(fog_a));
                drop(g, fog[i]);
                V f = asr(g, D(p), I(8));
                drop(g, p);
                if (params->fogMode & FOG_MULT) take(g, &src[i], f);
                else { take(g, &src[i], add(g, D(src[i]), D(f))); drop(g, f); }
            }
            drop(g, fog_a);
        }
        for (int i = 0; i < 3; i++) take(g, &src[i], clamp255(g, src[i]));
    }
    drop(g, iter_a);
    drop(g, z);
    drop(g, wf);
    drop(g, w_depth);

    /* alpha test */
    if (params->alphaMode & 1) keep(g, &alive, compare_mask(g, (params->alphaMode >> 1) & 7, src_a, K(R_AREF)));

    /* the destination: read only now, while few values are live */
    if (blend) {
        V index = add(g, D(rowel), K(R_COLROW));
        V dat = ld1(g, V2G_BT_FB, index);
        drop(g, index);
        dest_r = expand(g, dat, 8, 0xf8, 5);
        dest_g = expand(g, dat, 3, 0xfc, 6);
        dest_b = expand(g, dat, -3, 0xf8, 5);
        drop(g, dat);
    }
    if (blend || (fbz & FBZ_ALPHA_ENABLE)) {
        if (fbz & FBZ_ALPHA_ENABLE) {
            V index = add(g, D(rowel), K(R_AUXROW));
            V aux = ld1(g, V2G_BT_FB, index);
            drop(g, index);
            dest_a = and_(g, D(aux), I(0xff));
            drop(g, aux);
        } else {
            dest_a = mov(g, I(0xff));
        }
    }
    drop(g, rowel);

    /* alpha blending */
    if (blend) {
        if ((fbz & FBZ_DITHER_SUB) && voodoo->dithersub_enabled) {
            bool two = fbz & FBZ_DITHER_2x2;
            take(g, &dest_r, table_lookup(c, two ? TABLE_SUB_RB2 : TABLE_SUB_RB, dest_r, two));
            take(g, &dest_g, table_lookup(c, two ? TABLE_SUB_G2 : TABLE_SUB_G, dest_g, two));
            take(g, &dest_b, table_lookup(c, two ? TABLE_SUB_RB2 : TABLE_SUB_RB, dest_b, two));
        }
        V dst[3] = { dest_r, dest_g, dest_b };
        V newdest[3];
        for (int i = 0; i < 3; i++) {
            V factor = -1;
            bool inverse = false;
            switch (dest_afunc_) {
            case AFUNC_AZERO: newdest[i] = mov(g, I(0)); continue;
            case AFUNC_AONE: newdest[i] = mov(g, D(dst[i])); continue;
            case AFUNC_ASRC_ALPHA: factor = src_a; break;
            case AFUNC_A_COLOR: factor = src[i]; break;
            case AFUNC_ADST_ALPHA: factor = dest_a; break;
            case AFUNC_AOMSRC_ALPHA: factor = src_a; inverse = true; break;
            case AFUNC_AOM_COLOR: factor = src[i]; inverse = true; break;
            case AFUNC_AOMDST_ALPHA: factor = dest_a; inverse = true; break;
            case AFUNC_ACOLORBEFOREFOG: factor = colbfog[i]; break;
            default: newdest[i] = mov(g, I(0)); continue;
            }
            V f = inverse ? add(g, gen8_neg(D(factor)), I(255)) : mov(g, D(factor));
            V p = mul(g, D(dst[i]), W(f));
            drop(g, f);
            newdest[i] = div255(g, p);
            drop(g, p);
        }
        const int src_afunc_ = (params->alphaMode >> 8) & 0xf;
        V sat = -1;
        if (src_afunc_ == AFUNC_ASATURATE) {
            V a = add(g, gen8_neg(D(dest_a)), I(255));
            sat = min_(g, D(src_a), D(a));
            drop(g, a);
        }
        V old_src[3];
        for (int i = 0; i < 3; i++) old_src[i] = mov(g, D(src[i]));
        for (int i = 0; i < 3; i++) {
            V factor = -1;
            bool inverse = false;
            switch (src_afunc_) {
            case AFUNC_AZERO: take(g, &src[i], mov(g, I(0))); continue;
            case AFUNC_AONE: continue;
            case AFUNC_ASRC_ALPHA: factor = src_a; break;
            case AFUNC_A_COLOR: factor = dst[i]; break;
            case AFUNC_ADST_ALPHA: factor = dest_a; break;
            case AFUNC_AOMSRC_ALPHA: factor = src_a; inverse = true; break;
            case AFUNC_AOM_COLOR: factor = dst[i]; inverse = true; break;
            case AFUNC_AOMDST_ALPHA: factor = dest_a; inverse = true; break;
            case AFUNC_ASATURATE: factor = sat; break;
            default: continue;
            }
            V f = inverse ? add(g, gen8_neg(D(factor)), I(255)) : mov(g, D(factor));
            V p = mul(g, D(old_src[i]), W(f));
            drop(g, f);
            take(g, &src[i], div255(g, p));
            drop(g, p);
        }
        for (int i = 0; i < 3; i++) drop(g, old_src[i]);
        drop(g, sat);
        for (int i = 0; i < 3; i++) {
            take(g, &src[i], add(g, D(src[i]), D(newdest[i])));
            drop(g, newdest[i]);
            take(g, &src[i], clamp255(g, src[i]));
        }
        /* src_a = (((dest_aafunc == 4) ? dest_a * 256 : 0) + ((src_aafunc == 4) ? src_a * 256 : 0)) >> 8 */
        const bool da = ((params->alphaMode >> 20) & 0xf) == 4, sa = ((params->alphaMode >> 16) & 0xf) == 4;
        V na = da && sa ? add(g, D(dest_a), D(src_a)) : da ? mov(g, D(dest_a)) : sa ? mov(g, D(src_a)) : mov(g, I(0));
        take(g, &src_a, na);
    }
    for (int i = 0; i < 3; i++) drop(g, colbfog[i]);
    drop(g, dest_r); drop(g, dest_g); drop(g, dest_b); drop(g, dest_a);

    /* dither and pack */
    if (fbz & FBZ_DITHER) {
        bool two = fbz & FBZ_DITHER_2x2;
        take(g, &src[0], table_lookup(c, two ? TABLE_RB2 : TABLE_RB, src[0], two));
        take(g, &src[1], table_lookup(c, two ? TABLE_G2 : TABLE_G, src[1], two));
        take(g, &src[2], table_lookup(c, two ? TABLE_RB2 : TABLE_RB, src[2], two));
    } else {
        take(g, &src[0], asr(g, D(src[0]), I(3)));
        take(g, &src[1], asr(g, D(src[1]), I(2)));
        take(g, &src[2], asr(g, D(src[2]), I(3)));
    }
    V color = shl(g, D(src[0]), I(11));
    V t = shl(g, D(src[1]), I(5));
    take(g, &color, or_(g, D(color), D(t)));
    drop(g, t);
    take(g, &color, or_(g, D(color), D(src[2])));
    for (int i = 0; i < 3; i++) drop(g, src[i]);
    drop(g, c->x);
    drop(g, c->row);
    drop(g, c->dxf);
    drop(g, c->dyf);
    drop(g, c->pos4);
    drop(g, c->pos2);

    const bool write_color = fbz & FBZ_RGB_WMASK;
    V aux = -1;
    if ((fbz & (FBZ_DEPTH_WMASK | FBZ_ALPHA_ENABLE)) == (FBZ_DEPTH_WMASK | FBZ_ALPHA_ENABLE)) aux = mov(g, D(src_a));
    else if ((fbz & (FBZ_DEPTH_WMASK | FBZ_DEPTH_ENABLE)) == (FBZ_DEPTH_WMASK | FBZ_DEPTH_ENABLE)) aux = mov(g, D(new_depth));
    drop(g, src_a);
    drop(g, new_depth);
    if (!write_color && aux < 0) return false;    /* nothing to write: the CPU counts it */

    /* header: g0, g1 with the pixel mask in g1.7 */
    Gen8Ctl nm = plain;
    nm.nomask = 1;
    emit(g, gen8_inst(GEN8_OP_MOV, 8, gen8_dst(RT_MSG, 0, GEN8_TYPE_UD), gen8_vec_t(0, GEN8_TYPE_UD),
                      gen8_vec_t(0, GEN8_TYPE_UD), nm));
    emit(g, gen8_inst(GEN8_OP_MOV, 8, gen8_dst(RT_MSG + 1, 0, GEN8_TYPE_UD), gen8_vec_t(1, GEN8_TYPE_UD),
                      gen8_vec_t(1, GEN8_TYPE_UD), nm));
    {
        Gen8Ctl cc = plain;
        cc.cond = GEN8_COND_NZ;
        cc.flag_sub = 1;
        emit(g, gen8_inst(GEN8_OP_CMP, 16, gen8_dst_null(GEN8_TYPE_D), D(alive), I(0), cc));
        emit(g, gen8_inst(GEN8_OP_AND, 1, gen8_dst(RT_MSG + 1, 28, GEN8_TYPE_UW), gen8_flag(1),
                          gen8_scalar_t(1, 28, GEN8_TYPE_UW), nm));
    }
    drop(g, alive);
    if (write_color) {
        opd(g, GEN8_OP_MOV, RT_MSG + 2, D(color), D(color));
        emit(g, gen8_send(GEN8_SFID_RENDER, 0, RT_MSG, GEN8_DESC_RTW16_HDR(V2G_BT_COLOR, aux < 0), aux < 0));
    }
    if (aux >= 0) {
        opd(g, GEN8_OP_MOV, RT_MSG + 2, D(aux), D(aux));
        emit(g, gen8_send(GEN8_SFID_RENDER, 0, RT_MSG, GEN8_DESC_RTW16_HDR(V2G_BT_AUX, 1), 1));
    }
    drop(g, color);
    drop(g, aux);
    return !g->fail;
}

/* ================= kernels ================= */

static KernelKey key_of(const voodoo_t *voodoo, const voodoo_params_t *params)
{
    KernelKey k;
    memset(&k, 0, sizeof(k));
    k.fbzMode = params->fbzMode;
    k.fbzColorPath = params->fbzColorPath;
    k.alphaMode = params->alphaMode & 0x00ffffff;
    k.fogMode = params->fogMode;
    k.textureMode[0] = params->textureMode[0];
    k.textureMode[1] = params->textureMode[1];
    k.tLOD_mirror[0] = params->tLOD[0] & (LOD_TMIRROR_S | LOD_TMIRROR_T);
    k.tLOD_mirror[1] = params->tLOD[1] & (LOD_TMIRROR_S | LOD_TMIRROR_T);
    k.flags = (voodoo->dual_tmus ? 1 : 0) | (voodoo->bilinear_enabled ? 2 : 0) | (voodoo->dithersub_enabled ? 4 : 0) |
              ((voodoo->trexInit1[0] & (1 << 18)) ? 8 : 0);
    return k;
}

static Kernel *kernel_for(V2gState *state, const voodoo_t *voodoo, const voodoo_params_t *params)
{
    KernelKey key = key_of(voodoo, params);
    for (unsigned i = 0; i < KERNEL_SLOTS; i++)
        if (state->kernels[i].used && !memcmp(&state->kernels[i].key, &key, sizeof(key)))
            return state->kernels[i].count ? &state->kernels[i] : NULL;
    Kernel *k = &state->kernels[state->next_slot];
    state->next_slot = (state->next_slot + 1) % KERNEL_SLOTS;
    if (!k->code) k->code = malloc(sizeof(Gen8Inst) * MAX_CODE);
    if (!k->code) return NULL;
    G g;
    memset(&g, 0, sizeof(g));
    g.code = k->code;
    bool ok = generate(voodoo, params, &g);
    k->key = key;
    k->used = true;
    k->count = ok ? g.n : 0;     /* 0: remembered as not drawable here */
    if (!ok)
        printf("voodoo2 gpu: no kernel for fbzMode %08x fbzColorPath %08x alphaMode %08x fogMode %08x "
               "textureMode %08x %08x flags %x (%u instructions%s)\n", key.fbzMode, key.fbzColorPath, key.alphaMode,
               key.fogMode, key.textureMode[0], key.textureMode[1], key.flags, g.n, g.fail ? ", out of registers or space" : "");
    k->id = state->next_id++;
    state->stats.kernels++;
    state->stats.kernel_bytes += g.n * 16u;
    return ok ? k : NULL;
}

/* ================= triangles ================= */

V2gState *voodoo2_gpu_create(const V2gBackend *backend)
{
    V2gState *state = calloc(1, sizeof(*state));
    if (!state) return NULL;
    state->backend = *backend;
    state->cover = calloc((size_t)COVER_ROWS * COVER_WORDS, sizeof(uint64_t));
    if (!state->cover) { free(state); return NULL; }
    state->cover_y0 = COVER_ROWS;
    state->cover_y1 = -1;
    voodoo2_gpu_tables(NULL);
    return state;
}

void voodoo2_gpu_destroy(V2gState *state)
{
    if (!state) return;
    for (unsigned i = 0; i < KERNEL_SLOTS; i++) free(state->kernels[i].code);
    free(state->cover);
    free(state);
}

bool voodoo2_gpu_pending(const V2gState *state) { return state && state->pending; }

void voodoo2_gpu_cpu_wrote(V2gState *state, uint32_t lo, uint32_t hi)
{
    if (!state || hi <= lo) return;
    if (state->cpu_hi <= state->cpu_lo) {
        state->cpu_lo = lo;
        state->cpu_hi = hi;
        return;
    }
    if (lo < state->cpu_lo) state->cpu_lo = lo;
    if (hi > state->cpu_hi) state->cpu_hi = hi;
}

void voodoo2_gpu_texture_decoded(V2gState *state, unsigned tmu, unsigned entry, uint32_t first, uint32_t end)
{
    if (state && state->backend.texture_written)
        state->backend.texture_written(state->backend.ctx, tmu, entry, first, end);
}

V2gStats voodoo2_gpu_stats(const V2gState *state)
{
    V2gStats none = { 0 };
    return state ? state->stats : none;
}

static void cover_clear(V2gState *state)
{
    if (state->cover_y1 >= state->cover_y0)
        memset(state->cover + (size_t)state->cover_y0 * COVER_WORDS, 0,
               (size_t)(state->cover_y1 - state->cover_y0 + 1) * COVER_WORDS * sizeof(uint64_t));
    state->cover_y0 = COVER_ROWS;
    state->cover_y1 = -1;
}

static uint64_t range_mask(int lo, int hi, int word)
{
    int a = lo - word * 64, b = hi - word * 64;
    if (a < 0) a = 0;
    if (b > 63) b = 63;
    if (a > b) return 0;
    uint64_t upper = b == 63 ? ~0ull : ((1ull << (b + 1)) - 1);
    return upper & ~((1ull << a) - 1);
}

void voodoo2_gpu_finish(V2gState *state, voodoo_t *voodoo)
{
    if (!state || (!state->pending && !state->owed)) return;
    if (state->pending) {
        uint64_t t0 = time_us_64();
        state->backend.finish(state->backend.ctx);
        state->stats.finish_us += time_us_64() - t0;
        state->stats.finishes++;
    }
    state->pending = false;
    state->owed = false;
    state->lodtab_valid[0] = state->lodtab_valid[1] = false;
    state->fogtab_valid = false;
    cover_clear(state);
    /* the textures the GPU used are free now: the counts 86Box's render
     * threads would have made (voodoo_half_triangle's refcount_r++) */
    for (unsigned tmu = 0; tmu < 2; tmu++)
        for (unsigned entry = 0; entry < TEX_CACHE_MAX; entry++) {
            if (!state->texture_used[tmu][entry]) continue;
            for (int worker = 0; worker < 4; worker++)
                voodoo->texture_cache[tmu][entry].refcount_r[worker] += state->texture_owed[tmu][entry];
            state->texture_used[tmu][entry] = 0;
            state->texture_owed[tmu][entry] = 0;
        }
}

static int32_t sext16(int32_t v)
{
    v &= 0xffff;
    return (v & 0x8000) ? v | (int32_t)0xffff0000 : v;
}

/* 32-bit wrapping arithmetic, as the core's int computations */
static int32_t wrap_mul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

static float to_float(int64_t fixed32) { return (float)((double)fixed32 / 4294967296.0); }

static void lod_table(const voodoo_params_t *params, int tmu, uint32_t entry_index, uint32_t out[9][4])
{
    for (int lod = 0; lod <= LOD_MAX; lod++) {
        out[lod][0] = (uint32_t)params->tex_w_mask[tmu][lod];
        out[lod][1] = (uint32_t)params->tex_h_mask[tmu][lod];
        out[lod][2] = (uint32_t)params->tex_lod[tmu][lod];
        out[lod][3] = entry_index + texture_offset[lod];
    }
}

/* Mirrors voodoo_triangle()'s set-up and voodoo_half_triangle()'s spans. */
static bool queue(V2gState *state, voodoo_t *voodoo, voodoo_params_t *params, unsigned *why)
{
    const uint32_t fbz = params->fbzMode;
    if (fbz & FBZ_STIPPLE) { *why = V2G_WHY_STIPPLE; return false; }
    if (params->col_tiled || params->aux_tiled) { *why = V2G_WHY_TILED; return false; }
    if (SLI_ENABLED) { *why = V2G_WHY_SLI; return false; }
    Kernel *kernel = kernel_for(state, voodoo, params);
    if (!kernel) { *why = V2G_WHY_KERNEL; return false; }

    V2gDraw draw;
    memset(&draw, 0, sizeof(draw));
    uint32_t *rec = draw.record;
    int dx = 8 - (params->vertexAx & 0xf);
    if ((params->vertexAx & 0xf) > 8) dx += 16;
    int dy = 8 - (params->vertexAy & 0xf);
    if ((params->vertexAy & 0xf) > 8) dy += 16;
    int32_t start[5] = { (int32_t)params->startR, (int32_t)params->startG, (int32_t)params->startB,
                         (int32_t)params->startA, (int32_t)params->startZ };
    int32_t ddx[5] = { params->dRdX, params->dGdX, params->dBdX, params->dAdX, params->dZdX };
    int32_t ddy[5] = { params->dRdY, params->dGdY, params->dBdY, params->dAdY, params->dZdY };
    int64_t st[2][3], sdx[2][3], sdy[2][3];
    for (int t = 0; t < 2; t++) {
        st[t][0] = params->tmu[t].startS; sdx[t][0] = params->tmu[t].dSdX; sdy[t][0] = params->tmu[t].dSdY;
        st[t][1] = params->tmu[t].startT; sdx[t][1] = params->tmu[t].dTdX; sdy[t][1] = params->tmu[t].dTdY;
        st[t][2] = params->tmu[t].startW; sdx[t][2] = params->tmu[t].dWdX; sdy[t][2] = params->tmu[t].dWdY;
    }
    int64_t w0 = params->startW;
    if (params->fbzColorPath & FBZ_PARAM_ADJUST) {
        for (int p = 0; p < 5; p++) start[p] += (dx * ddx[p] + dy * ddy[p]) >> 4;
        for (int t = 0; t < 2; t++)
            for (int p = 0; p < 3; p++) st[t][p] += (dx * sdx[t][p] + dy * sdy[t][p]) >> 4;
        w0 += (dx * params->dWdX + dy * params->dWdY) >> 4;
    }
    int32_t Ay = sext16(params->vertexAy), By = sext16(params->vertexBy), Cy = sext16(params->vertexCy);
    int32_t Ax = sext16(params->vertexAx), Bx = sext16(params->vertexBx), Cx = sext16(params->vertexCx);
    int ystart = (Ay + 7) >> 4, yend = (Cy + 7) >> 4;
    int dxAB = By - Ay ? (int)((((int64_t)Bx << 12) - ((int64_t)Ax << 12)) << 4) / (By - Ay) : 0;
    int dxAC = Cy - Ay ? (int)((((int64_t)Cx << 12) - ((int64_t)Ax << 12)) << 4) / (Cy - Ay) : 0;
    int dxBC = Cy - By ? (int)((((int64_t)Cx << 12) - ((int64_t)Bx << 12)) << 4) / (Cy - By) : 0;
    const int xdir = params->sign ? -1 : 1;
    const int yref = ystart;
    if ((fbz & 1) && ystart < params->clipLowY) ystart = params->clipLowY;
    if ((fbz & 1) && yend >= params->clipHighY) yend = params->clipHighY;
    if (yend - ystart > SPAN_LINES) { *why = V2G_WHY_RANGE; return false; }
    const int xa = ((Ax << 12) + 0x7000) >> 16;
    const int y_origin = voodoo->v_disp - 1;
    const bool flip = fbz & (1 << 17);
    const uint32_t row_el = params->row_width / 2;
    const uint32_t fb_bytes = voodoo->fb_mask + 1;
    const uint32_t top = params->draw_offset > params->aux_offset ? params->draw_offset : params->aux_offset;
    if (!row_el || top >= fb_bytes || (params->draw_offset | params->aux_offset | params->row_width) & 1) {
        *why = V2G_WHY_RANGE;
        return false;
    }
    const int rows = (int)((fb_bytes - top) / params->row_width);

    /* spans */
    int lines = yend > ystart ? yend - ystart : 0;
    uint32_t span_index = 0;
    uint32_t *spans = NULL;
    if (lines) {
        spans = state->backend.alloc_data(state->backend.ctx, (unsigned)lines, 1, &span_index);
        if (!spans) { *why = V2G_WHY_BATCH; return false; }
    }
    int bx0 = 1 << 30, bx1 = -1, by0 = 1 << 30, by1 = -1;
    uint64_t pixels = 0;
    for (int y = ystart; y < yend; y++) {
        int real_y = (y << 4) + 8;
        int x = (Ax << 12) + (wrap_mul(dxAC, real_y - Ay) >> 4);
        int x2 = real_y < By ? (Ax << 12) + (wrap_mul(dxAB, real_y - Ay) >> 4)
                             : (Bx << 12) + (wrap_mul(dxBC, real_y - By) >> 4);
        int row = flip ? y_origin - y : y;
        if (xdir > 0) x2 -= 1 << 16;
        else x -= 1 << 16;
        x = (x + 0x7000) >> 16;
        x2 = (x2 + 0x7000) >> 16;
        int clip_left = (fbz & 1) ? params->clipLeft : 0, clip_right = (fbz & 1) ? params->clipRight : 4096;
        if ((fbz & 1) || ((x | x2) & ~0xfff)) {
            if (xdir > 0) {
                if (x < clip_left) x = clip_left;
                if (x2 >= clip_right) x2 = clip_right - 1;
            } else {
                if (x >= clip_right) x = clip_right - 1;
                if (x2 < clip_left) x2 = clip_left;
            }
        }
        uint32_t span = 1;     /* empty: x_lo 1, x_hi 0 */
        if (!((x2 < x && xdir > 0) || (x2 > x && xdir < 0))) {
            int lo = x < x2 ? x : x2, hi = x < x2 ? x2 : x;
            if (row < 0 || row >= rows || row >= COVER_ROWS || hi >= (int)row_el || hi >= COVER_WIDTH || lo < 0) {
                *why = V2G_WHY_RANGE;
                return false;
            }
            span = (uint32_t)lo | (uint32_t)hi << 16;
            if (lo < bx0) bx0 = lo;
            if (hi > bx1) bx1 = hi;
            if (row < by0) by0 = row;
            if (row > by1) by1 = row;
            pixels += (uint64_t)(hi - lo + 1);
        }
        spans[y - ystart] = span;
    }
    if (bx1 < 0) {
        /* nothing to draw: as the core, only the texture counts */
        state->stats.triangles++;
        return true;
    }

    /* Hazards: pixels an earlier draw since the last barrier wrote. A shader
     * reading them must see that write; and the Cherryview GPU does not keep
     * the order of two draws' writes to one pixel either (physical test: the
     * earlier draw's colour survived), so every overlap gets a barrier. */
    bool hazard = false;
    {
        for (int y = ystart; y < yend && !hazard; y++) {
            uint32_t span = spans[y - ystart];
            int lo = span & 0xffff, hi = span >> 16;
            if (lo > hi) continue;
            int row = flip ? y_origin - y : y;
            if (row < state->cover_y0 || row > state->cover_y1) continue;
            const uint64_t *words = state->cover + (size_t)row * COVER_WORDS;
            for (int w = lo / 64; w <= hi / 64; w++)
                if (words[w] & range_mask(lo, hi, w)) { hazard = true; break; }
        }
        if (hazard) cover_clear(state);
    }
    for (int y = ystart; y < yend; y++) {
        uint32_t span = spans[y - ystart];
        int lo = span & 0xffff, hi = span >> 16;
        if (lo > hi) continue;
        int row = flip ? y_origin - y : y;
        uint64_t *words = state->cover + (size_t)row * COVER_WORDS;
        for (int w = lo / 64; w <= hi / 64; w++) words[w] |= range_mask(lo, hi, w);
        if (row < state->cover_y0) state->cover_y0 = row;
        if (row > state->cover_y1) state->cover_y1 = row;
    }

    /* the record */
    rec[R_XA] = (uint32_t)xa;
    rec[R_YREF] = (uint32_t)yref;
    rec[R_YSTART] = (uint32_t)ystart;
    rec[R_YEND] = (uint32_t)yend;
    rec[R_SPAN] = span_index;
    rec[R_YFLIP] = (uint32_t)y_origin;
    rec[R_COLROW] = params->draw_offset / 2;
    rec[R_AUXROW] = params->aux_offset / 2;
    rec[R_ROWEL] = row_el;
    for (int p = 0; p < 5; p++) {
        rec[R_PLANE + p * 3] = (uint32_t)start[p];
        rec[R_PLANE + p * 3 + 1] = (uint32_t)ddx[p];
        rec[R_PLANE + p * 3 + 2] = (uint32_t)ddy[p];
    }
    union { float f; uint32_t u; } cv;
#define PUTF(i, v) do { cv.f = (v); rec[i] = cv.u; } while (0)
    PUTF(R_W, to_float(w0));
    PUTF(R_W + 1, to_float(params->dWdX));
    PUTF(R_W + 2, to_float(params->dWdY));
    for (int t = 0; t < 2; t++)
        for (int p = 0; p < 3; p++) {
            PUTF(R_TMU + t * 9 + p * 3, to_float(st[t][p]));
            PUTF(R_TMU + t * 9 + p * 3 + 1, to_float(sdx[t][p]));
            PUTF(R_TMU + t * 9 + p * 3 + 2, to_float(sdy[t][p]));
        }
#undef PUTF
    rec[R_COLOR0] = params->color0;
    rec[R_COLOR1] = params->color1;
    rec[R_ZACOLOR] = params->zaColor;
    rec[R_ZBIAS] = (uint32_t)(int32_t)(int16_t)params->zaColor;
    rec[R_CHROMA] = (uint32_t)(params->chromaKey_r & 0xff) << 16 | (uint32_t)(params->chromaKey_g & 0xff) << 8 |
                    (uint32_t)(params->chromaKey_b & 0xff);
    rec[R_FOGCOLOR] = (uint32_t)params->fogColor.r << 16 | (uint32_t)params->fogColor.g << 8 | params->fogColor.b;
    rec[R_AREF] = params->alphaMode >> 24;
    rec[R_TMUCONFIG] = voodoo->tmuConfig;
    const bool textured = params->fbzColorPath & FBZCP_TEXTURE_ENABLED;
    for (int t = 0; t < 2; t++) {
        /* voodoo_triangle_lod_base() and voodoo_lod_bias() */
        double dsdx = (double)params->tmu[t].dSdX / 4294967296.0, dtdx = (double)params->tmu[t].dTdX / 4294967296.0;
        double dsdy = (double)params->tmu[t].dSdY / 4294967296.0, dtdy = (double)params->tmu[t].dTdY / 4294967296.0;
        double gx = dsdx * dsdx + dtdx * dtdx, gy = dsdy * dsdy + dtdy * dtdy, gm = gx > gy ? gx : gy;
        int base = -(64 << 8);
        if (gm > 0.0) {
            double l = 0;
            /* log2 without libm: exponent and a 24-step refinement */
            double v = gm;
            int e = 0;
            while (v >= 2.0) { v /= 2.0; e++; }
            while (v < 1.0) { v *= 2.0; e--; }
            double frac = 0, bit = 0.5;
            for (int i = 0; i < 30; i++) {
                v *= v;
                if (v >= 2.0) { v /= 2.0; frac += bit; }
                bit /= 2.0;
            }
            l = e + frac;
            base = (int)(l / 2.0 * 256.0);
        }
        int bias = (params->tLOD[t] >> 12) & 0x3f;
        if (bias & 0x20) bias -= 0x40;
        bias *= 64;
        int lod_min = (params->tLOD[t] & 0x3f) << 6, lod_max = ((params->tLOD[t] >> 6) & 0x3f) << 6;
        if (lod_min > 0x800) lod_min = 0x800;
        if (lod_max > 0x800) lod_max = 0x800;
        rec[R_LOD + t] = (uint32_t)base;
        rec[R_LODBIAS + t] = (uint32_t)bias;
        rec[R_LODMIN + t] = (uint32_t)lod_min;
        rec[R_LODMAX + t] = (uint32_t)lod_max;
        rec[R_DETAILMAX + t] = (uint32_t)params->detail_max[t];
        rec[R_DETAILBIAS + t] = (uint32_t)params->detail_bias[t];
        rec[R_DETAILSCALE + t] = (uint32_t)params->detail_scale[t];
        if (textured && (t == 0 || voodoo->dual_tmus)) {
            uint32_t entry = (uint32_t)params->tex_entry[t];
            uint32_t base_index = state->backend.tex_index(state->backend.ctx, (unsigned)t, entry);
            uint32_t table[9][4];
            lod_table(params, t, base_index, table);
            if (!state->lodtab_valid[t] || memcmp(state->lodtab_key[t], table, sizeof(table))) {
                uint32_t index;
                uint32_t *data = state->backend.alloc_data(state->backend.ctx, 36, 4, &index);
                if (!data) { *why = V2G_WHY_BATCH; return false; }
                memcpy(data, table, sizeof(table));
                state->lodtab_index[t] = index / 4;
                memcpy(state->lodtab_key[t], table, sizeof(table));
                state->lodtab_valid[t] = true;
            }
            rec[R_LODTAB + t] = state->lodtab_index[t];
        }
    }
    if ((params->fogMode & (FOG_ENABLE | FOG_CONSTANT | FOG_Z | FOG_ALPHA)) == FOG_ENABLE) {
        if (!state->fogtab_valid || memcmp(state->fogtab_key, params->fogTable, sizeof(state->fogtab_key))) {
            uint32_t index;
            uint32_t *data = state->backend.alloc_data(state->backend.ctx, 64, 1, &index);
            if (!data) { *why = V2G_WHY_BATCH; return false; }
            for (int i = 0; i < 64; i++) data[i] = params->fogTable[i].fog | (uint32_t)params->fogTable[i].dfog << 8;
            memcpy(state->fogtab_key, params->fogTable, sizeof(state->fogtab_key));
            state->fogtab_index = index;
            state->fogtab_valid = true;
        }
        rec[R_FOGTAB] = state->fogtab_index;
    }

    draw.code = (const uint32_t *)kernel->code;
    draw.code_bytes = kernel->count * 16;
    draw.key_id = kernel->id;
    draw.x0 = bx0;
    draw.x1 = bx1;
    draw.y0 = by0;
    draw.y1 = by1;
    draw.draw_offset = params->draw_offset;
    draw.aux_offset = params->aux_offset;
    draw.row_width = params->row_width;
    draw.rows = (uint32_t)rows;
    draw.hazard = hazard;
    draw.cpu_lo = state->cpu_lo;
    draw.cpu_hi = state->cpu_hi;
    uint64_t t_draw = time_us_64();
    bool drawn = state->backend.draw(state->backend.ctx, &draw);
    state->stats.draw_us += time_us_64() - t_draw;
    if (!drawn) { *why = V2G_WHY_BATCH; return false; }
    state->cpu_lo = state->cpu_hi = 0;
    state->pending = true;
    state->stats.triangles++;
    state->stats.draws++;
    state->stats.barriers += hazard;
    state->stats.spans += (uint64_t)lines;
    state->stats.pixels += pixels;
    voodoo->fbiPixelsIn += (uint32_t)pixels;
    voodoo->fbiPixelsOut += (uint32_t)pixels;
    if (params->draw_offset == params->front_offset)
        for (int y = by0; y <= by1 && y < 2048; y++) voodoo->dirty_line[y] = 1;
    return true;
}

bool voodoo2_gpu_triangle(V2gState *state, voodoo_t *voodoo, voodoo_params_t *params)
{
    unsigned why = V2G_WHY_NONE;
    voodoo->tri_count++;
    uint64_t t0 = time_us_64();
    uint64_t draw0 = state->stats.draw_us;
    bool done = queue(state, voodoo, params, &why);
    state->stats.setup_us += time_us_64() - t0 - (state->stats.draw_us - draw0);
    if (!done && why == V2G_WHY_BATCH) {
        /* the batch is full: run it and try once more in an empty one */
        voodoo2_gpu_finish(state, voodoo);
        done = queue(state, voodoo, params, &why);
    }
    if (done) {
        /* both units' textures are owed a render-thread count when the GPU is done */
        for (unsigned tmu = 0; tmu < 2; tmu++) {
            unsigned entry = (unsigned)params->tex_entry[tmu];
            if (entry >= TEX_CACHE_MAX) continue;
            state->texture_used[tmu][entry] = 1;
            state->texture_owed[tmu][entry]++;
        }
        state->owed = true;
        return true;
    }
    voodoo->tri_count--;
    voodoo2_gpu_finish(state, voodoo);
    state->stats.cpu_triangles++;
    state->stats.last_fallback = why;
    return false;
}
