/*
 * Voodoo2 GPU back end against the CPU renderer; see voodoo2_gpu_test.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voodoo2_gpu_test.h"
#include "voodoo/voodoo_port.h"
#include <86box/vid_voodoo_common.h>
#include <86box/vid_voodoo_regs.h>
#include <86box/vid_voodoo_render.h>

#define BAR 0xe0000000u
#define TEX (BAR + 0x800000u)

static Voodoo2State *state;
static I440FXState *bridge;
static int voodoo_devfn;
static bool write_failed;

static void config_write(unsigned address, uint32_t value, int size_log2)
{
    i440fx_write_addr(bridge, 0, 0x80000000u | ((uint32_t)voodoo_devfn << 8) | (address & ~3u), 2);
    i440fx_write_data(bridge, address & 3u, value, size_log2);
}

static void reg(uint32_t offset, uint32_t value)
{
    if (!voodoo2_write(state, BAR + offset, 4, value)) write_failed = true;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
static int rnd_range(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

Voodoo2State *voodoo2_gpu_test_board(void)
{
    PCIBus *bus;
    int piix3_devfn;
    bridge = i440fx_init(&bus, &piix3_devfn);
    if (!bridge) return NULL;
    state = voodoo2_init(bus, 12);
    if (!state) return NULL;
    voodoo_devfn = pci_device_get_devfn(voodoo2_pci_device(state));
    config_write(0x10, BAR, 2);
    config_write(4, PCI_COMMAND_MEMORY, 1);
    config_write(0x40, 1, 0);
    reg(SST_fbiInit1, 10u << 4);
    reg(SST_fbiInit2, 150u << 11);
    reg(SST_videoDimensions, 639u | (480u << 16));
    reg(SST_fbiInit0, 0x1c10 | FBIINIT0_GRAPHICS_RESET);
    reg(SST_fbiInit0, 0x1c10);
    reg(SST_clipLeftRight, 640);
    reg(SST_clipLowYHighY, 480);
    for (unsigned tmu = 0; tmu < 2; tmu++)
        for (unsigned t = 0; t < 256; t++)
            for (unsigned s = 0; s < 256; s += 2) {
                uint32_t a = (s * 0x0841u + t * 0x1003u + tmu * 0x5555u) & 0xffff, b = (a * 7u + 0x3cf1u) & 0xffff;
                if (!voodoo2_write(state, TEX + tmu * 0x200000u + (t << 9) + (s << 1), 4, a | (b << 16)))
                    write_failed = true;
            }
    /* the reference is the C renderer: the recompiler approximates fog (it
     * halves the colour difference before the multiply) */
    voodoo2_set_jit(state, false);
    return write_failed ? NULL : state;
}

typedef struct {
    const char *name;
    uint32_t fbzMode, fbzColorPath, alphaMode, fogMode, textureMode0, textureMode1, tLOD;
    bool perspective, wbuffer;
    double tolerance;      /* fraction of pixels allowed to differ */
} Mode;

#define DEPTH (FBZ_DEPTH_ENABLE | FBZ_DEPTH_WMASK | (DEPTHOP_LESSTHAN << 5))
#define RGB FBZ_RGB_WMASK
#define TEX565 (TEX_R5G6B5 << 8)
#define TEX4444 (TEX_ARGB4444 << 8)
#define BILINEAR ((1u << 1) | (1u << 2))
/* fbzColorPath: rgb_sel tex, cc_mselect clocal, reverse: tex * iterated */
#define MODULATE (FBZCP_TEXTURE_ENABLED | 1u | (CC_MSELECT_CLOCAL << 10) | (1u << 13))
#define DECAL (FBZCP_TEXTURE_ENABLED | 1u)
/* alpha from the texture, too */
#define MODULATE_A (MODULATE | (1u << 2) | (CCA_MSELECT_ALOCAL << 19) | (1u << 22))

static const Mode modes[] = {
    { "flat Gouraud", RGB | FBZ_DRAW_BACK, 0, 0, 0, 0, 0, 0, false, false, 0 },
    { "Gouraud, depth, dither 4x4", RGB | DEPTH | FBZ_DITHER | FBZ_DRAW_BACK, 0, 0, 0, 0, 0, 0, false, false, 0 },
    { "Gouraud, depth >=, dither 2x2, clipped, y up", RGB | FBZ_DEPTH_ENABLE | FBZ_DEPTH_WMASK | (DEPTHOP_GREATERTHANEQUAL << 5) | FBZ_DITHER | FBZ_DITHER_2x2 | 1u | (1u << 17) | FBZ_DRAW_BACK, 0, 0, 0, 0, 0, 0, false, false, 0 },
    { "constant colour1, alpha blend src/1-src", RGB | FBZ_DRAW_BACK, 2u | (2u << 2), (1u << 4) | (AFUNC_ASRC_ALPHA << 8) | (AFUNC_AOMSRC_ALPHA << 12), 0, 0, 0, 0, false, false, 0 },
    { "Gouraud, additive blend, dither sub", RGB | FBZ_DITHER | FBZ_DITHER_SUB | FBZ_DRAW_BACK, 0, (1u << 4) | (AFUNC_AONE << 8) | (AFUNC_AONE << 12), 0, 0, 0, 0, false, false, 0 },
    { "decal texture, point", RGB | DEPTH | FBZ_DRAW_BACK, DECAL, 0, 0, TEXTUREMODE_LOCAL | TEX565, 0, 0, false, false, 0.001 },
    { "modulate, bilinear, dither", RGB | DEPTH | FBZ_DITHER | FBZ_DRAW_BACK, MODULATE, 0, 0, TEXTUREMODE_LOCAL | TEX565 | BILINEAR, 0, 0, false, false, 0.001 },
    { "modulate, clamp s/t, mirror", RGB | FBZ_DRAW_BACK, MODULATE, 0, 0, TEXTUREMODE_LOCAL | TEX565 | TEXTUREMODE_TCLAMPS | TEXTUREMODE_TCLAMPT, 0, LOD_TMIRROR_S, false, false, 0.001 },
    { "texture alpha, alpha test >, chroma key", RGB | FBZ_CHROMAKEY | FBZ_DRAW_BACK, MODULATE_A, 1u | (AFUNC_GREATERTHAN << 1) | (0x40u << 24), 0, TEXTUREMODE_LOCAL | TEX4444, 0, 0, false, false, 0.001 },
    { "texture alpha blend dst colour", RGB | FBZ_DRAW_BACK, MODULATE_A, (1u << 4) | (AFUNC_A_COLOR << 8) | (AFUNC_AOMSRC_ALPHA << 12), 0, TEXTUREMODE_LOCAL | TEX4444 | BILINEAR, 0, 0, false, false, 0.001 },
    { "W buffer, table fog", RGB | FBZ_DEPTH_ENABLE | FBZ_DEPTH_WMASK | FBZ_W_BUFFER | (DEPTHOP_LESSTHANEQUAL << 5) | FBZ_DRAW_BACK, 0, 0, FOG_ENABLE, 0, 0, 0, false, true, 0.002 },
    { "Z fog, fog add, depth bias", RGB | DEPTH | FBZ_DEPTH_BIAS | FBZ_DRAW_BACK, 0, 0, FOG_ENABLE | FOG_Z | FOG_ADD, 0, 0, 0, false, false, 0 },
    { "alpha fog mult, constant fog", RGB | FBZ_DRAW_BACK, 0, 0, FOG_ENABLE | FOG_ALPHA | FOG_MULT, 0, 0, 0, false, false, 0 },
    { "depth source <, depth write", RGB | FBZ_DEPTH_ENABLE | FBZ_DEPTH_WMASK | (DEPTHOP_LESSTHAN << 5) | FBZ_DEPTH_SOURCE | FBZ_DRAW_BACK, 0, 0, 0, 0, 0, 0, false, false, 0 },
    { "alpha planes write, dest alpha blend", RGB | FBZ_ALPHA_ENABLE | FBZ_DEPTH_WMASK | FBZ_DRAW_BACK, 2u << 2, (1u << 4) | (AFUNC_ADST_ALPHA << 8) | (AFUNC_AOMDST_ALPHA << 12) | (4u << 16), 0, 0, 0, 0, false, false, 0 },
    { "perspective modulate, point, mipmaps", RGB | DEPTH | FBZ_DRAW_BACK, MODULATE, 0, 0, TEXTUREMODE_LOCAL | TEX565 | TEXTUREMODE_TPERSP_ST, 0, (8u << 6), true, false, 0.02 },
    { "perspective modulate, bilinear, mipmaps, W fog", RGB | DEPTH | FBZ_DITHER | FBZ_DRAW_BACK, MODULATE, 0, FOG_ENABLE, TEXTUREMODE_LOCAL | TEX565 | TEXTUREMODE_TPERSP_ST | BILINEAR, 0, (8u << 6), true, true, 0.03 },
    { "two units: TMU1 x TMU0", RGB | DEPTH | FBZ_DRAW_BACK, DECAL,
      0, 0, TEX565 | (TC_MSELECT_CLOCAL << 14) | (1u << 17) | (TCA_MSELECT_CLOCAL << 23) | (1u << 26), TEXTUREMODE_LOCAL | TEX565, 0, false, false, 0.001 },
    { "two units: detail and lod fraction", RGB | FBZ_DRAW_BACK, DECAL,
      0, 0, TEX565 | (1u << 13) | (TC_MSELECT_DETAIL << 14) | (1u << 18) | (TCA_MSELECT_LOD_FRAC << 23) | (1u << 27), TEXTUREMODE_LOCAL | TEX565, (8u << 6), false, false, 0.001 },
    { "pass-through TMU1, perspective", RGB | DEPTH | FBZ_DRAW_BACK, DECAL, 0, 0, TEX565 | TEXTUREMODE_TPERSP_ST, TEXTUREMODE_LOCAL | TEX565 | TEXTUREMODE_TPERSP_ST | BILINEAR, (8u << 6), true, false, 0.03 },
};

typedef struct {
    int32_t ax, ay, bx, by, cx, cy;
    uint32_t sign;
    uint32_t start[5];
    int32_t ddx[5], ddy[5];
    int32_t s, t, w, dsdx, dsdy, dtdx, dtdy, dwdx, dwdy;
    uint32_t color0, color1, zacolor, chroma, fogcolor;
} Tri;

static Tri random_tri(const Mode *m)
{
    Tri tri;
    memset(&tri, 0, sizeof tri);
    int x[3], y[3];
    for (int i = 0; i < 3; i++) {
        x[i] = rnd_range(-40 * 16, 680 * 16);
        y[i] = rnd_range(-40 * 16, 520 * 16);
    }
    if (rnd() % 3 == 0) {      /* small ones too */
        for (int i = 1; i < 3; i++) {
            x[i] = x[0] + rnd_range(-60 * 16, 60 * 16);
            y[i] = y[0] + rnd_range(-60 * 16, 60 * 16);
        }
    }
    /* unclipped modes stay on the screen: beyond it the core writes into the
     * neighbouring rows, which the GPU leaves to the CPU (tested separately) */
    if (!(m->fbzMode & 1))
        for (int i = 0; i < 3; i++) {
            x[i] = x[i] < 0 ? 0 : x[i] > 638 * 16 ? 638 * 16 : x[i];
            y[i] = y[i] < 0 ? 0 : y[i] > 478 * 16 ? 478 * 16 : y[i];
        }
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (y[j] < y[i]) { int t = y[i]; y[i] = y[j]; y[j] = t; t = x[i]; x[i] = x[j]; x[j] = t; }
    tri.ax = x[0]; tri.ay = y[0]; tri.bx = x[1]; tri.by = y[1]; tri.cx = x[2]; tri.cy = y[2];
    /* B right of the edge A-C: drawn left to right (sign 0) */
    double area = (double)(x[1] - x[0]) * (y[2] - y[0]) - (double)(x[2] - x[0]) * (y[1] - y[0]);
    tri.sign = area > 0 ? 0 : 0x80000000u;
    for (int p = 0; p < 4; p++) {
        tri.start[p] = (uint32_t)rnd_range(0, 255) << 12 | (rnd() & 0xfff);
        tri.ddx[p] = rnd_range(-3 << 12, 3 << 12);
        tri.ddy[p] = rnd_range(-3 << 12, 3 << 12);
    }
    tri.start[4] = (uint32_t)rnd_range(0x1000, 0xf000) << 12 | (rnd() & 0xfff);
    tri.ddx[4] = rnd_range(-0x40 << 12, 0x40 << 12);
    tri.ddy[4] = rnd_range(-0x40 << 12, 0x40 << 12);
    if (m->perspective || m->wbuffer) {
        /* W (2.30) from 0.3 to 1 over the screen, s and t carry W */
        double w0 = 0.3 + (rnd() % 1000) / 1500.0;
        double wdx = ((int)(rnd() % 2001) - 1000) / 2.5e6, wdy = ((int)(rnd() % 2001) - 1000) / 2.5e6;
        double u0 = rnd_range(-300, 300), v0 = rnd_range(-300, 300);
        double udx = ((int)(rnd() % 2001) - 1000) / 400.0, vdy = ((int)(rnd() % 2001) - 1000) / 400.0;
        tri.w = (int32_t)(w0 * (1 << 30));
        tri.dwdx = (int32_t)(wdx * (1 << 30));
        tri.dwdy = (int32_t)(wdy * (1 << 30));
        tri.s = (int32_t)(u0 * w0 * (1 << 18));
        tri.t = (int32_t)(v0 * w0 * (1 << 18));
        tri.dsdx = (int32_t)((udx * w0 + u0 * wdx) * (1 << 18));
        tri.dsdy = (int32_t)(u0 * wdy * (1 << 18));
        tri.dtdx = (int32_t)(v0 * wdx * (1 << 18));
        tri.dtdy = (int32_t)((vdy * w0 + v0 * wdy) * (1 << 18));
    } else {
        tri.s = rnd_range(-300, 300) << 18;
        tri.t = rnd_range(-300, 300) << 18;
        tri.dsdx = rnd_range(-(4 << 18), 4 << 18);
        tri.dsdy = rnd_range(-(1 << 18), 1 << 18);
        tri.dtdx = rnd_range(-(1 << 18), 1 << 18);
        tri.dtdy = rnd_range(-(4 << 18), 4 << 18);
        tri.w = 1 << 30;
    }
    tri.color0 = rnd();
    tri.color1 = rnd();
    tri.zacolor = rnd();
    tri.chroma = rnd() & 0xf0f0f0;
    tri.fogcolor = rnd() & 0xffffff;
    return tri;
}

static void draw_tri(const Mode *m, const Tri *t)
{
    reg(SST_fbzMode, m->fbzMode);
    reg(SST_fbzColorPath, m->fbzColorPath);
    reg(SST_alphaMode, m->alphaMode);
    reg(SST_fogMode, m->fogMode);
    reg(SST_textureMode | 0x800, m->textureMode0);
    reg(SST_textureMode | 0x1000, m->textureMode1);
    reg(SST_tLOD | 0x800, m->tLOD);
    reg(SST_tLOD | 0x1000, m->tLOD);
    reg(SST_tDetail | 0x800, (0x80u) | (20u << 8) | (2u << 14));
    reg(SST_texBaseAddr | 0x800, 0);
    reg(SST_texBaseAddr | 0x1000, 0);
    reg(SST_color0, t->color0);
    reg(SST_color1, t->color1);
    reg(SST_zaColor, t->zacolor);
    reg(SST_chromaKey, t->chroma);
    reg(SST_fogColor, t->fogcolor);
    for (int i = 0; i < 32; i++) reg(SST_fogTable00 + i * 4, (uint32_t)(i * 8) | (uint32_t)(i * 8 + 3) << 16 | 0x02000200u);
    reg(SST_vertexAx, (uint32_t)t->ax & 0xffff);
    reg(SST_vertexAy, (uint32_t)t->ay & 0xffff);
    reg(SST_vertexBx, (uint32_t)t->bx & 0xffff);
    reg(SST_vertexBy, (uint32_t)t->by & 0xffff);
    reg(SST_vertexCx, (uint32_t)t->cx & 0xffff);
    reg(SST_vertexCy, (uint32_t)t->cy & 0xffff);
    static const uint32_t starts[5] = { SST_startR, SST_startG, SST_startB, SST_startA, SST_startZ };
    static const uint32_t dxs[5] = { SST_dRdX, SST_dGdX, SST_dBdX, SST_dAdX, SST_dZdX };
    static const uint32_t dys[5] = { SST_dRdY, SST_dGdY, SST_dBdY, SST_dAdY, SST_dZdY };
    for (int p = 0; p < 5; p++) {
        reg(starts[p], t->start[p]);
        reg(dxs[p], (uint32_t)t->ddx[p]);
        reg(dys[p], (uint32_t)t->ddy[p]);
    }
    reg(SST_startS, (uint32_t)t->s);
    reg(SST_startT, (uint32_t)t->t);
    reg(SST_startW, (uint32_t)t->w);
    reg(SST_dSdX, (uint32_t)t->dsdx);
    reg(SST_dSdY, (uint32_t)t->dsdy);
    reg(SST_dTdX, (uint32_t)t->dtdx);
    reg(SST_dTdY, (uint32_t)t->dtdy);
    reg(SST_dWdX, (uint32_t)t->dwdx);
    reg(SST_dWdY, (uint32_t)t->dwdy);
    reg(SST_triangleCMD, t->sign);
}

V2gTestResult voodoo2_gpu_test_run(Voodoo2State *board, V2gState *gpu, unsigned triangles, int only_mode, bool verbose)
{
    V2gTestResult result = { 0, 0, 0, 0, 0 };
    state = board;
    voodoo_t *dev = voodoo2_device(state);
    uint32_t bytes = dev->fb_mask + 1;
    uint8_t *before = malloc(bytes), *cpu = malloc(bytes);
    unsigned count = triangles > 256 ? 256 : triangles;
    Tri *tris = malloc(sizeof(Tri) * 256);
    if (!before || !cpu || !tris) {
        printf("v2test: no memory\n");
        free(before); free(cpu); free(tris);
        return result;
    }
    for (unsigned mi = 0; mi < sizeof modes / sizeof modes[0]; mi++) {
        const Mode *m = &modes[mi];
        if (only_mode >= 0 && only_mode != (int)mi) continue;
        result.modes++;
        /* a noisy frame buffer: colours, depths and alpha planes everywhere */
        for (uint32_t i = 0; i < bytes / 2; i++) ((uint16_t *)dev->fb_mem)[i] = (uint16_t)rnd();
        memcpy(before, dev->fb_mem, bytes);
        for (unsigned i = 0; i < count; i++) tris[i] = random_tri(m);
        voodoo2_set_gpu(state, NULL);
        for (unsigned i = 0; i < count; i++) draw_tri(m, &tris[i]);
        memcpy(cpu, dev->fb_mem, bytes);
        memcpy(dev->fb_mem, before, bytes);
        voodoo2_set_gpu(state, gpu);
        V2gStats s0 = voodoo2_gpu_stats(gpu);
        for (unsigned i = 0; i < count; i++) draw_tri(m, &tris[i]);
        voodoo2_set_gpu(state, NULL);       /* finishes the GPU */
        V2gStats s1 = voodoo2_gpu_stats(gpu);
        const uint8_t *gpu_fb = dev->fb_mem;
        unsigned long long differ = 0, touched = 0, worst = 0, missing = 0, extra = 0, value = 0;
        uint32_t first = 0xffffffffu;
        for (uint32_t i = 0; i < bytes / 2; i++) {
            uint16_t a = ((const uint16_t *)cpu)[i], b = ((const uint16_t *)gpu_fb)[i], o = ((const uint16_t *)before)[i];
            if (a != o || b != o) touched++;
            if (a != b) {
                differ++;
                if (a != o && b == o) missing++;
                else if (a == o && b != o) extra++;
                else value++;
                if (first == 0xffffffffu) first = i;
                int dr = (a >> 11) - (b >> 11), dg = ((a >> 5) & 63) - ((b >> 5) & 63), db = (a & 31) - (b & 31);
                unsigned long long d = (unsigned long long)(abs(dr) + abs(dg) + abs(db));
                if (d > worst) worst = d;
            }
        }
        double fraction = touched ? (double)differ / (double)touched : 0;
        bool pass = !write_failed && touched > 1000 && fraction <= m->tolerance && s1.cpu_triangles == s0.cpu_triangles;
        result.passed += pass;
        result.exact += pass && !differ;
        result.pixels += touched;
        result.differ += differ;
        printf("%-48s %6llu px, %5llu differ (%.4f%%), %llu GPU / %llu CPU tris%s\n", m->name, touched, differ,
               fraction * 100.0, (unsigned long long)(s1.triangles - s0.triangles),
               (unsigned long long)(s1.cpu_triangles - s0.cpu_triangles), pass ? "" : "  <-- FAIL");
        if (s1.cpu_triangles != s0.cpu_triangles) printf("   CPU fallback, reason %u\n", s1.last_fallback);
        if ((verbose || !pass) && first != 0xffffffffu) {
            uint32_t offset = first * 2;
            printf("   missing %llu, extra %llu, other value %llu, worst %llu LSB; first at byte %08x: cpu %04x gpu %04x before %04x\n",
                   missing, extra, value, worst, offset, ((const uint16_t *)cpu)[first], ((const uint16_t *)gpu_fb)[first],
                   ((const uint16_t *)before)[first]);
        }
        if (verbose && differ && differ <= 16) {
            /* which triangles draw each differing pixel, each alone on the CPU */
            uint8_t *gpu_copy = malloc(bytes);
            uint32_t where[16];
            unsigned n = 0;
            if (gpu_copy) {
                memcpy(gpu_copy, gpu_fb, bytes);
                for (uint32_t i = 0; i < bytes / 2 && n < 16; i++)
                    if (((const uint16_t *)cpu)[i] != ((const uint16_t *)gpu_copy)[i]) where[n++] = i;
                for (unsigned k = 0; k < n; k++) {
                    uint32_t off = where[k] * 2;
                    int rel = (int)off - (int)dev->params.draw_offset;
                    printf("   pixel %08x (draw buffer row %d x %d): cpu %04x gpu %04x before %04x; drawn by", off,
                           rel / (int)dev->row_width, (rel % (int)dev->row_width) / 2, ((const uint16_t *)cpu)[where[k]],
                           ((const uint16_t *)gpu_copy)[where[k]], ((const uint16_t *)before)[where[k]]);
                    for (unsigned t = 0; t < count; t++) {
                        memcpy(dev->fb_mem, before, bytes);
                        draw_tri(m, &tris[t]);
                        uint16_t v = ((const uint16_t *)dev->fb_mem)[where[k]];
                        if (v != ((const uint16_t *)before)[where[k]]) printf(" %u:%04x", t, v);
                    }
                    printf("\n");
                }
                memcpy(dev->fb_mem, gpu_copy, bytes);
                free(gpu_copy);
            }
        }
    }
    free(before);
    free(cpu);
    free(tris);
    return result;
}
