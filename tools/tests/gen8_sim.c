/* Gen8 EU interpreter for the tests; see gen8_sim.h. */
#include "gen8_sim.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int gen8_sim_trace;

static uint32_t field(const Gen8Inst *in, unsigned hi, unsigned lo)
{
    uint32_t v = 0;
    for (unsigned bit = hi + 1; bit-- > lo;) v = (v << 1) | ((in->dw[bit / 32] >> (bit % 32)) & 1);
    return v;
}

static unsigned type_size(unsigned type)
{
    switch (type) {
    case GEN8_TYPE_UD: case GEN8_TYPE_D: case GEN8_TYPE_F: return 4;
    case GEN8_TYPE_UW: case GEN8_TYPE_W: return 2;
    case GEN8_TYPE_UB: case GEN8_TYPE_B: return 1;
    default: return 0;
    }
}

static unsigned decode_count(unsigned code) { return code == 0 ? 0 : 1u << (code - 1); }   /* vstride/hstride */
static unsigned decode_width(unsigned code) { return 1u << code; }

typedef struct {
    unsigned file, type, reg, subreg, vstride, width, hstride, negate, abs;
    uint32_t imm;
} Operand;

/* raw: CMP writes all ones of its execution type, stored bit for bit */
typedef struct { bool is_float; double f; int64_t i; bool raw; uint32_t bits; } Value;

static bool load(Gen8Thread *t, const Operand *o, unsigned element, Value *out)
{
    out->raw = false;
    out->bits = 0;
    uint32_t raw = 0;
    if (o->file == GEN8_IMM) {
        if (o->type == GEN8_IMM_V) {
            int nib = (o->imm >> ((element % 8) * 4)) & 0xf;
            out->is_float = false;
            out->i = nib & 8 ? nib - 16 : nib;
            goto modifiers;
        }
        raw = o->imm;
        if (type_size(o->type) == 2) raw &= 0xffff;
    } else if (o->file == GEN8_ARF) {
        if (o->reg == GEN8_ARF_NULL) raw = 0;
        else if (o->reg == GEN8_ARF_FLAG && o->subreg < 4) raw = t->flag[o->subreg / 2];
        else { snprintf(t->error, sizeof t->error, "unmodelled ARF %02x", o->reg); return false; }
    } else if (o->file == GEN8_GRF) {
        unsigned size = type_size(o->type);
        unsigned row = element / o->width, col = element % o->width;
        unsigned offset = o->reg * 32 + o->subreg + (row * o->vstride + col * o->hstride) * size;
        if (!size || offset + size > sizeof t->grf) {
            snprintf(t->error, sizeof t->error, "region out of the register file");
            return false;
        }
        memcpy(&raw, &t->grf[0][0] + offset, size);
    } else {
        snprintf(t->error, sizeof t->error, "bad register file");
        return false;
    }
    switch (o->type) {
    case GEN8_TYPE_F: { float f; memcpy(&f, &raw, 4); out->is_float = true; out->f = f; break; }
    case GEN8_TYPE_UD: out->is_float = false; out->i = raw; break;
    case GEN8_TYPE_D: out->is_float = false; out->i = (int32_t)raw; break;
    case GEN8_TYPE_UW: out->is_float = false; out->i = raw & 0xffff; break;
    case GEN8_TYPE_W: out->is_float = false; out->i = (int16_t)raw; break;
    case GEN8_TYPE_UB: out->is_float = false; out->i = raw & 0xff; break;
    case GEN8_TYPE_B: out->is_float = false; out->i = (int8_t)raw; break;
    default: snprintf(t->error, sizeof t->error, "bad type %u", o->type); return false;
    }
modifiers:
    if (o->abs) { if (out->is_float) out->f = fabs(out->f); else if (out->i < 0) out->i = -out->i; }
    if (o->negate) { if (out->is_float) out->f = -out->f; else out->i = -out->i; }
    return true;
}

static uint32_t float_to_int(double f, bool is_signed)
{
    if (f != f) return 0;
    f = trunc(f);
    if (is_signed) {
        if (f >= 2147483647.0) return 0x7fffffff;
        if (f <= -2147483648.0) return 0x80000000u;
        return (uint32_t)(int32_t)f;
    }
    if (f <= 0) return 0;
    if (f >= 4294967295.0) return 0xffffffffu;
    return (uint32_t)f;
}

static bool store(Gen8Thread *t, unsigned file, unsigned reg, unsigned subreg, unsigned hstride, unsigned type,
                  unsigned element, Value v, bool saturate)
{
    if (file == GEN8_ARF && reg == GEN8_ARF_NULL) return true;
    if (file == GEN8_ARF && reg == GEN8_ARF_FLAG) {
        t->flag[subreg / 2] = (uint16_t)(v.is_float ? float_to_int(v.f, false) : (uint32_t)v.i);
        return true;
    }
    if (file != GEN8_GRF) { snprintf(t->error, sizeof t->error, "bad destination file"); return false; }
    unsigned size = type_size(type);
    unsigned offset = reg * 32 + subreg + element * hstride * size;
    if (!size || offset + size > sizeof t->grf) { snprintf(t->error, sizeof t->error, "destination out of range"); return false; }
    uint32_t raw;
    if (v.raw) {
        raw = v.bits;
    } else if (type == GEN8_TYPE_F) {
        double d = v.is_float ? v.f : (double)v.i;
        if (saturate) d = d < 0 ? 0 : d > 1 ? 1 : d;
        float f = (float)d;
        memcpy(&raw, &f, 4);
    } else if (v.is_float) {
        raw = float_to_int(v.f, type == GEN8_TYPE_D || type == GEN8_TYPE_W || type == GEN8_TYPE_B);
        if (size == 2) raw = type == GEN8_TYPE_W ? (uint32_t)(v.f > 32767 ? 32767 : v.f < -32768 ? -32768 : (int32_t)raw) : (raw > 65535 ? 65535 : raw);
    } else {
        raw = (uint32_t)v.i;
    }
    memcpy(&t->grf[0][0] + offset, &raw, size);
    return true;
}

static bool compare(unsigned cond, Value a, Value b, bool *result)
{
    double x = a.is_float || b.is_float ? (a.is_float ? a.f : (double)a.i) : 0;
    double y = a.is_float || b.is_float ? (b.is_float ? b.f : (double)b.i) : 0;
    bool fl = a.is_float || b.is_float;
    int c;
    if (fl) c = x < y ? -1 : x > y ? 1 : 0;
    else c = a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
    if (fl && (x != x || y != y)) { *result = cond == GEN8_COND_NZ; return true; }
    switch (cond) {
    case GEN8_COND_Z: *result = c == 0; return true;
    case GEN8_COND_NZ: *result = c != 0; return true;
    case GEN8_COND_G: *result = c > 0; return true;
    case GEN8_COND_GE: *result = c >= 0; return true;
    case GEN8_COND_L: *result = c < 0; return true;
    case GEN8_COND_LE: *result = c <= 0; return true;
    default: return false;
    }
}

static int64_t int32_wrap(int64_t v) { return (int64_t)(int32_t)(uint32_t)v; }

bool gen8_sim_run(Gen8Thread *t, const Gen8Inst *code, unsigned count, Gen8SendFn send, void *ctx)
{
    t->error[0] = 0;
    for (unsigned pc = 0; pc < count; pc++) {
        const Gen8Inst *in = &code[pc];
        unsigned opcode = field(in, 6, 0);
        if (field(in, 8, 8)) { snprintf(t->error, sizeof t->error, "%u: align16", pc); return false; }
        if (field(in, 29, 29)) { snprintf(t->error, sizeof t->error, "%u: compacted", pc); return false; }
        unsigned exec = 1u << field(in, 23, 21);
        unsigned pred = field(in, 19, 16), pred_inv = field(in, 20, 20);
        unsigned cond = field(in, 27, 24), saturate = field(in, 31, 31);
        unsigned flag_sub = field(in, 32, 32), nomask = field(in, 34, 34);
        unsigned dfile = field(in, 36, 35), dtype = field(in, 40, 37), dsub = field(in, 52, 48),
                 dreg = field(in, 60, 53), dhs = decode_count(field(in, 62, 61));
        if (field(in, 63, 63) || field(in, 79, 79)) { snprintf(t->error, sizeof t->error, "%u: indirect", pc); return false; }
        Operand s0 = { field(in, 42, 41), field(in, 46, 43), field(in, 76, 69), field(in, 68, 64),
                       decode_count(field(in, 88, 85)), decode_width(field(in, 84, 82)),
                       decode_count(field(in, 81, 80)), field(in, 78, 78), field(in, 77, 77), in->dw[3] };
        Operand s1 = { field(in, 90, 89), field(in, 94, 91), field(in, 108, 101), field(in, 100, 96),
                       decode_count(field(in, 120, 117)), decode_width(field(in, 116, 114)),
                       decode_count(field(in, 113, 112)), field(in, 110, 110), field(in, 109, 109), in->dw[3] };
        /* an immediate has no region or source modifiers: those bits are its value */
        if (s0.file == GEN8_IMM) s0.negate = s0.abs = 0;
        if (s1.file == GEN8_IMM) s1.negate = s1.abs = 0;
        if (s0.file == GEN8_IMM && s1.file == GEN8_IMM) { snprintf(t->error, sizeof t->error, "%u: two immediates", pc); return false; }
        if (s0.file == GEN8_IMM && opcode != GEN8_OP_MOV && opcode != GEN8_OP_SEND && opcode != GEN8_OP_NOT &&
            opcode != GEN8_OP_RNDD && opcode != GEN8_OP_RNDE && opcode != GEN8_OP_RNDZ && opcode != GEN8_OP_FRC &&
            opcode != GEN8_OP_LZD) {
            snprintf(t->error, sizeof t->error, "%u: immediate in src0 of a two-source instruction", pc);
            return false;
        }
        uint16_t mask = nomask ? (uint16_t)((1u << exec) - 1) : (uint16_t)(t->exec_mask & ((1u << exec) - 1));
        if (exec == 16 && !nomask) mask = t->exec_mask;
        if (opcode == GEN8_OP_SEND) {
            uint32_t desc = in->dw[3];
            if (s0.file != GEN8_GRF) { snprintf(t->error, sizeof t->error, "%u: send source", pc); return false; }
            send(ctx, t, cond, desc, s0.reg, dreg, mask);
            if (t->error[0]) return false;
            if (desc & 0x80000000u) {
                if (s0.reg < 112) { snprintf(t->error, sizeof t->error, "%u: EOT send below g112", pc); return false; }
                return true;
            }
            continue;
        }
        uint16_t enabled = mask;
        bool sel_pred = opcode == GEN8_OP_SEL && pred;
        if (pred && !sel_pred) {
            uint16_t f = t->flag[flag_sub];
            enabled &= pred_inv ? (uint16_t)~f : f;
        }
        uint16_t new_flags = t->flag[flag_sub];
        Value results[16];
        bool writes[16] = { 0 };
        for (unsigned e = 0; e < exec; e++) {
            if (!((enabled >> e) & 1)) continue;
            Value a, b = { false, 0, 0 }, r = { false, 0, 0 };
            if (!load(t, &s0, e, &a)) goto fail;
            bool two = !(opcode == GEN8_OP_MOV || opcode == GEN8_OP_NOT || opcode == GEN8_OP_RNDD ||
                         opcode == GEN8_OP_RNDE || opcode == GEN8_OP_RNDZ || opcode == GEN8_OP_FRC ||
                         opcode == GEN8_OP_LZD);
            if (opcode == GEN8_OP_MATH && s1.file == GEN8_ARF) two = false;
            if (two && !load(t, &s1, e, &b)) goto fail;
            bool fl = a.is_float || (two && b.is_float);
            double af = a.is_float ? a.f : (double)a.i, bf = b.is_float ? b.f : (double)b.i;
            switch (opcode) {
            case GEN8_OP_MOV: r = a; break;
            case GEN8_OP_ADD:
                if (fl) { r.is_float = true; r.f = (float)((float)af + (float)bf); }
                else r.i = int32_wrap(a.i + b.i);
                break;
            case GEN8_OP_MUL:
                if (fl) { r.is_float = true; r.f = (float)((float)af * (float)bf); }
                else {
                    if (type_size(s0.type) == 4 && type_size(s1.type) == 4 && s1.file != GEN8_IMM) {
                        snprintf(t->error, sizeof t->error, "%u: 32x32 integer multiply (not on Cherryview)", pc);
                        return false;
                    }
                    r.i = int32_wrap(a.i * b.i);
                }
                break;
            case GEN8_OP_AND: r.i = (int64_t)(uint32_t)((uint32_t)a.i & (uint32_t)b.i); break;
            case GEN8_OP_OR: r.i = (int64_t)(uint32_t)((uint32_t)a.i | (uint32_t)b.i); break;
            case GEN8_OP_XOR: r.i = (int64_t)(uint32_t)((uint32_t)a.i ^ (uint32_t)b.i); break;
            case GEN8_OP_NOT: r.i = (int64_t)(uint32_t)~(uint32_t)a.i; break;
            case GEN8_OP_SHL: r.i = (int64_t)(uint32_t)((uint32_t)a.i << (b.i & 31)); break;
            case GEN8_OP_SHR: r.i = (int64_t)((uint32_t)a.i >> (b.i & 31)); break;
            case GEN8_OP_ASR: r.i = (int64_t)((int32_t)(uint32_t)a.i >> (b.i & 31)); break;
            case GEN8_OP_LZD: { uint32_t v = (uint32_t)a.i; r.i = v ? __builtin_clz(v) : 32; break; }
            case GEN8_OP_RNDD: r.is_float = true; r.f = floor(af); break;
            case GEN8_OP_RNDE: r.is_float = true; r.f = nearbyint(af); break;
            case GEN8_OP_RNDZ: r.is_float = true; r.f = trunc(af); break;
            case GEN8_OP_FRC: r.is_float = true; r.f = af - floor(af); break;
            case GEN8_OP_SEL:
                if (sel_pred) {
                    bool f = (t->flag[flag_sub] >> e) & 1;
                    r = (f ^ pred_inv) ? a : b;
                } else {
                    bool c;
                    if (!compare(cond, a, b, &c)) { snprintf(t->error, sizeof t->error, "%u: sel cond", pc); return false; }
                    r = c ? a : b;
                }
                break;
            case GEN8_OP_CMP: {
                bool c;
                if (!compare(cond, a, b, &c)) { snprintf(t->error, sizeof t->error, "%u: cmp cond", pc); return false; }
                new_flags = (uint16_t)((new_flags & ~(1u << e)) | (c ? 1u << e : 0));
                /* true is all ones in the execution type: for a float compare that
                 * is a NaN, which an integer destination receives as 0 */
                r.raw = true;
                r.bits = c ? 0xffffffffu : 0;
                if (c && fl && dtype != GEN8_TYPE_F) r.bits = 0;
                break;
            }
            case GEN8_OP_MATH:
                r.is_float = true;
                switch (cond) {
                case GEN8_MATH_INV: r.f = (float)(1.0 / af); break;
                case GEN8_MATH_LOG: r.f = (float)log2(af); break;
                case GEN8_MATH_EXP: r.f = (float)exp2(af); break;
                case GEN8_MATH_SQRT: r.f = (float)sqrt(af); break;
                default: snprintf(t->error, sizeof t->error, "%u: math function %u", pc, cond); return false;
                }
                break;
            case GEN8_OP_PLN: {
                Operand u = s1, v = s1;
                v.reg += 2;
                Value pu, pv, k0 = a, k1, k3;
                Operand q = s0;
                q.subreg += 4;
                if (!load(t, &u, e, &pu) || !load(t, &v, e, &pv) || !load(t, &q, 0, &k1)) goto fail;
                q.subreg += 8;
                if (!load(t, &q, 0, &k3)) goto fail;
                r.is_float = true;
                r.f = (float)(k0.f * pu.f + k1.f * pv.f + k3.f);
                break;
            }
            default:
                snprintf(t->error, sizeof t->error, "%u: opcode %02x not modelled", pc, opcode);
                return false;
            }
            if (cond && opcode != GEN8_OP_CMP && opcode != GEN8_OP_SEL && opcode != GEN8_OP_MATH) {
                Value zero = { r.is_float, 0, 0 };
                bool c;
                if (!compare(cond, r, zero, &c)) { snprintf(t->error, sizeof t->error, "%u: cond", pc); return false; }
                new_flags = (uint16_t)((new_flags & ~(1u << e)) | (c ? 1u << e : 0));
            }
            results[e] = r;
            writes[e] = true;
        }
        /* all sources are read before the destination is written */
        for (unsigned e = 0; e < exec; e++)
            if (writes[e] && !store(t, dfile, dreg, dsub, dhs ? dhs : 1, dtype, e, results[e], saturate)) goto fail;
        if (opcode == GEN8_OP_CMP || (cond && opcode != GEN8_OP_SEL && opcode != GEN8_OP_MATH)) t->flag[flag_sub] = new_flags;
        if (gen8_sim_trace) {
            uint32_t first = 0;
            int lane = gen8_sim_trace - 1;
            if (dfile == GEN8_GRF) memcpy(&first, &t->grf[0][0] + dreg * 32 + dsub + lane * dhs * type_size(dtype), 4);
            printf("  %3u op %02x exec %2u dst g%u.%u type %u lane %d = %08x (%d) flags %04x %04x\n", pc, opcode, exec,
                   dreg, dsub, dtype, lane, first, (int)first, t->flag[0], t->flag[1]);
        }
        continue;
    fail:
        if (!t->error[0]) snprintf(t->error, sizeof t->error, "%u: operand", pc);
        return false;
    }
    snprintf(t->error, sizeof t->error, "ran off the end without EOT");
    return false;
}
