/* SPDX-License-Identifier: GPL-3.0-or-later
 * Fixed offscreen Gen8 Gouraud draw, derived from tools/wgpu/wgputest.c.
 * Same IGT-based state and project Gen8 EU encoder. The diagnostic stays fixed;
 * the bounded fill variant targets a validated framebuffer surface.
 * CPU code uses integer stores
 * of IEEE-754 representations and never touches the NT kernel's FPU state.
 */
#include <stdint.h>
#include "wgpu_nt.h"
#include "wgpu_fill.h"
#include "wgpu_draw.h"
#include "../../efi/platform/igt/gen8_render.h"
#include "../../efi/platform/gen8_asm.h"

#define STATE_OFFSET (32u * 1024u)
#define TRI_BYTES WGPU_NT_TRI_BATCH_BYTES
#define MI_BATCH_BUFFER_END (0x0au << 23)

typedef struct { volatile uint32_t *buffer; uint32_t cmd, state; } Cursor;
static void out(Cursor *c, uint32_t value) { c->buffer[c->cmd / 4] = value; c->cmd += 4; }
static void zeros(Cursor *c, unsigned count) { while (count--) out(c, 0); }
static volatile void *ptr(Cursor *c, uint32_t offset) { return (volatile uint8_t *)c->buffer + offset; }
static uint32_t state(Cursor *c, uint32_t bytes, uint32_t align)
{
    c->state = (c->state + align - 1) & ~(align - 1);
    uint32_t at = c->state;
    for (uint32_t i = 0; i < bytes / 4; ++i) c->buffer[at / 4 + i] = 0;
    c->state += bytes;
    return at;
}

/* These conversions emit IEEE bits using integer operations. They never use
 * x87/SSE state in the NT kernel. Geometry is bounded to 0..2048. */
static uint32_t integer_float(uint32_t n)
{
    uint32_t exponent=150;
    if (!n) return 0;
    while (!(n & 0x800000u)) { n <<= 1; --exponent; }
    return exponent<<23 | (n & 0x7fffffu);
}
static uint32_t channel_float(uint32_t n, uint32_t maximum)
{
    uint32_t exponent=127, scaled, mantissa, remainder;
    if (!n) return 0;
    while (n<maximum) { n<<=1; --exponent; }
    /* Split the shift so this also handles texture dimensions up to 2048
     * without 64-bit division or kernel floating-point state. */
    scaled=n<<19;
    mantissa=(scaled/maximum)<<4;
    scaled=(scaled%maximum)<<4;
    mantissa+=scaled/maximum;
    remainder=scaled%maximum;
    if (remainder*2>maximum || (remainder*2==maximum && (mantissa&1))) ++mantissa;
    if (mantissa==0x1000000u) {mantissa>>=1; ++exponent;}
    return exponent<<23 | (mantissa & 0x7fffffu);
}
static uint32_t bind_surface(Cursor *c, uint32_t address, uint32_t pitch,
    uint32_t width, uint32_t height, uint32_t bpp)
{
        uint32_t ss_at = state(c, sizeof(struct gen8_surface_state), 64);
        volatile struct gen8_surface_state *ss = ptr(c, ss_at);
        ss->ss0.surface_type = SURFACE_2D;
        ss->ss0.surface_format = bpp==32 ? SURFACEFORMAT_B8G8R8A8_UNORM : SURFACEFORMAT_B5G6R5_UNORM;
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
        return ss_at;
}

static WgpuNtU32 build_render(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags,
    const WgpuNtFill *fill, const WgpuNtFill *source, const WgpuNtDraw *draw,
    WgpuNtU32 lfb_gfx)
{
    if (!memory || bytes < WGPU_NT_TRI_MIN || !fence || flags > 1 ||
        (gfx & 4095u) || gfx > 0xffffffffu - bytes) return 0;
    volatile uint32_t *ap = (volatile uint32_t *)memory;
    Cursor c = { ap + WGPU_NT_TRI_BATCH / 4, 0, STATE_OFFSET };
    uint32_t batch_gfx = gfx + WGPU_NT_TRI_BATCH;
    uint32_t kernel_gfx = gfx + WGPU_NT_TRI_KERNEL;
    uint32_t color = gfx + WGPU_NT_TRI_TARGET;
    uint32_t pitch = WGPU_NT_TRI_WIDTH * 2, width = WGPU_NT_TRI_WIDTH, height = WGPU_NT_TRI_HEIGHT;
    uint32_t vertex_count=draw ? draw->vertex_count : fill ? 6 : 3;
    if (fill) {color=lfb_gfx+fill->offset;pitch=fill->pitch;width=fill->width;height=fill->height;}
    for (unsigned i = 0; i < TRI_BYTES / 4; ++i) c.buffer[i] = 0;
    if (!fill)
        for (unsigned i = 0; i < WGPU_NT_TRI_WIDTH * WGPU_NT_TRI_HEIGHT / 2; ++i)
            ap[WGPU_NT_TRI_TARGET / 4 + i] = 0x10821082u;
    Gen8Inst kernel[] = { gen8_pln(113, 6, 0), gen8_pln(115, 6, 16),
        gen8_pln(117, 7, 0), gen8_pln(119, 7, 16), gen8_send(5, 0, 113, 0x10031000u, 1) };
    /* The existing IGT sampler shader used by efi/platform/intel_gpu.c. */
    static const uint32_t copy_kernel[][4] = {
        { 0x0080005a, 0x2f403ae8, 0x3a0000c0, 0x008d0040 },
        { 0x0080005a, 0x2f803ae8, 0x3a0000d0, 0x008d0040 },
        { 0x02800031, 0x2e203a48, 0x0e8d0f40, 0x08840001 },
        { 0x05800031, 0x20003a40, 0x0e8d0e20, 0x90031000 },
    };
    for (unsigned i = 0; i < (source ? 4u : 5u); ++i)
        for (unsigned j = 0; j < 4; ++j)
            ap[WGPU_NT_TRI_KERNEL / 4 + i * 4 + j] = source ? copy_kernel[i][j] : kernel[i].dw[j];
    uint32_t bt = state(&c, 8, 32), sampler = 0;
    c.buffer[bt / 4] = bind_surface(&c,color,pitch,width,height,fill ? fill->bpp : 16);
    if (source) {
        c.buffer[bt / 4 + 1] = bind_surface(&c,lfb_gfx+source->offset,source->pitch,
                                            source->width,source->height,source->bpp);
        sampler = state(&c, sizeof(struct gen8_sampler_state), 64);
        volatile struct gen8_sampler_state *ss = ptr(&c, sampler);
        ss->ss0.min_filter = GEN4_MAPFILTER_NEAREST;
        ss->ss0.mag_filter = GEN4_MAPFILTER_NEAREST;
        ss->ss3.r_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
        ss->ss3.s_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
        ss->ss3.t_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
    }
    uint32_t vb = state(&c, vertex_count * 32, 8);
    static const uint32_t vertex[24] = {
        0x42800000,0x41000000,0x3f800000,0,0,0x3f800000,0,0,
        0x42f00000,0x42f00000,0,0x3f800000,0,0x3f800000,0,0,
        0x41000000,0x42f00000,0,0,0x3f800000,0x3f800000,0,0
    };
    if (draw) {
        for (unsigned i=0; i<vertex_count; ++i) {
            const WgpuNtVertex *in=&draw->vertices[i];
            volatile uint32_t *v=c.buffer+vb/4+i*8;
            v[0]=in->x;
            v[1]=in->y;
            v[2]=channel_float((in->argb>>16)&255,255);
            v[3]=channel_float((in->argb>>8)&255,255);
            v[4]=channel_float(in->argb&255,255);
            v[5]=channel_float(in->argb>>24,255);
        }
    } else if (!fill) {
        for (unsigned i = 0; i < 24; ++i) c.buffer[vb / 4 + i] = vertex[i];
    } else {
        uint32_t r,g,b,a;
        static const unsigned right[6]={0,1,0,1,1,0}, bottom[6]={0,0,1,0,1,1};
        if (fill->bpp==16) {
            r=channel_float(fill->color>>11,31);g=channel_float((fill->color>>5)&63,63);
            b=channel_float(fill->color&31,31);a=0x3f800000u;
        } else {
            r=channel_float((fill->color>>16)&255,255);g=channel_float((fill->color>>8)&255,255);
            b=channel_float(fill->color&255,255);a=channel_float(fill->color>>24,255);
        }
        for (unsigned i=0;i<6;++i) {
            volatile uint32_t *v=c.buffer+vb/4+i*8;
            v[0]=integer_float(right[i]?fill->right:fill->left);
            v[1]=integer_float(bottom[i]?fill->bottom:fill->top);
            v[2]=r;v[3]=g;v[4]=b;v[5]=a;v[6]=v[7]=0;
            if (source) {
                v[2]=channel_float(right[i]?source->right:source->left,source->width);
                v[3]=channel_float(bottom[i]?source->bottom:source->top,source->height);
            }
        }
    }
    uint32_t cc_state = state(&c, sizeof(struct gen6_color_calc_state), 64);
    uint32_t blend = state(&c, sizeof(struct gen8_blend_state), 64);
    {
        volatile struct gen8_blend_state *bs = ptr(&c, blend);
        for (int i = 0; i < 16; i++) {
            bs->bs[i].dest_blend_factor = GEN6_BLENDFACTOR_ZERO;
            bs->bs[i].source_blend_factor = GEN6_BLENDFACTOR_ONE;
            bs->bs[i].color_blend_func = GEN6_BLENDFUNCTION_ADD;
            bs->bs[i].pre_blend_color_clamp = 1;
        }
    }
    uint32_t cc_vp = state(&c, sizeof(struct gen4_cc_viewport), 32);
    ((volatile uint32_t *)ptr(&c, cc_vp))[0] = 0xf99a130cu;
    ((volatile uint32_t *)ptr(&c, cc_vp))[1] = 0x799a130cu;
    uint32_t sf_vp = state(&c, sizeof(struct gen7_sf_clip_viewport), 64);
    ((volatile uint32_t *)ptr(&c, sf_vp))[8] = 0;
    ((volatile uint32_t *)ptr(&c, sf_vp))[9] = 0x3f800000u;
    ((volatile uint32_t *)ptr(&c, sf_vp))[10] = 0;
    ((volatile uint32_t *)ptr(&c, sf_vp))[11] = 0x3f800000u;
    uint32_t scissor = state(&c, sizeof(struct gen6_scissor_rect), 64);

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
    out(&c, batch_gfx | 1); out(&c, 0);              /* surface */
    out(&c, batch_gfx | 1); out(&c, 0);              /* dynamic */
    out(&c, 0); out(&c, 0);                          /* indirect */
    out(&c, kernel_gfx | 1); out(&c, 0);             /* instruction */
    out(&c, 0xfffff000u | 1); out(&c, (TRI_BYTES / 4096) << 12 | 1);
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
    out(&c, 0); out(&c, 0);                          /* kernel at the instruction base */
    out(&c, source ? (1u << GEN6_3DSTATE_WM_SAMPLER_COUNT_SHIFT |
                      2u << GEN6_3DSTATE_WM_BINDING_TABLE_ENTRY_COUNT_SHIFT)
                   : 1u << GEN6_3DSTATE_WM_BINDING_TABLE_ENTRY_COUNT_SHIFT);
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
    out(&c, (height - 1) << 16 | (width - 1)); out(&c, 0);
    out(&c, GEN4_3DSTATE_VERTEX_BUFFERS | (1 + 4 - 2));
    out(&c, 0u << GEN6_VB0_BUFFER_INDEX_SHIFT | GEN8_VB0_BUFFER_ADDR_MOD_EN | 32u << VB0_BUFFER_PITCH_SHIFT);
    out(&c, batch_gfx + vb); out(&c, 0);
    out(&c, vertex_count * 32);
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
    out(&c, GEN4_3DPRIMITIVE | (7 - 2)); out(&c, 0); out(&c, vertex_count); out(&c, 0); out(&c, 1); out(&c, 0); out(&c, 0);
    out(&c, MI_BATCH_BUFFER_END);
    if (c.cmd & 7) out(&c, 0);
    if (c.cmd > STATE_OFFSET || c.state > TRI_BYTES) return 0;
    ap[0] = completed;
    ap[WGPU_NT_DESCRIPTOR / 4] = WGPU_NT_TRI_BATCH;
    ap[WGPU_NT_DESCRIPTOR / 4 + 1] = c.state;
    ap[WGPU_NT_DESCRIPTOR / 4 + 2] = fence;
    ap[WGPU_NT_DESCRIPTOR / 4 + 3] = flags;
    return c.state;
}

WgpuNtU32 wgpu_nt_build_triangle(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags)
{
    return build_render(memory,bytes,gfx,completed,fence,flags,0,0,0,0);
}
WgpuNtU32 wgpu_nt_build_fill(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags,
    const WgpuNtFill *fill, WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx)
{
    WgpuNtFill request;
    if (!wgpu_nt_fill_valid(fill,lfb_bytes,lfb_gfx)) return 0;
    request=*fill;
    return build_render(memory,bytes,gfx,completed,fence,flags,&request,0,0,lfb_gfx);
}

WgpuNtU32 wgpu_nt_build_copy(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags,
    const WgpuNtCopy *copy, WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx)
{
    WgpuNtCopy request;
    if (!wgpu_nt_copy_valid(copy,lfb_bytes,lfb_gfx)) return 0;
    request=*copy;
    return build_render(memory,bytes,gfx,completed,fence,flags,&request.dst,&request.src,0,lfb_gfx);
}

WgpuNtU32 wgpu_nt_build_draw(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags,
    const WgpuNtDraw *draw, WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx)
{
    WgpuNtDraw request;
    if (!wgpu_nt_draw_valid(draw,lfb_bytes,lfb_gfx)) return 0;
    request=*draw;
    return build_render(memory,bytes,gfx,completed,fence,flags,&request.target,0,&request,lfb_gfx);
}
