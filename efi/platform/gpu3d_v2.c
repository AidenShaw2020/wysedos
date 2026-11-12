/*
 * The Voodoo2 GPU back end's GPU half on Cherryview (V2gBackend of
 * pc/voodoo2_gpu.h), on top of gpu3d.c's render engine.
 *
 * One 4 MB batch buffer is filled draw by draw and run when the frontend
 * finishes (or the buffer is full):
 *   [0, 64K)    surface states and binding tables (surface state base)
 *   [64K, 1M)   commands: the 3D state once, then per draw the push
 *               constants, kernel and binding table changes, barriers and a
 *               RECTLIST 3DPRIMITIVE
 *   [1M, 2M)    per-draw records (push constant buffer 3) and vertices (the
 *               one vertex buffer)
 *   [2M, 4M)    per-batch data (spans, LOD and fog tables): V2G_BT_DATA(4)
 * Kernels live in their own 512 KB instruction area, cached by key id.
 * The Voodoo2 frame buffer and all 128 texture-cache entries are entered in
 * the GGTT once per board. Command layouts: IGT rendercopy_gen8 and Mesa's
 * genxml/gen80.xml (3DSTATE_CONSTANT_BODY, PS, PS_EXTRA, buffer surfaces as
 * isl_buffer_fill_state).
 */
#ifdef WYSE_BUILD
#include "gpu3d.h"
#include "gen8_asm.h"
#include "igt/gen8_render.h"
#include "../../pc/voodoo2_gpu.h"
#include "../../pc/voodoo/voodoo_port.h"
#include <86box/vid_voodoo_common.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t wyse_ticks_us(void);

#define BATCH_BYTES (4u << 20)
#define SS_END (64u << 10)
#define CMD_START SS_END
#define CMD_END (1u << 20)
#define DYN_START CMD_END
#define DYN_END (2u << 20)
#define DATA_START DYN_END
#define KERNEL_BYTES (512u << 10)
#define KERNEL_SLOTS 128
#define ENTRY_BYTES ((256 * 256 + 256 * 256 + 128 * 128 + 64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2) * 4)
#define VERTEX_PITCH 8

typedef struct {
    voodoo_t *dev;
    uint8_t *batch;
    uint32_t batch_gpu;
    uint8_t *kernels;
    uint32_t kernels_gpu, kernel_used;
    struct { unsigned id; uint32_t offset; } kcache[KERNEL_SLOTS];
    unsigned kcount;
    uint32_t fb_gpu, fb_bytes;
    uint32_t tex_base_gpu, tex_gpu[2][TEX_CACHE_MAX], tex_elements;
    uint32_t tables_gpu, table_bytes;
    uint32_t cmd, ss, dyn, data;          /* cursors, byte offsets in the batch */
    bool started;
    unsigned kernel_id;
    bool kernel_valid, bt_valid;
    uint32_t bt_draw, bt_aux, bt_row, bt_rows;
    uint32_t written_lo, written_hi;      /* frame-buffer bytes the GPU may have written */
    uint64_t batches, draws, barriers, busy_us, flush_us;
    bool broken;
} V2Hw;

static V2Hw *contexts[4];
/* "gpu v2serial 1": a barrier before every draw (diagnosis) */
int g_gpu_v2_serialize;

static void out(V2Hw *h, uint32_t v) { memcpy(h->batch + h->cmd, &v, 4); h->cmd += 4; }
static void zeros(V2Hw *h, unsigned n) { while (n--) out(h, 0); }

static uint32_t ss_alloc(V2Hw *h, uint32_t bytes, uint32_t align)
{
    h->ss = (h->ss + align - 1) & ~(align - 1);
    uint32_t at = h->ss;
    memset(h->batch + at, 0, bytes);
    h->ss += bytes;
    return at;
}

static uint32_t dyn_alloc(V2Hw *h, uint32_t bytes, uint32_t align)
{
    h->dyn = (h->dyn + align - 1) & ~(align - 1);
    uint32_t at = h->dyn;
    memset(h->batch + at, 0, bytes);
    h->dyn += bytes;
    return at;
}

static void *at(V2Hw *h, uint32_t offset) { return h->batch + offset; }

static void surface_2d(struct gen8_surface_state *ss, uint32_t address, uint32_t format, uint32_t width,
                       uint32_t height, uint32_t pitch)
{
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
}

/* isl_buffer_fill_state (Gen7/8): element count - 1 in width 6:0, height 20:7, depth 30:21 */
static void surface_buffer(struct gen8_surface_state *ss, uint32_t address, uint32_t format, uint32_t elements,
                           uint32_t element_bytes)
{
    uint32_t n = elements - 1;
    ss->ss0.surface_type = SURFACE_BUFFER;
    ss->ss0.surface_format = format;
    ss->ss0.vertical_alignment = 1;
    ss->ss0.horizontal_alignment = 1;
    ss->ss1.memory_object_control = CHV_MOCS_WB | CHV_MOCS_L3;
    ss->ss8.base_addr = address;
    ss->ss2.width = n & 0x7f;
    ss->ss2.height = (n >> 7) & 0x3fff;
    ss->ss3.depth = (n >> 21) & 0x3ff;
    ss->ss3.pitch = element_bytes - 1;
    ss->ss7.shader_chanel_select_r = 4;
    ss->ss7.shader_chanel_select_g = 5;
    ss->ss7.shader_chanel_select_b = 6;
    ss->ss7.shader_chanel_select_a = 7;
}

static uint32_t binding_table(V2Hw *h, const V2gDraw *d)
{
    uint32_t bt = ss_alloc(h, V2G_BT_COUNT * 4, 32);
    uint32_t entries[V2G_BT_COUNT];
    for (unsigned i = 0; i < V2G_BT_COUNT; i++) {
        uint32_t s = ss_alloc(h, sizeof(struct gen8_surface_state), 64);
        struct gen8_surface_state *ss = at(h, s);
        switch (i) {
        case V2G_BT_COLOR:
            surface_2d(ss, h->fb_gpu + d->draw_offset, SURFACEFORMAT_R16_UINT, d->row_width / 2, d->rows, d->row_width);
            break;
        case V2G_BT_AUX:
            surface_2d(ss, h->fb_gpu + d->aux_offset, SURFACEFORMAT_R16_UINT, d->row_width / 2, d->rows, d->row_width);
            break;
        case V2G_BT_FB: surface_buffer(ss, h->fb_gpu, SURFACEFORMAT_R16_UINT, h->fb_bytes / 2, 2); break;
        case V2G_BT_TEX: surface_buffer(ss, h->tex_base_gpu, SURFACEFORMAT_R32_UINT, h->tex_elements, 4); break;
        case V2G_BT_TABLES: surface_buffer(ss, h->tables_gpu, SURFACEFORMAT_R8_UINT, h->table_bytes, 1); break;
        case V2G_BT_DATA:
            surface_buffer(ss, h->batch_gpu + DATA_START, SURFACEFORMAT_R32_UINT, (BATCH_BYTES - DATA_START) / 4, 4);
            break;
        default:
            surface_buffer(ss, h->batch_gpu + DATA_START, SURFACEFORMAT_R32G32B32A32_UINT, (BATCH_BYTES - DATA_START) / 16, 16);
            break;
        }
        entries[i] = s;
    }
    memcpy(at(h, bt), entries, sizeof entries);
    return bt;
}

/* ---- the 3D state of a batch ---- */

static void prologue(V2Hw *h)
{
    uint32_t cc_state = dyn_alloc(h, sizeof(struct gen6_color_calc_state), 64);
    uint32_t blend = dyn_alloc(h, sizeof(struct gen8_blend_state), 64);
    {
        struct gen8_blend_state *bs = at(h, blend);
        for (int i = 0; i < 16; i++) {
            bs->bs[i].dest_blend_factor = GEN6_BLENDFACTOR_ZERO;
            bs->bs[i].source_blend_factor = GEN6_BLENDFACTOR_ONE;
            bs->bs[i].color_blend_func = GEN6_BLENDFUNCTION_ADD;
        }
    }
    uint32_t cc_vp = dyn_alloc(h, sizeof(struct gen4_cc_viewport), 32);
    ((struct gen4_cc_viewport *)at(h, cc_vp))->min_depth = -1.e35f;
    ((struct gen4_cc_viewport *)at(h, cc_vp))->max_depth = 1.e35f;
    uint32_t sf_vp = dyn_alloc(h, sizeof(struct gen7_sf_clip_viewport), 64);
    {
        struct gen7_sf_clip_viewport *vp = at(h, sf_vp);
        vp->guardband.xmin = 0; vp->guardband.xmax = 1.0f;
        vp->guardband.ymin = 0; vp->guardband.ymax = 1.0f;
    }
    uint32_t scissor = dyn_alloc(h, sizeof(struct gen6_scissor_rect), 64);

    out(h, GEN6_PIPE_CONTROL | (6 - 2));
    out(h, GEN7_PIPE_CONTROL_CS_STALL | (1u << 1));
    zeros(h, 4);
    out(h, GEN6_PIPE_CONTROL | (6 - 2));
    out(h, (1u << 11) | GEN6_PIPE_CONTROL_TC_FLUSH | (1u << 4) | (1u << 3) | (1u << 2));
    zeros(h, 4);
    out(h, G4X_PIPELINE_SELECT | PIPELINE_SELECT_3D);
    out(h, GEN6_3DSTATE_VF_STATISTICS | 1);
    out(h, GEN4_STATE_SIP | (3 - 2)); zeros(h, 2);
    /* push constants: 16 KB for the pixel shader, URB from 16 KB as IGT */
    out(h, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_VS); out(h, 0);
    out(h, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_HS); out(h, 0);
    out(h, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_DS); out(h, 0);
    out(h, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_GS); out(h, 0);
    out(h, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_PS); out(h, 16u | 0u << GEN7_PUSH_CONSTANT_BUFFER_OFFSET_SHIFT);
    out(h, GEN4_STATE_BASE_ADDRESS | (16 - 2));
    out(h, 1); out(h, 0);                            /* general */
    out(h, 1);                                       /* stateless data port */
    out(h, h->batch_gpu | 1); out(h, 0);             /* surface */
    out(h, h->batch_gpu | 1); out(h, 0);             /* dynamic */
    out(h, 0); out(h, 0);                            /* indirect */
    out(h, h->kernels_gpu | 1); out(h, 0);           /* instruction */
    out(h, 0xfffff000u | 1); out(h, BATCH_BYTES | 1);
    out(h, 0xfffff000u | 1); out(h, KERNEL_BYTES | 1);
    out(h, GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_CC); out(h, cc_vp);
    out(h, GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP); out(h, sf_vp);
    out(h, GEN7_3DSTATE_URB_VS); out(h, 64 | ((2 - 1) << 16) | (2u << 25));
    out(h, GEN7_3DSTATE_URB_GS); out(h, 2u << 25);
    out(h, GEN7_3DSTATE_URB_HS); out(h, 2u << 25);
    out(h, GEN7_3DSTATE_URB_DS); out(h, 2u << 25);
    out(h, GEN7_3DSTATE_BLEND_STATE_POINTERS); out(h, blend | 1);
    out(h, GEN6_3DSTATE_CC_STATE_POINTERS); out(h, cc_state | 1);
    out(h, GEN8_3DSTATE_MULTISAMPLE); out(h, 0);
    out(h, GEN6_3DSTATE_SAMPLE_MASK); out(h, 1);
    out(h, GEN8_3DSTATE_WM_HZ_OP | (5 - 2)); zeros(h, 4);
    out(h, GEN7_3DSTATE_CONSTANT_HS | (11 - 2)); zeros(h, 10);
    out(h, GEN7_3DSTATE_HS | (9 - 2)); zeros(h, 8);
    out(h, GEN7_3DSTATE_BINDING_TABLE_POINTERS_HS); out(h, 0);
    out(h, GEN8_3DSTATE_SAMPLER_STATE_POINTERS_HS); out(h, 0);
    out(h, GEN7_3DSTATE_TE | (4 - 2)); zeros(h, 3);
    out(h, GEN6_3DSTATE_CONSTANT_GS | (11 - 2)); zeros(h, 10);
    out(h, GEN6_3DSTATE_GS | (10 - 2)); zeros(h, 9);
    out(h, GEN7_3DSTATE_BINDING_TABLE_POINTERS_GS); out(h, 0);
    out(h, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_GS); out(h, 0);
    out(h, GEN7_3DSTATE_CONSTANT_DS | (11 - 2)); zeros(h, 10);
    out(h, GEN7_3DSTATE_DS | (9 - 2)); zeros(h, 8);
    out(h, GEN7_3DSTATE_BINDING_TABLE_POINTERS_DS); out(h, 0);
    out(h, GEN8_3DSTATE_SAMPLER_STATE_POINTERS_DS); out(h, 0);
    out(h, GEN7_3DSTATE_BINDING_TABLE_POINTERS_VS); out(h, 0);
    out(h, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_VS); out(h, 0);
    out(h, GEN6_3DSTATE_CONSTANT_VS | (11 - 2)); zeros(h, 10);
    out(h, GEN6_3DSTATE_VS | (9 - 2)); zeros(h, 8);
    out(h, GEN7_3DSTATE_STREAMOUT | (5 - 2)); zeros(h, 4);
    out(h, GEN6_3DSTATE_CLIP | (4 - 2)); out(h, 1u << 10); zeros(h, 2);
    out(h, GEN7_3DSTATE_SBE | (4 - 2));
    out(h, 1u << GEN7_SBE_NUM_OUTPUTS_SHIFT | GEN8_SBE_FORCE_URB_ENTRY_READ_LENGTH |
           GEN8_SBE_FORCE_URB_ENTRY_READ_OFFSET | 1u << GEN7_SBE_URB_ENTRY_READ_LENGTH_SHIFT |
           1u << GEN8_SBE_URB_ENTRY_READ_OFFSET_SHIFT);
    zeros(h, 2);
    out(h, GEN8_3DSTATE_SBE_SWIZ | (11 - 2)); zeros(h, 10);
    out(h, GEN8_3DSTATE_RASTER | (5 - 2));
    out(h, GEN8_RASTER_FRONT_WINDING_CCW | GEN8_RASTER_CULL_NONE); zeros(h, 3);
    out(h, GEN6_3DSTATE_SF | (4 - 2)); zeros(h, 3);
    out(h, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_PS); out(h, 0);
    out(h, GEN6_3DSTATE_WM | (2 - 2));
    out(h, GEN8_3DSTATE_PS_PERSPECTIVE_PIXEL_BARYCENTRIC | GEN7_WM_STATISTICS_ENABLE);
    out(h, GEN8_3DSTATE_PS_BLEND | (2 - 2)); out(h, GEN8_PS_BLEND_HAS_WRITEABLE_RT);
    /* valid, attribute enable, kills pixel (bit 28) */
    out(h, GEN8_3DSTATE_PS_EXTRA | (2 - 2)); out(h, GEN8_PSX_PIXEL_SHADER_VALID | GEN8_PSX_ATTRIBUTE_ENABLE | (1u << 28));
    out(h, GEN8_3DSTATE_SCISSOR_STATE_POINTERS); out(h, scissor);
    out(h, GEN8_3DSTATE_WM_DEPTH_STENCIL | (3 - 2)); zeros(h, 2);
    out(h, GEN7_3DSTATE_DEPTH_BUFFER | (8 - 2)); zeros(h, 7);
    out(h, GEN8_3DSTATE_HIER_DEPTH_BUFFER | (5 - 2)); zeros(h, 4);
    out(h, GEN8_3DSTATE_STENCIL_BUFFER | (5 - 2)); zeros(h, 4);
    out(h, GEN7_3DSTATE_CLEAR_PARAMS | (3 - 2)); out(h, 0); out(h, 1);
    out(h, GEN4_3DSTATE_DRAWING_RECTANGLE | (4 - 2)); out(h, 0); out(h, 4095u << 16 | 4095u); out(h, 0);
    out(h, GEN4_3DSTATE_VERTEX_BUFFERS | (1 + 4 - 2));
    out(h, 0u << GEN6_VB0_BUFFER_INDEX_SHIFT | GEN8_VB0_BUFFER_ADDR_MOD_EN | VERTEX_PITCH << VB0_BUFFER_PITCH_SHIFT);
    out(h, h->batch_gpu + DYN_START); out(h, 0);
    out(h, DYN_END - DYN_START);
    /* VUE: header (zeros), position (x, y, 0, 1), attribute 0 (zeros) */
    out(h, GEN4_3DSTATE_VERTEX_ELEMENTS | (3 * 2 + 1 - 2));
    out(h, GEN6_VE0_VALID | SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT | 0u << VE0_OFFSET_SHIFT);
    out(h, GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_0_SHIFT | GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_1_SHIFT |
           GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT | GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT);
    out(h, GEN6_VE0_VALID | SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT | 0u << VE0_OFFSET_SHIFT);
    out(h, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT | GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
           GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT | GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);
    out(h, GEN6_VE0_VALID | SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT | 0u << VE0_OFFSET_SHIFT);
    out(h, GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_0_SHIFT | GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_1_SHIFT |
           GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT | GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT);
    out(h, GEN8_3DSTATE_VF_TOPOLOGY); out(h, _3DPRIM_RECTLIST);
    out(h, GEN8_3DSTATE_VF_INSTANCING | (3 - 2)); zeros(h, 2);
    h->started = true;
    h->kernel_valid = h->bt_valid = false;
}

static void barrier(V2Hw *h)
{
    /* render target writes to memory, then drop the sampler's copies */
    out(h, GEN6_PIPE_CONTROL | (6 - 2));
    out(h, GEN7_PIPE_CONTROL_CS_STALL | (1u << 1) | GEN6_PIPE_CONTROL_WC_FLUSH | (1u << 5));
    zeros(h, 4);
    out(h, GEN6_PIPE_CONTROL | (6 - 2));
    out(h, GEN7_PIPE_CONTROL_CS_STALL | (1u << 1) | GEN6_PIPE_CONTROL_TC_FLUSH);
    zeros(h, 4);
    h->barriers++;
}

static bool kernel_offset(V2Hw *h, const V2gDraw *d, uint32_t *offset)
{
    for (unsigned i = 0; i < h->kcount; i++)
        if (h->kcache[i].id == d->key_id) { *offset = h->kcache[i].offset; return true; }
    uint32_t bytes = (d->code_bytes + 63) & ~63u;
    if (h->kcount == KERNEL_SLOTS || h->kernel_used + bytes + 64 > KERNEL_BYTES) {
        if (h->started) return false;          /* in use by the queued draws: finish first */
        h->kcount = 0;
        h->kernel_used = 0;
    }
    uint32_t o = h->kernel_used;
    memcpy(h->kernels + o, d->code, d->code_bytes);
    memset(h->kernels + o + d->code_bytes, 0, bytes + 64 - d->code_bytes);
    gpu3d_flush_cpu(h->kernels + o, bytes + 64);
    h->kernel_used += bytes;
    h->kcache[h->kcount].id = d->key_id;
    h->kcache[h->kcount].offset = o;
    h->kcount++;
    *offset = o;
    return true;
}

static void flush_fb(V2Hw *h, uint32_t lo, uint32_t hi)
{
    if (hi > h->fb_bytes) hi = h->fb_bytes;
    /* GPU writes do not invalidate the CPU's lines (physical: the DOS Glide
     * game's start-up readback hung with snooping alone), and a dirty CPU line
     * written back later would undo a GPU write: always flush */
    if (hi > lo) gpu3d_flush_cpu(h->dev->fb_mem + lo, hi - lo);
}

/* ---- V2gBackend ---- */

static uint32_t *hw_alloc_data(void *ctx, unsigned dwords, unsigned align, uint32_t *index)
{
    V2Hw *h = ctx;
    uint32_t a = align * 4;
    uint32_t start = (h->data + a - 1) / a * a;
    if (start + dwords * 4 > BATCH_BYTES - DATA_START) return NULL;
    h->data = start + dwords * 4;
    *index = start / 4;
    return (uint32_t *)(h->batch + DATA_START + start);
}

static uint32_t hw_tex_index(void *ctx, unsigned tmu, unsigned entry)
{
    V2Hw *h = ctx;
    return (h->tex_gpu[tmu][entry] - h->tex_base_gpu) / 4;
}

static bool hw_draw(void *ctx, const V2gDraw *d)
{
    V2Hw *h = ctx;
    if (h->broken) return false;
    if (h->cmd + 512 > CMD_END || h->dyn + 512 > DYN_END || h->ss + 1024 > SS_END) return false;
    uint32_t kernel;
    if (!kernel_offset(h, d, &kernel)) return false;
    if (!h->started) prologue(h);
    if (d->cpu_hi > d->cpu_lo) {
        uint64_t t0 = wyse_ticks_us();
        flush_fb(h, d->cpu_lo, d->cpu_hi);
        h->flush_us += wyse_ticks_us() - t0;
    }
    if (d->hazard || g_gpu_v2_serialize) barrier(h);
    if (!h->kernel_valid || h->kernel_id != d->key_id) {
        out(h, GEN7_3DSTATE_PS | (12 - 2));
        out(h, kernel); out(h, 0);
        out(h, (uint32_t)V2G_BT_COUNT << GEN6_3DSTATE_WM_BINDING_TABLE_ENTRY_COUNT_SHIFT);
        zeros(h, 2);
        out(h, (63u - 1) << GEN8_3DSTATE_PS_MAX_THREADS_SHIFT | (1u << 11) /* push constants */ |
               GEN6_3DSTATE_WM_16_DISPATCH_ENABLE);
        out(h, 6u << GEN6_3DSTATE_WM_DISPATCH_START_GRF_0_SHIFT);
        zeros(h, 4);
        h->kernel_id = d->key_id;
        h->kernel_valid = true;
    }
    if (!h->bt_valid || h->bt_draw != d->draw_offset || h->bt_aux != d->aux_offset || h->bt_row != d->row_width ||
        h->bt_rows != d->rows) {
        uint32_t bt = binding_table(h, d);
        out(h, GEN7_3DSTATE_BINDING_TABLE_POINTERS_PS); out(h, bt);
        h->bt_draw = d->draw_offset;
        h->bt_aux = d->aux_offset;
        h->bt_row = d->row_width;
        h->bt_rows = d->rows;
        h->bt_valid = true;
    }
    /* the record: push constant buffer 3 (absolute address, as iris) */
    uint32_t rec = dyn_alloc(h, V2G_RECORD_DWORDS * 4, 32);
    memcpy(at(h, rec), d->record, V2G_RECORD_DWORDS * 4);
    out(h, GEN6_3DSTATE_CONSTANT_PS | (11 - 2));
    out(h, 0);                                        /* read lengths 0, 1 */
    out(h, (uint32_t)(V2G_RECORD_DWORDS / 8) << 16);  /* read length 3 in registers */
    zeros(h, 6);                                      /* buffers 0-2 */
    out(h, h->batch_gpu + rec); out(h, 0);            /* buffer 3 */
    /* RECTLIST: (x1, y1), (x0, y1), (x0, y0) of the exclusive rectangle */
    uint32_t v = dyn_alloc(h, 3 * VERTEX_PITCH, VERTEX_PITCH);
    float xy[6] = { (float)(d->x1 + 1), (float)(d->y1 + 1), (float)d->x0, (float)(d->y1 + 1), (float)d->x0, (float)d->y0 };
    memcpy(at(h, v), xy, sizeof xy);
    out(h, GEN4_3DPRIMITIVE | (7 - 2));
    out(h, 0);
    out(h, 3);
    out(h, (v - DYN_START) / VERTEX_PITCH);
    out(h, 1);
    zeros(h, 2);
    uint32_t lo1 = d->draw_offset + (uint32_t)d->y0 * d->row_width, hi1 = d->draw_offset + (uint32_t)(d->y1 + 1) * d->row_width;
    uint32_t lo2 = d->aux_offset + (uint32_t)d->y0 * d->row_width, hi2 = d->aux_offset + (uint32_t)(d->y1 + 1) * d->row_width;
    uint32_t lo = lo1 < lo2 ? lo1 : lo2, hi = hi1 > hi2 ? hi1 : hi2;
    if (h->written_hi <= h->written_lo) { h->written_lo = lo; h->written_hi = hi; }
    else {
        if (lo < h->written_lo) h->written_lo = lo;
        if (hi > h->written_hi) h->written_hi = hi;
    }
    h->draws++;
    return true;
}

static void hw_finish(void *ctx)
{
    V2Hw *h = ctx;
    if (!h->started) { h->data = 0; return; }
    uint64_t t0 = wyse_ticks_us();
    uint32_t seqno = gpu3d_next_seqno();
    out(h, GEN6_PIPE_CONTROL | (6 - 2));
    out(h, GEN7_PIPE_CONTROL_CS_STALL | (1u << 1) | GEN6_PIPE_CONTROL_WC_FLUSH | (1u << 5));
    zeros(h, 4);
    out(h, (0x20u << 23) | (1u << 22) | 2);      /* MI_STORE_DATA_IMM, GGTT */
    out(h, gpu3d_status_gpu()); out(h, 0); out(h, seqno);
    out(h, 0x0au << 23);                          /* MI_BATCH_BUFFER_END */
    if (h->cmd & 7) out(h, 0);
    gpu3d_flush_cpu(h->batch, h->ss);
    gpu3d_flush_cpu(h->batch + CMD_START, h->cmd - CMD_START);
    gpu3d_flush_cpu(h->batch + DYN_START, h->dyn - DYN_START);
    gpu3d_flush_cpu(h->batch + DATA_START, h->data);
    if (!gpu3d_execute(h->batch_gpu + CMD_START, seqno, 2000000)) {
        printf("gpu v2: batch of %llu draws did not finish; GPU back end off\n", (unsigned long long)h->draws);
        h->broken = true;
    }
    /* the CPU must not keep stale copies of what the GPU wrote */
    flush_fb(h, h->written_lo, h->written_hi);
    h->written_lo = h->written_hi = 0;
    h->cmd = CMD_START;
    h->ss = 0;
    h->dyn = DYN_START;
    h->data = 0;
    h->started = false;
    h->batches++;
    h->busy_us += wyse_ticks_us() - t0;
}

static void hw_texture_written(void *ctx, unsigned tmu, unsigned entry, uint32_t first, uint32_t end)
{
    V2Hw *h = ctx;
    if (tmu < 2 && entry < TEX_CACHE_MAX && end > first)
        gpu3d_flush_data(h->dev->texture_cache[tmu][entry].data + first, (end - first) * 4);
}

/* A back end for this board (made once per board; the GGTT has no free). */
bool gpu3d_v2_backend(voodoo_t *dev, V2gBackend *backend)
{
    V2Hw *h = NULL;
    for (unsigned i = 0; i < 4; i++)
        if (contexts[i] && contexts[i]->dev == dev) h = contexts[i];
    if (!h) {
        unsigned slot = 0;
        while (slot < 4 && contexts[slot]) slot++;
        if (slot == 4 || !gpu3d_open()) return false;
        h = calloc(1, sizeof(*h));
        if (!h) return false;
        h->dev = dev;
        h->batch = gpu3d_alloc(BATCH_BYTES / 4096, &h->batch_gpu);
        h->kernels = gpu3d_alloc(KERNEL_BYTES / 4096, &h->kernels_gpu);
        unsigned table_bytes;
        const uint8_t *tables = voodoo2_gpu_tables(&table_bytes);
        uint8_t *t = gpu3d_alloc((table_bytes + 4095) / 4096, &h->tables_gpu);
        if (!h->batch || !h->kernels || !t) { free(h); return false; }
        memcpy(t, tables, table_bytes);
        gpu3d_flush_cpu(t, table_bytes);
        h->table_bytes = table_bytes;
        h->fb_bytes = dev->fb_mask + 1;
        if ((uintptr_t)dev->fb_mem & 4095) { printf("gpu v2: frame buffer not page-aligned\n"); free(h); return false; }
        h->fb_gpu = gpu3d_map(dev->fb_mem, h->fb_bytes);
        uint32_t last = 0;
        for (unsigned tmu = 0; tmu < 2; tmu++)
            for (unsigned e = 0; e < TEX_CACHE_MAX; e++) {
                uint32_t a = gpu3d_map(dev->texture_cache[tmu][e].data, ENTRY_BYTES);
                if (!a) { printf("gpu v2: GGTT full\n"); free(h); return false; }
                if (!tmu && !e) h->tex_base_gpu = a & ~4095u;
                h->tex_gpu[tmu][e] = a;
                last = a + ENTRY_BYTES;
                gpu3d_flush_cpu(dev->texture_cache[tmu][e].data, ENTRY_BYTES);
            }
        h->tex_elements = (last - h->tex_base_gpu) / 4;
        if (!h->fb_gpu) { free(h); return false; }
        h->cmd = CMD_START;
        h->dyn = DYN_START;
        contexts[slot] = h;
        printf("gpu v2: back end for board %p: fb %08x, textures %08x (%u MB), batch %08x\n", (void *)dev, h->fb_gpu,
               h->tex_base_gpu, (unsigned)((last - h->tex_base_gpu) >> 20), h->batch_gpu);
    }
    V2gBackend b = { h, hw_alloc_data, hw_tex_index, hw_draw, hw_finish, hw_texture_written };
    *backend = b;
    return !h->broken;
}

/* "gpu v2test [triangles] [mode]": the host test's comparison on a private
 * test board, the real GPU against the C renderer */
#include "../../pc/voodoo2_gpu_test.h"

void gpu3d_v2_test(unsigned triangles, int mode)
{
    static Voodoo2State *board;
    static V2gState *gpu;
    if (!board) {
        board = voodoo2_gpu_test_board();
        if (!board) { printf("gpu v2test: no test board\n"); return; }
    }
    if (!gpu) {
        V2gBackend backend;
        if (!gpu3d_v2_backend(voodoo2_device(board), &backend)) { printf("gpu v2test: no GPU back end\n"); return; }
        gpu = voodoo2_gpu_create(&backend);
        if (!gpu) { printf("gpu v2test: no memory\n"); return; }
    }
    uint64_t t0 = wyse_ticks_us();
    V2gTestResult r = voodoo2_gpu_test_run(board, gpu, triangles ? triangles : 24, mode, true);
    V2gStats st = voodoo2_gpu_stats(gpu);
    printf("gpu v2test: %u of %u modes pass (%u exact), %llu of %llu pixels differ, %llu ms; kernels %llu, draws %llu, "
           "barriers %llu, finishes %llu - %s\n", r.passed, r.modes, r.exact, r.differ, r.pixels,
           (unsigned long long)((wyse_ticks_us() - t0) / 1000), (unsigned long long)st.kernels,
           (unsigned long long)st.draws, (unsigned long long)st.barriers, (unsigned long long)st.finishes,
           r.passed == r.modes ? "PASS" : "FAIL");
    gpu3d_v2_report();
}

void gpu3d_v2_report(void)
{
    for (unsigned i = 0; i < 4; i++) {
        V2Hw *h = contexts[i];
        if (!h) continue;
        printf("gpu v2 board %p: batches %llu, draws %llu, barriers %llu, GPU+wait %llu us, CPU flush %llu us%s\n",
               (void *)h->dev, (unsigned long long)h->batches, (unsigned long long)h->draws,
               (unsigned long long)h->barriers, (unsigned long long)h->busy_us, (unsigned long long)h->flush_us,
               h->broken ? ", BROKEN" : "");
    }
}

#endif
