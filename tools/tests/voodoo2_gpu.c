/*
 * The Voodoo2 GPU back end (pc/voodoo2_gpu.c) against the CPU renderer: the
 * same random triangles in many modes, once drawn by the 86Box core, once by
 * the generated Gen8 pixel shaders on the simulator (gen8_sim.c), and the
 * whole frame buffer compared. Exact where the shader repeats the core's
 * integer arithmetic; perspective textures (float S/T/W, GPU math) within a
 * small fraction of pixels.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../pc/voodoo2.h"
#include "../../pc/voodoo2_gpu.h"
#include "../../pc/voodoo/voodoo_port.h"
#include <86box/vid_voodoo_common.h>
#include <86box/vid_voodoo_regs.h>
#include <86box/vid_voodoo_render.h>
#include "../../pc/voodoo2_gpu_test.h"
#include "gen8_sim.h"

#define ENTRY_ELEMS (256 * 256 + 256 * 256 + 128 * 128 + 64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2)
#define DATA_DWORDS (1u << 20)

static uint64_t test_time_us;

uint64_t time_us_64(void) { return test_time_us; }

#ifdef _WIN32
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, size_t size, unsigned long type, unsigned long protect);
__declspec(dllimport) int __stdcall VirtualFree(void *address, size_t size, unsigned long type);
void *voodoo2_platform_alloc_exec(size_t size) { return VirtualAlloc(NULL, size, 0x3000, 0x40); }
void voodoo2_platform_free_exec(void *pointer, size_t size) { (void)size; VirtualFree(pointer, 0, 0x8000); }
#endif

/* ---- the simulated GPU ---- */

typedef struct {
    voodoo_t *dev;
    uint32_t data[DATA_DWORDS];
    unsigned used;
    const uint8_t *tables;
    unsigned table_bytes;
    const V2gDraw *draw;
    uint64_t threads, draws, written;
    bool failed;
} Host;

static Host host;

static uint32_t *host_alloc(void *ctx, unsigned dwords, unsigned align, uint32_t *index)
{
    Host *h = ctx;
    unsigned at = (h->used + align - 1) / align * align;
    if (at + dwords > DATA_DWORDS) return NULL;
    h->used = at + dwords;
    *index = at;
    return h->data + at;
}

static uint32_t host_tex_index(void *ctx, unsigned tmu, unsigned entry)
{
    (void)ctx;
    return (tmu * TEX_CACHE_MAX + entry) * ENTRY_ELEMS;
}

static void host_finish(void *ctx) { ((Host *)ctx)->used = 0; }

static uint32_t element(Host *h, unsigned bt, int32_t index, uint32_t out[4])
{
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    if (index < 0) return 0;
    uint32_t i = (uint32_t)index;
    switch (bt) {
    case V2G_BT_FB:
        if (i < (h->dev->fb_mask + 1) / 2) out[0] = ((uint16_t *)h->dev->fb_mem)[i];
        break;
    case V2G_BT_TEX: {
        uint32_t entry = i / ENTRY_ELEMS, at = i % ENTRY_ELEMS;
        if (entry < 2 * TEX_CACHE_MAX) out[0] = h->dev->texture_cache[entry / TEX_CACHE_MAX][entry % TEX_CACHE_MAX].data[at];
        break;
    }
    case V2G_BT_TABLES:
        if (i < h->table_bytes) out[0] = h->tables[i];
        break;
    case V2G_BT_DATA:
        if (i < h->used) out[0] = h->data[i];
        break;
    case V2G_BT_DATA4:
        if (i * 4 + 3 < h->used) memcpy(out, h->data + i * 4, 16);
        break;
    default:
        h->failed = true;
        printf("ld from binding table entry %u\n", bt);
    }
    return out[0];
}

static uint32_t reg32(Gen8Thread *t, unsigned reg, unsigned channel)
{
    uint32_t v;
    memcpy(&v, &t->grf[reg + channel / 8][(channel % 8) * 4], 4);
    return v;
}

static void host_send(void *ctx, Gen8Thread *t, unsigned sfid, uint32_t desc, unsigned src, unsigned dst,
                      uint16_t mask)
{
    Host *h = ctx;
    unsigned bt = desc & 0xff;
    if (sfid == GEN8_SFID_SAMPLER) {
        if (((desc >> 12) & 0x1f) != 7 || ((desc >> 17) & 3) != 2 || ((desc >> 25) & 0xf) != 4 ||
            ((desc >> 20) & 0x1f) != 8 || (desc & (1u << 19))) {
            snprintf(t->error, sizeof t->error, "unexpected sampler message %08x", desc);
            return;
        }
        for (unsigned e = 0; e < 16; e++) {
            if (!((mask >> e) & 1)) continue;
            uint32_t out[4];
            element(h, bt, (int32_t)reg32(t, src, e), out);
            for (unsigned c = 0; c < 4; c++) memcpy(&t->grf[dst + 2 * c + e / 8][(e % 8) * 4], &out[c], 4);
        }
        return;
    }
    if (sfid == GEN8_SFID_RENDER) {
        if (((desc >> 14) & 0xf) != 12 || !(desc & (1u << 19)) || ((desc >> 25) & 0xf) != 10 || ((desc >> 8) & 7)) {
            snprintf(t->error, sizeof t->error, "unexpected render message %08x", desc);
            return;
        }
        uint16_t pixels;
        memcpy(&pixels, &t->grf[src + 1][28], 2);
        const V2gDraw *d = h->draw;
        uint32_t base = bt == V2G_BT_COLOR ? d->draw_offset : bt == V2G_BT_AUX ? d->aux_offset : 0xffffffffu;
        if (base == 0xffffffffu) { snprintf(t->error, sizeof t->error, "render target %u", bt); return; }
        for (unsigned e = 0; e < 16; e++) {
            if (!((mask >> e) & (pixels >> e) & 1)) continue;
            uint32_t xy;
            memcpy(&xy, &t->grf[src + 1][8 + (e / 4) * 4], 4);
            unsigned x = (xy & 0xffff) + (e & 1), y = (xy >> 16) + ((e >> 1) & 1);
            if (y >= d->rows || x >= d->row_width / 2) { snprintf(t->error, sizeof t->error, "write outside the target"); return; }
            uint32_t v = reg32(t, src + 2, e);
            uint32_t offset = base + y * d->row_width + x * 2;
            uint16_t px = (uint16_t)v;
            if (bt == V2G_BT_COLOR) h->written++;
            memcpy(h->dev->fb_mem + offset, &px, 2);
        }
        return;
    }
    snprintf(t->error, sizeof t->error, "message to sfid %u", sfid);
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

static bool host_draw(void *ctx, const V2gDraw *d)
{
    Host *h = ctx;
    h->draw = d;
    h->draws++;
    int qx0 = d->x0 & ~1, qy0 = d->y0 & ~1;
    int qw = (d->x1 - qx0) / 2 + 1, qh = (d->y1 - qy0) / 2 + 1;
    int quads = qw * qh;
    for (int first = 0; first < quads; first += 4) {
        Gen8Thread t;
        for (unsigned i = 0; i < sizeof t.grf; i++) (&t.grf[0][0])[i] = (uint8_t)rnd();
        t.flag[0] = (uint16_t)rnd();
        t.flag[1] = (uint16_t)rnd();
        uint16_t mask = 0;
        for (int k = 0; k < 4; k++) {
            int q = first + k;
            uint32_t xy = 0;
            if (q < quads) {
                int qx = qx0 + (q % qw) * 2, qy = qy0 + (q / qw) * 2;
                xy = (uint32_t)qx | (uint32_t)qy << 16;
                for (int j = 0; j < 4; j++) {
                    int x = qx + (j & 1), y = qy + (j >> 1);
                    if (x >= d->x0 && x <= d->x1 && y >= d->y0 && y <= d->y1) mask |= (uint16_t)(1u << (k * 4 + j));
                }
            }
            memcpy(&t.grf[1][8 + k * 4], &xy, 4);
        }
        uint32_t m32 = mask;
        memcpy(&t.grf[1][28], &m32, 4);
        memcpy(&t.grf[6][0], d->record, V2G_RECORD_DWORDS * 4);
        t.exec_mask = mask;
        if (!mask) continue;
        h->threads++;
        static bool traced;
        if (getenv("V2TRACE") && !traced) {
            int tx = 0, ty = 0;
            sscanf(getenv("V2TRACE"), "%d,%d", &tx, &ty);
            for (int k = 0; k < 4; k++) {
                uint32_t xy;
                memcpy(&xy, &t.grf[1][8 + k * 4], 4);
                if ((int)(xy & 0xffff) == (tx & ~1) && (int)(xy >> 16) == (ty & ~1)) {
                    traced = true;
                    gen8_sim_trace = 1 + k * 4 + (tx & 1) + (ty & 1) * 2;
                    printf("trace: subspan %d lane %d\n", k, gen8_sim_trace - 1);
                }
            }
        }
        bool ran = gen8_sim_run(&t, (const Gen8Inst *)d->code, d->code_bytes / 16, host_send, h);
        gen8_sim_trace = 0;
        if (!ran) {
            printf("simulator: %s (kernel %u, %u instructions)\n", t.error, d->key_id, d->code_bytes / 16);
            h->failed = true;
            return true;
        }
    }
    if (getenv("V2DEBUG")) {
        uint64_t expected = 0;
        for (uint32_t y = d->record[2]; (int32_t)y < (int32_t)d->record[3]; y++) {
            uint32_t span = h->data[d->record[4] + y - d->record[2]];
            if ((span & 0xffff) <= (span >> 16)) expected += (span >> 16) - (span & 0xffff) + 1;
        }
        printf("draw %llu: rect %d,%d-%d,%d lines %d..%d, span pixels %llu, colour writes %llu\n",
               (unsigned long long)h->draws, d->x0, d->y0, d->x1, d->y1, (int)d->record[2], (int)d->record[3],
               (unsigned long long)expected, (unsigned long long)h->written);
    }
    h->written = 0;
    return true;
}

int main(int argc, char **argv)
{
    unsigned triangles = argc > 1 ? (unsigned)atoi(argv[1]) : 24;
    Voodoo2State *board = voodoo2_gpu_test_board();
    assert(board);
    host.dev = voodoo2_device(board);
    host.tables = voodoo2_gpu_tables(&host.table_bytes);
    V2gBackend backend = { &host, host_alloc, host_tex_index, host_draw, host_finish, NULL };
    V2gState *gpu = voodoo2_gpu_create(&backend);
    assert(gpu);
    V2gTestResult r = voodoo2_gpu_test_run(board, gpu, triangles, getenv("V2MODE") ? atoi(getenv("V2MODE")) : -1,
                                           getenv("V2DEBUG") != NULL);
    V2gStats st = voodoo2_gpu_stats(gpu);
    printf("%u of %u modes pass (%u exact); simulator threads %llu, kernels %llu (%llu bytes), draws %llu, barriers %llu\n",
           r.passed, r.modes, r.exact, (unsigned long long)host.threads, (unsigned long long)st.kernels,
           (unsigned long long)st.kernel_bytes, (unsigned long long)st.draws, (unsigned long long)st.barriers);
    bool pass = r.passed == r.modes && !host.failed;
    voodoo2_gpu_destroy(gpu);
    voodoo2_free(board);
    puts(pass ? "Voodoo2 Gen8 pixel shaders match the CPU renderer: PASS" : "Voodoo2 Gen8 pixel shaders: FAIL");
    return pass ? 0 : 1;
}
