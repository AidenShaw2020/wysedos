/*
 * Triangles on the Cherryview GPU's 3D engine; see gpu3d.h.
 *
 * The 3D state follows IGT GPU Tools' gen8_render_copyfunc (lib/rendercopy_gen8.c,
 * MIT; headers in igt/). Pixel shaders are assembled at open by gen8_asm.c:
 *  - Gouraud: four PLNs interpolate the colour (attribute 0) into g113-g120,
 *    which a render-target write sends.
 *  - Texture x colour: two PLNs give s, t (attribute 1) for a SIMD16 sample
 *    into g113-g120, four more the colour into g102-g109, four MULs combine.
 * Attribute 0 arrives in g6-g7, attribute 1 in g8-g9 (dispatch start GRF 6),
 * each component as a plane of four floats.
 */
#include "gpu3d.h"
#ifdef WYSE_BUILD
#include "gen8_asm.h"
#include "igt/gen8_render.h"
#include "../include/efi.h"
#include <stdio.h>
#include <string.h>

extern EFI_BOOT_SERVICES *gBS;
uint64_t wyse_ticks_us(void);

#define GGTT_BASE_INDEX 0x50000u      /* 1.25 GB */
#define GGTT_LIMIT_PAGES 65536u       /* 256 MB of mappings */
#define RING_PAGES 8u
#define RING_BYTES (RING_PAGES * 4096u)
#define BATCH_PAGES 16u
#define STATE_OFFSET (32u * 1024u)    /* in the batch: commands below, state and vertices above */
#define RCS 0x2000u

static struct {
    bool open, broken, awake, suspended;
    uint32_t suspended_ctl;
    volatile uint8_t *mmio;
    volatile uint64_t *ggtt;
    uint32_t next_index;              /* next free GGTT entry */
    volatile uint32_t *ring, *batch, *kernels, *status;
    uint32_t ring_gpu, batch_gpu, kernels_gpu, status_gpu;
    uint32_t ring_tail, seqno;
    uint32_t ring_seqno;              /* stored by the ring after every batch, to status[RING_SEQNO] */
    uint32_t resets, guest_submits, ring_waits;
    uint32_t watch_done;              /* hang watch of queued work (gpu3d_poll) */
    uint64_t watch_since;
    uint32_t kernel_gouraud, kernel_textured;   /* offsets from the kernel page */
} g;

/* The ring is shared: the guest's render channel submits from the main core,
 * the Voodoo2 back end from its render core. */
static volatile int ring_lock;
static void lock(void) { while (__atomic_exchange_n(&ring_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause"); }
static void unlock(void) { __atomic_store_n(&ring_lock, 0, __ATOMIC_RELEASE); }

#define RING_SEQNO 16u                /* dword index in the status page */

static uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)(g.mmio + reg); }
static void wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(g.mmio + reg) = v; }

static uint32_t pci_cfg(unsigned reg)
{
    uint32_t previous, value;
    __asm__ volatile("inl %1, %0" : "=a"(previous) : "Nd"((uint16_t)0xcf8));
    __asm__ volatile("outl %0, %1" :: "a"(0x80001000u | (reg & 0xfc)), "Nd"((uint16_t)0xcf8));
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"((uint16_t)0xcfc));
    __asm__ volatile("outl %0, %1" :: "a"(previous), "Nd"((uint16_t)0xcf8));
    return value;
}

static bool forcewake(uint32_t reg, bool on)
{
    wr(reg, on ? 0x00010001u : 0x00010000u);
    uint64_t until = wyse_ticks_us() + 50000;
    while (wyse_ticks_us() < until)
        if (((rd(reg + 4) & 1u) != 0) == on) return true;
    return false;
}

/* Resuming an idle engine does not reset its ring, fences or GGTT mappings. */
static bool wake_existing(void)
{
    if (!g.open || g.broken) return false;
    if (!g.awake) {
        if (!forcewake(0x1300b0, true) || !forcewake(0x1300b8, true)) {
            forcewake(0x1300b0, false);
            forcewake(0x1300b8, false);
            return false;
        }
        g.awake = true;
    }
    if (g.suspended) {
        /* No reset: the completed ring's head, tail and contents survive. */
        wr(RCS + 0x3c, g.suspended_ctl);
        if (rd(RCS + 0x3c) != g.suspended_ctl) return false;
        g.suspended = false;
    }
    return true;
}

/* "gpu coherent 1" sets private PAT entry 0 (the one GGTT entries use) to
 * snoop, as Linux i915 does on Cherryview (the firmware leaves 0x03), and then
 * skips the flushes of memory the GPU only reads (textures, gpu3d_flush_data).
 * `gpu v2test` passed with no flush at all in that mode, but whether GPU
 * writes invalidate CPU lines is NOT settled: the DOS Glide game's start was
 * unreliable in that session for reasons of its own (it also crashed to real
 * mode with flushes on). Default: off, everything flushed. */
int g_gpu_coherent;

void gpu3d_flush_data(const void *cpu, uint32_t bytes)
{
    if (g_gpu_coherent) {
        __asm__ volatile("mfence" ::: "memory");
        return;
    }
    gpu3d_flush_cpu(cpu, bytes);
}

void gpu3d_flush_cpu(const void *cpu, uint32_t bytes)
{
    const uint8_t *p = (const uint8_t *)((uintptr_t)cpu & ~63ull);
    const uint8_t *end = (const uint8_t *)cpu + bytes;
    for (; p < end; p += 64) __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

uint32_t gpu3d_map(const void *cpu, uint32_t bytes)
{
    uintptr_t first = (uintptr_t)cpu & ~4095ull;
    uint32_t pages = (uint32_t)(((uintptr_t)cpu + bytes - first + 4095) / 4096);
    if (!g.open || !wake_existing() || g.next_index + pages > GGTT_BASE_INDEX + GGTT_LIMIT_PAGES || first >= (1ull << 39)) return 0;
    uint32_t index = g.next_index;
    for (uint32_t i = 0; i < pages; i++) g.ggtt[index + i] = (first + 4096ull * i) | 1u;
    (void)g.ggtt[index + pages - 1];
    wr(0x101008, 1);                  /* GFX_FLSH_CNTL */
    (void)rd(0x101008);
    g.next_index += pages;
    return index * 4096u + (uint32_t)((uintptr_t)cpu & 4095);
}

static void *alloc_pages(uint32_t pages, uint32_t *gpu)
{
    EFI_PHYSICAL_ADDRESS at = 0xffffffffu;
    if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &at))) return NULL;
    memset((void *)(uintptr_t)at, 0, pages * 4096u);
    gpu3d_flush_cpu((void *)(uintptr_t)at, pages * 4096u);
    *gpu = gpu3d_map((void *)(uintptr_t)at, pages * 4096u);
    return (void *)(uintptr_t)at;
}

/* ---- pixel shaders ---- */

static uint32_t emit_kernel(uint32_t *at, const Gen8Inst *code, unsigned count)
{
    uint32_t offset = *at;
    for (unsigned i = 0; i < count; i++)
        for (unsigned d = 0; d < 4; d++) g.kernels[offset / 4 + i * 4 + d] = code[i].dw[d];
    *at = (offset + count * 16 + 63) & ~63u;
    return offset;
}

#define RT_WRITE_SIMD16 0x10031000u   /* render target write, SIMD16, 8 registers, binding table 0 */
#define SAMPLE_SIMD16   0x08840001u   /* sample, SIMD16, 4 registers in, 8 out, binding table 1, sampler 0 */

static void build_kernels(void)
{
    uint32_t at = 0;
    Gen8Inst gouraud[] = {
        gen8_pln(113, 6, 0), gen8_pln(115, 6, 16), gen8_pln(117, 7, 0), gen8_pln(119, 7, 16),
        gen8_send(5, 0, 113, RT_WRITE_SIMD16, 1),
    };
    g.kernel_gouraud = emit_kernel(&at, gouraud, sizeof gouraud / sizeof gouraud[0]);
    Gen8Inst textured[] = {
        gen8_pln(122, 8, 0), gen8_pln(124, 8, 16),
        gen8_send(2, 113, 122, SAMPLE_SIMD16, 0),
        gen8_pln(102, 6, 0), gen8_pln(104, 6, 16), gen8_pln(106, 7, 0), gen8_pln(108, 7, 16),
        gen8_alu(GEN8_OP_MUL, 16, 113, 0, GEN8_TYPE_F, gen8_vec(113), gen8_vec(102), 0, 0),
        gen8_alu(GEN8_OP_MUL, 16, 115, 0, GEN8_TYPE_F, gen8_vec(115), gen8_vec(104), 0, 0),
        gen8_alu(GEN8_OP_MUL, 16, 117, 0, GEN8_TYPE_F, gen8_vec(117), gen8_vec(106), 0, 0),
        gen8_alu(GEN8_OP_MUL, 16, 119, 0, GEN8_TYPE_F, gen8_vec(119), gen8_vec(108), 0, 0),
        gen8_send(5, 0, 113, RT_WRITE_SIMD16, 1),
    };
    g.kernel_textured = emit_kernel(&at, textured, sizeof textured / sizeof textured[0]);
    gpu3d_flush_cpu((const void *)g.kernels, 4096);
}

/* ---- open / close ---- */

/* An empty ring at the ring pages; everything queued is forgotten. */
static bool ring_start(void)
{
    memset((void *)g.ring, 0, RING_BYTES);
    gpu3d_flush_cpu((const void *)g.ring, RING_BYTES);
    wr(RCS + 0x3c, 0);
    wr(RCS + 0x34, 0);
    wr(RCS + 0x30, 0);
    wr(RCS + 0x38, g.ring_gpu);
    wr(RCS + 0x9c, (1u << 8) << 16);  /* MI_MODE: STOP_RING off */
    wr(RCS + 0x3c, (RING_PAGES - 1) << 12 | 1);   /* length, valid */
    g.ring_tail = 0;
    g.suspended = false;
    g.status[RING_SEQNO] = g.ring_seqno;          /* nothing owed */
    gpu3d_flush_cpu((const void *)(g.status + RING_SEQNO), 4);
    g.watch_since = 0;
    return rd(RCS + 0x38) == g.ring_gpu && (rd(RCS + 0x3c) & 1);
}

bool gpu3d_is_open(void) { return g.open && !g.broken; }

bool gpu3d_open(void)
{
    if (g.open) {
        lock();
        bool ready = wake_existing();
        unlock();
        return ready;
    }
    if (pci_cfg(0) != 0x22b08086u || !(pci_cfg(4) & 4)) {
        printf("gpu3d: no Cherryview graphics with bus mastering\n");
        return false;
    }
    uint32_t ggms = (pci_cfg(0x50) >> 8) & 3;
    if (!ggms || (1u << (20 + ggms)) / 8 < GGTT_BASE_INDEX + GGTT_LIMIT_PAGES) {
        printf("gpu3d: GGTT too small\n");
        return false;
    }
    g.mmio = (volatile uint8_t *)(uintptr_t)(((uint64_t)pci_cfg(0x14) << 32 | pci_cfg(0x10)) & ~0xfull);
    g.ggtt = (volatile uint64_t *)(g.mmio + (8u << 20));
    if (!forcewake(0x1300b0, true) || !forcewake(0x1300b8, true)) {
        printf("gpu3d: the GT did not wake\n");
        forcewake(0x1300b0, false);
        forcewake(0x1300b8, false);
        return false;
    }
    if (rd(RCS + 0x29c) & (1u << 15)) {
        printf("gpu3d: execlists are on\n");
        return false;
    }
    if ((rd(RCS + 0x3c) & 1) && ((rd(RCS + 0x34) ^ rd(RCS + 0x30)) & 0x1ffff8u)) {
        printf("gpu3d: the render ring is in use\n");
        return false;
    }
    g.open = true;
    g.broken = false;
    g.awake = true;
    if (!g.ring) {                    /* once: a reopen keeps the pages and their GGTT entries */
        g.next_index = GGTT_BASE_INDEX;
        g.ring = alloc_pages(RING_PAGES, &g.ring_gpu);
        g.batch = alloc_pages(BATCH_PAGES, &g.batch_gpu);
        g.kernels = alloc_pages(1, &g.kernels_gpu);
        g.status = alloc_pages(1, &g.status_gpu);
        if (!g.ring || !g.batch || !g.kernels || !g.status) {
            printf("gpu3d: no memory\n");
            g.ring = NULL;
            g.open = false;
            return false;
        }
        build_kernels();
    }
    if (!ring_start()) {
        printf("gpu3d: the ring registers did not take\n");
        g.broken = true;
        return false;
    }
    printf("gpu3d: render engine taken; ring %08x batch %08x kernels %08x\n", g.ring_gpu, g.batch_gpu, g.kernels_gpu);
    return true;
}

void gpu3d_close(void)
{
    if (!g.open) return;
    /* Diagnostic close must still work on a broken engine. */
    if (!g.awake && (!forcewake(0x1300b0, true) || !forcewake(0x1300b8, true))) return;
    wr(RCS + 0x3c, 0);
    forcewake(0x1300b0, false);
    forcewake(0x1300b8, false);
    g.open = false;                   /* the pages stay: the GPU may have them cached */
    g.awake = false;
    g.suspended = false;
}

/* ---- batches ---- */

typedef struct {
    uint32_t cmd, state;
} Cursor;

static void out(Cursor *c, uint32_t v) { g.batch[c->cmd / 4] = v; c->cmd += 4; }

static uint32_t state(Cursor *c, uint32_t bytes, uint32_t align)
{
    c->state = (c->state + align - 1) & ~(align - 1);
    uint32_t at = c->state;
    for (uint32_t i = 0; i < bytes; i += 4) g.batch[(at + i) / 4] = 0;
    c->state += bytes;
    return at;
}

static void *ptr(uint32_t offset) { return (void *)(uintptr_t)((volatile uint8_t *)g.batch + offset); }

static uint32_t surface(Cursor *c, uint32_t address, uint32_t format, uint32_t width, uint32_t height,
                        uint32_t pitch)
{
    uint32_t at = state(c, sizeof(struct gen8_surface_state), 64);
    struct gen8_surface_state *ss = ptr(at);
    ss->ss0.surface_type = SURFACE_2D;
    ss->ss0.surface_format = format;
    ss->ss0.render_cache_read_write = 1;
    ss->ss0.vertical_alignment = 1;
    ss->ss0.horizontal_alignment = 1;
    ss->ss1.memory_object_control = CHV_MOCS_WB | CHV_MOCS_L3;
    ss->ss8.base_addr = address;
    ss->ss2.height = height - 1;
    ss->ss2.width = width - 1;
    ss->ss3.pitch = pitch - 1;
    ss->ss7.shader_chanel_select_r = 4;
    ss->ss7.shader_chanel_select_g = 5;
    ss->ss7.shader_chanel_select_b = 6;
    ss->ss7.shader_chanel_select_a = 7;
    return at;
}

static void zeros(Cursor *c, unsigned n) { while (n--) out(c, 0); }

static void put_f(uint32_t *at, float f)
{
    union { float f; uint32_t u; } v = { f };
    g.batch[*at / 4] = v.u;
    *at += 4;
}

#define VERTEX_BYTES 32u

static bool build(const G3dTarget *t, const G3dVertex *v, unsigned count, uint32_t seqno, uint32_t *bytes)
{
    Cursor c = { 0, STATE_OFFSET };
    const bool textured = t->texture != 0;
    uint32_t bt = state(&c, 8, 32);
    g.batch[bt / 4] = surface(&c, t->color, SURFACEFORMAT_B5G6R5_UNORM, t->width, t->height, t->pitch);
    g.batch[bt / 4 + 1] = textured ? surface(&c, t->texture, SURFACEFORMAT_B8G8R8A8_UNORM, t->tex_width,
                                             t->tex_height, t->tex_pitch) : 0;
    uint32_t sampler = state(&c, sizeof(struct gen8_sampler_state), 64);
    {
        struct gen8_sampler_state *ss = ptr(sampler);
        ss->ss0.min_filter = t->bilinear ? GEN4_MAPFILTER_LINEAR : GEN4_MAPFILTER_NEAREST;
        ss->ss0.mag_filter = t->bilinear ? GEN4_MAPFILTER_LINEAR : GEN4_MAPFILTER_NEAREST;
        ss->ss3.r_wrap_mode = GEN4_TEXCOORDMODE_WRAP;
        ss->ss3.s_wrap_mode = GEN4_TEXCOORDMODE_WRAP;
        ss->ss3.t_wrap_mode = GEN4_TEXCOORDMODE_WRAP;
    }
    uint32_t vb = state(&c, count * VERTEX_BYTES, 8), at = vb;
    if (c.state > BATCH_PAGES * 4096u - 1024) return false;
    for (unsigned i = 0; i < count; i++) {
        put_f(&at, v[i].x); put_f(&at, v[i].y);
        put_f(&at, v[i].r); put_f(&at, v[i].g); put_f(&at, v[i].b); put_f(&at, v[i].a);
        put_f(&at, v[i].s); put_f(&at, v[i].t);
    }
    uint32_t cc_state = state(&c, sizeof(struct gen6_color_calc_state), 64);
    uint32_t blend = state(&c, sizeof(struct gen8_blend_state), 64);
    {
        struct gen8_blend_state *bs = ptr(blend);
        for (int i = 0; i < 16; i++) {
            bs->bs[i].dest_blend_factor = GEN6_BLENDFACTOR_ZERO;
            bs->bs[i].source_blend_factor = GEN6_BLENDFACTOR_ONE;
            bs->bs[i].color_blend_func = GEN6_BLENDFUNCTION_ADD;
            bs->bs[i].pre_blend_color_clamp = 1;
        }
    }
    uint32_t cc_vp = state(&c, sizeof(struct gen4_cc_viewport), 32);
    ((struct gen4_cc_viewport *)ptr(cc_vp))->min_depth = -1.e35f;
    ((struct gen4_cc_viewport *)ptr(cc_vp))->max_depth = 1.e35f;
    uint32_t sf_vp = state(&c, sizeof(struct gen7_sf_clip_viewport), 64);
    {
        struct gen7_sf_clip_viewport *vp = ptr(sf_vp);
        vp->guardband.xmin = 0; vp->guardband.xmax = 1.0f;
        vp->guardband.ymin = 0; vp->guardband.ymax = 1.0f;
    }
    uint32_t scissor = state(&c, sizeof(struct gen6_scissor_rect), 64);

    /* the batch, its state and the texture are rewritten between draws: drop the GPU's copies */
    out(&c, GEN6_PIPE_CONTROL | (6 - 2));
    out(&c, GEN7_PIPE_CONTROL_CS_STALL | (1u << 1));             /* stall at the pixel scoreboard */
    zeros(&c, 4);
    out(&c, GEN6_PIPE_CONTROL | (6 - 2));
    out(&c, (1u << 11) | GEN6_PIPE_CONTROL_TC_FLUSH | (1u << 4) | (1u << 3) | (1u << 2));  /* I$, tex, VF, const, state */
    zeros(&c, 4);
    out(&c, G4X_PIPELINE_SELECT | PIPELINE_SELECT_3D);
    out(&c, GEN6_3DSTATE_VF_STATISTICS | 1);
    out(&c, GEN4_STATE_SIP | (3 - 2)); zeros(&c, 2);
    out(&c, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_VS); out(&c, 0);
    out(&c, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_HS); out(&c, 0);
    out(&c, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_DS); out(&c, 0);
    out(&c, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_GS); out(&c, 0);
    out(&c, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_PS); out(&c, 0);
    out(&c, GEN4_STATE_BASE_ADDRESS | (16 - 2));
    out(&c, 1); out(&c, 0);                          /* general */
    out(&c, 1);                                      /* stateless data port */
    out(&c, g.batch_gpu | 1); out(&c, 0);            /* surface */
    out(&c, g.batch_gpu | 1); out(&c, 0);            /* dynamic */
    out(&c, 0); out(&c, 0);                          /* indirect */
    out(&c, g.kernels_gpu | 1); out(&c, 0);          /* instruction */
    out(&c, 0xfffff000u | 1); out(&c, BATCH_PAGES << 12 | 1);
    out(&c, 0xfffff000u | 1); out(&c, 1u << 12 | 1);
    out(&c, GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_CC); out(&c, cc_vp);
    out(&c, GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP); out(&c, sf_vp);
    out(&c, GEN7_3DSTATE_URB_VS); out(&c, 64 | ((2 - 1) << 16) | (2u << 25));
    out(&c, GEN7_3DSTATE_URB_GS); out(&c, 2u << 25);
    out(&c, GEN7_3DSTATE_URB_HS); out(&c, 2u << 25);
    out(&c, GEN7_3DSTATE_URB_DS); out(&c, 2u << 25);
    out(&c, GEN7_3DSTATE_BLEND_STATE_POINTERS); out(&c, blend | 1);
    out(&c, GEN6_3DSTATE_CC_STATE_POINTERS); out(&c, cc_state | 1);
    out(&c, GEN8_3DSTATE_MULTISAMPLE); out(&c, 0);
    out(&c, GEN6_3DSTATE_SAMPLE_MASK); out(&c, 1);
    out(&c, GEN8_3DSTATE_WM_HZ_OP | (5 - 2)); zeros(&c, 4);
    out(&c, GEN7_3DSTATE_CONSTANT_HS | (11 - 2)); zeros(&c, 10);
    out(&c, GEN7_3DSTATE_HS | (9 - 2)); zeros(&c, 8);
    out(&c, GEN7_3DSTATE_BINDING_TABLE_POINTERS_HS); out(&c, 0);
    out(&c, GEN8_3DSTATE_SAMPLER_STATE_POINTERS_HS); out(&c, 0);
    out(&c, GEN7_3DSTATE_TE | (4 - 2)); zeros(&c, 3);
    out(&c, GEN6_3DSTATE_CONSTANT_GS | (11 - 2)); zeros(&c, 10);
    out(&c, GEN6_3DSTATE_GS | (10 - 2)); zeros(&c, 9);
    out(&c, GEN7_3DSTATE_BINDING_TABLE_POINTERS_GS); out(&c, 0);
    out(&c, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_GS); out(&c, 0);
    out(&c, GEN7_3DSTATE_CONSTANT_DS | (11 - 2)); zeros(&c, 10);
    out(&c, GEN7_3DSTATE_DS | (9 - 2)); zeros(&c, 8);
    out(&c, GEN7_3DSTATE_BINDING_TABLE_POINTERS_DS); out(&c, 0);
    out(&c, GEN8_3DSTATE_SAMPLER_STATE_POINTERS_DS); out(&c, 0);
    out(&c, GEN7_3DSTATE_BINDING_TABLE_POINTERS_VS); out(&c, 0);
    out(&c, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_VS); out(&c, 0);
    out(&c, GEN6_3DSTATE_CONSTANT_VS | (11 - 2)); zeros(&c, 10);
    out(&c, GEN6_3DSTATE_VS | (9 - 2)); zeros(&c, 8);
    out(&c, GEN7_3DSTATE_STREAMOUT | (5 - 2)); zeros(&c, 4);
    out(&c, GEN6_3DSTATE_CLIP | (4 - 2)); out(&c, 1u << 10); zeros(&c, 2);
    out(&c, GEN7_3DSTATE_SBE | (4 - 2));
    out(&c, 2u << GEN7_SBE_NUM_OUTPUTS_SHIFT | GEN8_SBE_FORCE_URB_ENTRY_READ_LENGTH |
            GEN8_SBE_FORCE_URB_ENTRY_READ_OFFSET | 1u << GEN7_SBE_URB_ENTRY_READ_LENGTH_SHIFT |
            1u << GEN8_SBE_URB_ENTRY_READ_OFFSET_SHIFT);
    zeros(&c, 2);
    out(&c, GEN8_3DSTATE_SBE_SWIZ | (11 - 2)); zeros(&c, 10);
    out(&c, GEN8_3DSTATE_RASTER | (5 - 2));
    out(&c, GEN8_RASTER_FRONT_WINDING_CCW | GEN8_RASTER_CULL_NONE); zeros(&c, 3);
    out(&c, GEN6_3DSTATE_SF | (4 - 2)); zeros(&c, 3);
    out(&c, GEN7_3DSTATE_BINDING_TABLE_POINTERS_PS); out(&c, bt);
    out(&c, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_PS); out(&c, sampler);
    out(&c, GEN6_3DSTATE_WM | (2 - 2));
    out(&c, GEN8_3DSTATE_PS_PERSPECTIVE_PIXEL_BARYCENTRIC | GEN7_WM_STATISTICS_ENABLE);
    out(&c, GEN6_3DSTATE_CONSTANT_PS | (11 - 2)); zeros(&c, 10);
    out(&c, GEN7_3DSTATE_PS | (12 - 2));
    out(&c, textured ? g.kernel_textured : g.kernel_gouraud); out(&c, 0);
    out(&c, (textured ? 1u : 0u) << GEN6_3DSTATE_WM_SAMPLER_COUNT_SHIFT |
            2u << GEN6_3DSTATE_WM_BINDING_TABLE_ENTRY_COUNT_SHIFT);
    zeros(&c, 2);
    out(&c, (63u - 1) << GEN8_3DSTATE_PS_MAX_THREADS_SHIFT | GEN6_3DSTATE_WM_16_DISPATCH_ENABLE);
    out(&c, 6u << GEN6_3DSTATE_WM_DISPATCH_START_GRF_0_SHIFT);
    zeros(&c, 4);
    out(&c, GEN8_3DSTATE_PS_BLEND | (2 - 2)); out(&c, GEN8_PS_BLEND_HAS_WRITEABLE_RT);
    out(&c, GEN8_3DSTATE_PS_EXTRA | (2 - 2)); out(&c, GEN8_PSX_PIXEL_SHADER_VALID | GEN8_PSX_ATTRIBUTE_ENABLE);
    out(&c, GEN8_3DSTATE_SCISSOR_STATE_POINTERS); out(&c, scissor);
    out(&c, GEN8_3DSTATE_WM_DEPTH_STENCIL | (3 - 2)); zeros(&c, 2);
    out(&c, GEN7_3DSTATE_DEPTH_BUFFER | (8 - 2)); zeros(&c, 7);
    out(&c, GEN8_3DSTATE_HIER_DEPTH_BUFFER | (5 - 2)); zeros(&c, 4);
    out(&c, GEN8_3DSTATE_STENCIL_BUFFER | (5 - 2)); zeros(&c, 4);
    out(&c, GEN7_3DSTATE_CLEAR_PARAMS | (3 - 2)); out(&c, 0); out(&c, 1);
    out(&c, GEN4_3DSTATE_DRAWING_RECTANGLE | (4 - 2)); out(&c, 0);
    out(&c, (t->height - 1) << 16 | (t->width - 1)); out(&c, 0);
    out(&c, GEN4_3DSTATE_VERTEX_BUFFERS | (1 + 4 - 2));
    out(&c, 0u << GEN6_VB0_BUFFER_INDEX_SHIFT | GEN8_VB0_BUFFER_ADDR_MOD_EN | VERTEX_BYTES << VB0_BUFFER_PITCH_SHIFT);
    out(&c, g.batch_gpu + vb); out(&c, 0);
    out(&c, count * VERTEX_BYTES);
    /* VUE: header (zeros), position (x, y, 0, 1), attribute 0 colour, attribute 1 (s, t, 0, 1) */
    out(&c, GEN4_3DSTATE_VERTEX_ELEMENTS | (4 * 2 + 1 - 2));
    out(&c, GEN6_VE0_VALID | SURFACEFORMAT_R32G32B32A32_FLOAT << VE0_FORMAT_SHIFT | 0u << VE0_OFFSET_SHIFT);
    out(&c, GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_0_SHIFT | GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_1_SHIFT |
            GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT | GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT);
    out(&c, GEN6_VE0_VALID | SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT | 0u << VE0_OFFSET_SHIFT);
    out(&c, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT | GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
            GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT | GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);
    out(&c, GEN6_VE0_VALID | SURFACEFORMAT_R32G32B32A32_FLOAT << VE0_FORMAT_SHIFT | 8u << VE0_OFFSET_SHIFT);
    out(&c, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT | GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
            GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_2_SHIFT | GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_3_SHIFT);
    out(&c, GEN6_VE0_VALID | SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT | 24u << VE0_OFFSET_SHIFT);
    out(&c, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT | GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
            GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT | GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);
    out(&c, GEN8_3DSTATE_VF_TOPOLOGY); out(&c, _3DPRIM_TRILIST);
    out(&c, GEN8_3DSTATE_VF_INSTANCING | (3 - 2)); zeros(&c, 2);
    out(&c, GEN4_3DPRIMITIVE | (7 - 2)); out(&c, 0); out(&c, count); out(&c, 0); out(&c, 1); out(&c, 0); out(&c, 0);
    out(&c, GEN6_PIPE_CONTROL | (6 - 2));
    out(&c, GEN7_PIPE_CONTROL_CS_STALL | GEN6_PIPE_CONTROL_WC_FLUSH | (1u << 5));
    zeros(&c, 4);
    out(&c, (0x20u << 23) | (1u << 22) | 2);     /* MI_STORE_DATA_IMM, GGTT */
    out(&c, g.status_gpu); out(&c, 0); out(&c, seqno);
    out(&c, 0x0au << 23);                         /* MI_BATCH_BUFFER_END */
    if (c.cmd & 7) out(&c, 0);
    if (c.cmd > STATE_OFFSET) return false;
    *bytes = c.state;
    return true;
}

uint32_t gpu3d_read(uint32_t reg) { return gpu3d_open() ? rd(reg) : 0xffffffffu; }

void gpu3d_set_coherent(bool on)
{
    if (!gpu3d_open()) return;
    /* GEN8_PRIVATE_PAT_LO entry 0: CHV_PPAT_SNOOP (bit 6), or the firmware's 0x03 */
    wr(0x40e0, (rd(0x40e0) & ~0xffu) | (on ? 0x40u : 0x03u));
    g_gpu_coherent = on && (rd(0x40e0) & 0xff) == 0x40;
}
void gpu3d_write(uint32_t reg, uint32_t value) { if (gpu3d_open()) wr(reg, value); }

void *gpu3d_alloc(uint32_t pages, uint32_t *gpu)
{
    if (!gpu3d_is_open()) return NULL;
    return alloc_pages(pages, gpu);
}

uint32_t gpu3d_status_gpu(void) { return g.status_gpu; }
uint32_t gpu3d_next_seqno(void) { return ++g.seqno; }

static bool run_batch(uint32_t batch_gpu, uint32_t seqno, uint32_t timeout_us);
static bool ring_start(void);

bool gpu3d_execute(uint32_t batch_gpu, uint32_t seqno, uint32_t timeout_us)
{
    if (!gpu3d_is_open()) return false;
    return run_batch(batch_gpu, seqno, timeout_us);
}

bool gpu3d_draw(const G3dTarget *target, const G3dVertex *vertices, unsigned count)
{
    if (!gpu3d_is_open() || !count || count % 3) return false;
    uint32_t seqno = ++g.seqno, used = 0;
    if (!build(target, vertices, count, seqno, &used)) return false;
    gpu3d_flush_cpu((const void *)g.batch, used);
    return run_batch(g.batch_gpu, seqno, 200000);
}

/* ---- the ring ---- */

static uint32_t ring_free(void)
{
    uint32_t head = rd(RCS + 0x34) & 0x1ffffcu;   /* RING_HEAD offset */
    return (head - g.ring_tail - 8u) & (RING_BYTES - 1);   /* 8 bytes apart: equal means empty */
}

static bool ring_wait(uint32_t bytes, uint32_t timeout_us)
{
    if (ring_free() >= bytes) return true;
    g.ring_waits++;
    uint64_t t0 = wyse_ticks_us();
    while (wyse_ticks_us() - t0 < timeout_us)
        if (ring_free() >= bytes) return true;
    return false;
}

/* Queue dwords (a multiple of two) and move the tail; under the lock. */
static bool ring_emit(const uint32_t *dw, uint32_t n, uint32_t timeout_us)
{
    uint32_t bytes = n * 4;
    if (g.ring_tail + bytes > RING_BYTES) {       /* no command crosses the end: pad with MI_NOOP */
        uint32_t pad = RING_BYTES - g.ring_tail;
        if (!ring_wait(pad, timeout_us)) return false;
        memset((void *)(g.ring + g.ring_tail / 4), 0, pad);
        gpu3d_flush_cpu((const void *)(g.ring + g.ring_tail / 4), pad);
        g.ring_tail = 0;
    }
    if (!ring_wait(bytes, timeout_us)) return false;
    for (uint32_t i = 0; i < n; i++) g.ring[g.ring_tail / 4 + i] = dw[i];
    gpu3d_flush_cpu((const void *)(g.ring + g.ring_tail / 4), bytes);
    g.ring_tail = (g.ring_tail + bytes) & (RING_BYTES - 1);
    wr(RCS + 0x30, g.ring_tail);
    return true;
}

static uint32_t ring_done(void)
{
    gpu3d_flush_cpu((const void *)(g.status + RING_SEQNO), 4);
    return g.status[RING_SEQNO];
}

static bool reached(uint32_t done, uint32_t seqno) { return (int32_t)(done - seqno) >= 0; }

/* A hung batch: reset the render domain and start an empty ring, so that the
 * engine is usable again; what was queued is lost. Under the lock. */
static void recover(const char *why)
{
    printf("gpu3d: %s: head %08x tail %08x acthd %08x ipehr %08x instdone %08x - resetting the render engine\n", why,
           rd(RCS + 0x34), rd(RCS + 0x30), rd(RCS + 0x74), rd(RCS + 0x68), rd(RCS + 0x6c));
    wr(RCS + 0x3c, 0);
    wr(0x941c, 1u << 1);                          /* GDRST: the render domain */
    uint64_t t0 = wyse_ticks_us();
    while ((rd(0x941c) & (1u << 1)) && wyse_ticks_us() - t0 < 50000) {}
    g.resets++;
    if ((rd(0x941c) & (1u << 1)) || !ring_start()) {
        printf("gpu3d: the render engine did not come back\n");
        g.broken = true;
    }
}

/* MI_BATCH_BUFFER_START through the ring, a flush, and the ring's own
 * completion dword; then wait for that dword. */
static bool run_batch(uint32_t batch_gpu, uint32_t seqno, uint32_t timeout_us)
{
    (void)seqno;                                  /* the batch's own store is not waited for any more */
    lock();
    if (!wake_existing()) { unlock(); return false; }
    uint32_t mine = g.ring_seqno + 1;
    uint32_t dw[14] = {
        (0x31u << 23) | 1, batch_gpu, 0, 0,       /* MI_BATCH_BUFFER_START (GGTT), MI_NOOP */
        GEN6_PIPE_CONTROL | (6 - 2), GEN7_PIPE_CONTROL_CS_STALL | GEN6_PIPE_CONTROL_WC_FLUSH | (1u << 5) | 1u,
        0, 0, 0, 0,
        (0x20u << 23) | (1u << 22) | 2, g.status_gpu + RING_SEQNO * 4, 0, mine,   /* MI_STORE_DATA_IMM, GGTT */
    };
    bool queued = ring_emit(dw, 14, timeout_us);
    if (queued) g.ring_seqno = mine;
    else recover("the ring did not drain");
    unlock();
    if (!queued) return false;
    uint64_t t0 = wyse_ticks_us();
    while (wyse_ticks_us() - t0 < timeout_us)
        if (reached(ring_done(), mine)) return true;
    lock();
    bool done = reached(ring_done(), mine);
    if (!done) recover("a batch did not finish");
    unlock();
    return done;
}

/* The guest's batch (render channel): read caches invalidated before it;
 * render, depth and data caches flushed after it with a stall, then its fence
 * and the ring's completion dword. Does not wait. */
int gpu3d_submit_guest(uint32_t batch_gpu, uint32_t fence_gpu, uint32_t fence_value)
{
    if (!gpu3d_open()) return 2;
    lock();
    if (!wake_existing()) { unlock(); return 2; }
    uint32_t mine = g.ring_seqno + 1;
    uint32_t dw[24] = {
        GEN6_PIPE_CONTROL | (6 - 2),
        GEN7_PIPE_CONTROL_CS_STALL | (1u << 11) | GEN6_PIPE_CONTROL_TC_FLUSH | (1u << 4) | (1u << 3) | (1u << 2),
        0, 0, 0, 0,                               /* I$, textures, VF, constants, state */
        (0x31u << 23) | 1, batch_gpu, 0, 0,
        GEN6_PIPE_CONTROL | (6 - 2), GEN7_PIPE_CONTROL_CS_STALL | GEN6_PIPE_CONTROL_WC_FLUSH | (1u << 5) | 1u,
        0, 0, 0, 0,                               /* render target, data and depth caches */
        (0x20u << 23) | (1u << 22) | 2, fence_gpu, 0, fence_value,
        (0x20u << 23) | (1u << 22) | 2, g.status_gpu + RING_SEQNO * 4, 0, mine,
    };
    __asm__ volatile("sfence" ::: "memory");      /* the guest's write-combined batch */
    int result = 3;
    if (ring_emit(dw, 24, 100000)) {
        g.ring_seqno = mine;
        g.guest_submits++;
        result = 0;
    }
    unlock();
    return result;
}

/* Notices a hung queue: queued work and no batch completing for 3 s resets
 * the engine. (The command streamer's address is no guide: a batch that
 * loops - jumps back to itself - keeps it moving.) Called whenever the guest
 * asks. */
void gpu3d_poll(void)
{
    if (!g.open || g.broken) return;
    lock();
    uint32_t done = ring_done();
    if (reached(done, g.ring_seqno)) {
        g.watch_since = 0;
    } else {
        uint64_t now = wyse_ticks_us();
        if (!g.watch_since || done != g.watch_done) {
            g.watch_since = now;
            g.watch_done = done;
        } else if (now - g.watch_since > 3000000) {
            recover("queued work hung");
        }
    }
    unlock();
}

bool gpu3d_busy(void) { return g.open && !reached(ring_done(), g.ring_seqno); }

/* Main-core DOS policy; serialize against either ring producer. Never drop
 * forcewake while a fence is pending. Disable the drained ring as well:
 * forcewake release alone did not restore DOS CPU rendering performance.
 * Preserve its registers, contents and allocations for the next request. */
void gpu3d_idle_release(void)
{
    lock();
    if (g.open && !g.broken && g.awake && reached(ring_done(), g.ring_seqno)) {
        /* The completion store can precede fetching the last ring padding.
         * Do not disable until the hardware has consumed the entire ring. */
        if ((rd(RCS + 0x34) & 0x1ffffcu) != g.ring_tail) { unlock(); return; }
        if (!g.suspended) {
            g.suspended_ctl = rd(RCS + 0x3c);
            if (!(g.suspended_ctl & 1u)) { unlock(); return; }
            wr(RCS + 0x3c, 0);
            if (rd(RCS + 0x3c) != 0) { unlock(); return; }
            g.suspended = true;
        }
        bool render = forcewake(0x1300b0, false);
        bool media = forcewake(0x1300b8, false);
        if (render && media) {
            g.awake = false;
            printf("gpu3d: idle DOS ring suspended, forcewake released\n");
        } else {
            g.awake = false;
            (void)wake_existing();
        }
    }
    unlock();
}
uint32_t gpu3d_resets(void) { return g.resets; }

void gpu3d_report(void)
{
    printf("gpu3d: %s, ring seqno %u done %u, tail %05x head %05x, guest submits %u, ring waits %u, resets %u\n",
           !g.open ? "closed" : g.broken ? "BROKEN" : !g.awake ? "asleep" : "open", g.ring_seqno, g.open ? ring_done() : 0, g.ring_tail,
           g.open && g.awake ? rd(RCS + 0x34) & 0x1ffffcu : 0, g.guest_submits, g.ring_waits, g.resets);
}

/* ---- the monitor's test ---- */

static uint16_t rgb565(float r, float g_, float b)
{
    unsigned R = (unsigned)(r * 31.0f + 0.5f), G = (unsigned)(g_ * 63.0f + 0.5f), B = (unsigned)(b * 31.0f + 0.5f);
    return (uint16_t)(R << 11 | G << 5 | B);
}

void gpu3d_test(bool textured)
{
    if (!gpu3d_open()) return;
    static uint16_t *surface_mem;
    static uint32_t *texture_mem;
    static uint32_t surface_gpu, texture_gpu;
    if (!surface_mem) {
        surface_mem = alloc_pages(2, &surface_gpu);           /* 64x64 RGB565, pitch 128 */
        texture_mem = alloc_pages(1, &texture_gpu);           /* 32x32 ARGB, pitch 128 */
        if (!surface_mem || !texture_mem) { printf("gpu3d test: no memory\n"); return; }
    }
    for (unsigned i = 0; i < 64 * 64; i++) surface_mem[i] = 0x1111;
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 32; x++)
            texture_mem[y * 32 + x] = ((x ^ y) & 8) ? 0xffffffffu : 0xff808080u;   /* checkerboard */
    gpu3d_flush_cpu(surface_mem, 8192);
    gpu3d_flush_cpu(texture_mem, 4096);
    G3dTarget t = { surface_gpu, 128, 64, 64, textured ? texture_gpu : 0, 32, 32, 128, false };
    /* red, green and blue corners, the texture once over the triangle's bounding square */
    G3dVertex v[3] = {
        { 0, 0, 1, 0, 0, 1, 0, 0 },
        { 64, 0, 0, 1, 0, 1, 1, 0 },
        { 0, 64, 0, 0, 1, 1, 0, 1 },
    };
    uint64_t t0 = wyse_ticks_us();
    bool ok = gpu3d_draw(&t, v, 3);
    uint64_t took = wyse_ticks_us() - t0;
    gpu3d_flush_cpu(surface_mem, 8192);
    if (!ok) { printf("gpu %s: FAIL (no completion)\n", textured ? "tex" : "tri"); return; }
    unsigned inside = 0, right = 0, outside_kept = 0, outside = 0, worst = 0, wx = 0, wy = 0;
    for (unsigned y = 0; y < 64; y++)
        for (unsigned x = 0; x < 64; x++) {
            float fx = x + 0.5f, fy = y + 0.5f, edge = fx + fy;
            uint16_t got = surface_mem[y * 64 + x];
            if (edge > 64.0f + 1.0f) { outside++; outside_kept += got == 0x1111; continue; }
            if (edge > 64.0f - 1.0f) continue;                   /* the edge itself: either is right */
            inside++;
            float g1 = fx / 64.0f, b1 = fy / 64.0f, r1 = 1.0f - g1 - b1, m = 1.0f;
            if (textured) m = (((x / 2) ^ (y / 2)) & 8) ? 1.0f : 128.0f / 255.0f;
            uint16_t want = rgb565(r1 * m, g1 * m, b1 * m);
            int dr = (got >> 11) - (want >> 11), dg = ((got >> 5) & 63) - ((want >> 5) & 63), db = (got & 31) - (want & 31);
            unsigned err = (unsigned)((dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db));
            if (err <= 3) right++;
            if (err > worst) { worst = err; wx = x; wy = y; }
        }
    bool pass = right == inside && outside_kept == outside;
    printf("gpu %s: %llu us, %u of %u inside pixels right (worst %u LSB at %u,%u: %04x), %u of %u outside kept - %s\n",
           textured ? "tex" : "tri", (unsigned long long)took, right, inside, worst, wx, wy, surface_mem[wy * 64 + wx],
           outside_kept, outside, pass ? "PASS" : "FAIL");
}

#endif
