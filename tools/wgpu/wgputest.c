/*
 * WGPUTEST - the wyseDOS GPU render channel (pc/wgpu.h) used from a DOS
 * guest, through its real PCI BARs: the register page (every access a VM
 * exit), the write-combining aperture, and the QEMU VGA frame buffer.
 *
 *   WGPUTEST        identity, 16 stores and fences, latency, a queue of 1000
 *   WGPUTEST TRI    also a Gouraud triangle by the 3D pipeline into the
 *                   frame buffer in VBE mode 111h (640x480, 16 bpp)
 *
 * Open Watcom, DOS/4GW (tools/wgpu/build.ps1). The 3D state follows gpu3d.c,
 * which follows IGT GPU Tools' gen8_render_copyfunc; the pixel shader comes
 * from gen8_asm.c by way of genkernel.c.
 */
#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "igt/gen8_render.h"
#include "wgpu.h"
#include "wgpu_kernel.h"

/* ---- CPU helpers ---- */

uint64_t rdtsc(void);
#pragma aux rdtsc = 0x0f 0x31 value [edx eax];
void sfence(void);
#pragma aux sfence = 0x0f 0xae 0xf8;
void mfence(void);
#pragma aux mfence = 0x0f 0xae 0xf0;
void clflush(const volatile void *p);
#pragma aux clflush = 0x0f 0xae 0x38 parm [eax];

static uint64_t tsc_per_ms;

static void calibrate(void)
{
    volatile uint32_t *ticks = (volatile uint32_t *)0x46c;   /* BIOS, 18.2 Hz */
    uint32_t t = *ticks;
    while (*ticks == t) {}
    uint64_t a = rdtsc();
    t = *ticks;
    while (*ticks - t < 4) {}
    tsc_per_ms = (rdtsc() - a) * 1000 / (4 * 54925);         /* 4 ticks = 219.7 ms */
    if (!tsc_per_ms) tsc_per_ms = 1;
}

static uint32_t us_since(uint64_t t0) { return (uint32_t)((rdtsc() - t0) * 1000 / tsc_per_ms); }

static void flush_range(const volatile void *p, uint32_t bytes)
{
    const volatile uint8_t *c = (const volatile uint8_t *)((uint32_t)p & ~63u), *end = (const volatile uint8_t *)p + bytes;
    for (; c < end; c += 64) clflush(c);
    mfence();
}

/* ---- PCI and memory ---- */

static uint32_t pci_read(unsigned dev, unsigned reg)
{
    outpd(0xcf8, 0x80000000u | dev << 11 | (reg & 0xfc));
    return inpd(0xcfc);
}

static int find(uint32_t id, uint32_t subsystem)
{
    for (unsigned dev = 0; dev < 32; dev++)
        if (pci_read(dev, 0) == id && (!subsystem || pci_read(dev, 0x2c) == subsystem)) return (int)dev;
    return -1;
}

static void *map_phys(uint32_t phys, uint32_t size)
{
    union REGS r;
    memset(&r, 0, sizeof r);
    r.w.ax = 0x0800;
    r.w.bx = (uint16_t)(phys >> 16);
    r.w.cx = (uint16_t)phys;
    r.w.si = (uint16_t)(size >> 16);
    r.w.di = (uint16_t)size;
    int386(0x31, &r, &r);
    if (r.x.cflag) return NULL;
    return (void *)((uint32_t)r.w.bx << 16 | r.w.cx);
}

/* ---- the channel ---- */

#define AP_MAP (1u << 20)              /* the part of the aperture this test uses */
#define FENCE 0x0000u
#define DESC 0x0040u
#define TEST_BATCH 0x1000u
#define TARGET 0x3000u
#define KERNEL 0x8000u
#define TRI_BATCH 0x10000u
#define TRI_BYTES 0x10000u

static volatile uint32_t *regs;
static volatile uint8_t *ap;
static uint32_t ap_gfx, lfb_gfx, fence_value;

#define REG(r) regs[(r) / 4]
#define AP32(off) (*(volatile uint32_t *)(ap + (off)))

static uint32_t submit(uint32_t batch, uint32_t bytes)
{
    AP32(DESC) = batch;
    AP32(DESC + 4) = bytes;
    AP32(DESC + 8) = ++fence_value;
    AP32(DESC + 12) = 0;
    sfence();
    REG(WGPU_REG_SUBMIT) = DESC;
    return REG(WGPU_REG_SUBMIT);
}

static int wait_fence(uint32_t value, uint32_t ms)
{
    uint64_t t0 = rdtsc();
    while (AP32(FENCE) != value)
        if (us_since(t0) > ms * 1000) {
            printf("  fence %08lx not seen in %lu ms: fence %08lx, status %lx\n", (unsigned long)value,
                   (unsigned long)ms, (unsigned long)AP32(FENCE), (unsigned long)REG(WGPU_REG_STATUS));
            return 0;
        }
    return 1;
}

#define MI_STORE_DATA_IMM_GGTT ((0x20u << 23) | (1u << 22) | 2)
#define MI_BATCH_BUFFER_END (0x0au << 23)

static int test_stores(void)
{
    uint32_t n = 0, i, right = 0;
    for (i = 0; i < 16; i++) AP32(TARGET + 4 * i) = 0;
    for (i = 0; i < 16; i++) {
        AP32(TEST_BATCH + 4 * n++) = MI_STORE_DATA_IMM_GGTT;
        AP32(TEST_BATCH + 4 * n++) = ap_gfx + TARGET + 4 * i;
        AP32(TEST_BATCH + 4 * n++) = 0;
        AP32(TEST_BATCH + 4 * n++) = 0x5eed0000u + i;
    }
    AP32(TEST_BATCH + 4 * n++) = MI_BATCH_BUFFER_END;
    AP32(TEST_BATCH + 4 * n++) = 0;
    uint64_t t0 = rdtsc();
    uint32_t r = submit(TEST_BATCH, n * 4);
    if (r != WGPU_SUBMIT_OK) { printf("  submit refused: %lu\n", (unsigned long)r); return 0; }
    if (!wait_fence(fence_value, 1000)) return 0;
    uint32_t first = us_since(t0);
    for (i = 0; i < 16; i++) right += AP32(TARGET + 4 * i) == 0x5eed0000u + i;
    printf("  %lu of 16 stores arrived, first submit-to-fence %lu us\n", (unsigned long)right, (unsigned long)first);
    if (right != 16) return 0;
    uint32_t sum = 0;
    for (i = 0; i < 200; i++) {
        t0 = rdtsc();
        if (submit(TEST_BATCH, n * 4) != WGPU_SUBMIT_OK || !wait_fence(fence_value, 1000)) return 0;
        sum += us_since(t0);
    }
    t0 = rdtsc();
    for (i = 0; i < 1000; i++)
        if (submit(TEST_BATCH, n * 4) != WGPU_SUBMIT_OK) { printf("  queued submit %lu refused\n", (unsigned long)i); return 0; }
    if (!wait_fence(fence_value, 2000)) return 0;
    printf("  submit-to-fence %lu us on average (200); 1000 queued in %lu us\n", (unsigned long)(sum / 200),
           (unsigned long)us_since(t0));
    return 1;
}

/* ---- a Gouraud triangle (gpu3d.c's build(), untextured) ---- */

static uint32_t batch[TRI_BYTES / 4];
#define STATE_OFFSET (32u * 1024u)

typedef struct { uint32_t cmd, state; } Cursor;

static void out(Cursor *c, uint32_t v) { batch[c->cmd / 4] = v; c->cmd += 4; }
static void zeros(Cursor *c, unsigned n) { while (n--) out(c, 0); }
static void *ptr(uint32_t offset) { return (uint8_t *)batch + offset; }

static uint32_t state(Cursor *c, uint32_t bytes, uint32_t align)
{
    c->state = (c->state + align - 1) & ~(align - 1);
    uint32_t at = c->state;
    memset(ptr(at), 0, bytes);
    c->state += bytes;
    return at;
}

static void put_f(uint32_t *at, float f)
{
    memcpy(ptr(*at), &f, 4);
    *at += 4;
}

typedef struct { float x, y, r, g, b, a; } Vertex;

static uint32_t build_triangle(uint32_t batch_gfx, uint32_t kernel_gfx, uint32_t color, uint32_t pitch,
                               uint32_t width, uint32_t height, const Vertex *v)
{
    Cursor c = { 0, STATE_OFFSET };
    uint32_t bt = state(&c, 8, 32);
    {
        uint32_t ss_at = state(&c, sizeof(struct gen8_surface_state), 64);
        struct gen8_surface_state *ss = ptr(ss_at);
        ss->ss0.surface_type = SURFACE_2D;
        ss->ss0.surface_format = SURFACEFORMAT_B5G6R5_UNORM;
        ss->ss0.render_cache_read_write = 1;
        ss->ss0.vertical_alignment = 1;
        ss->ss0.horizontal_alignment = 1;
        ss->ss1.memory_object_control = CHV_MOCS_WB | CHV_MOCS_L3;
        ss->ss8.base_addr = color;
        ss->ss2.height = height - 1;
        ss->ss2.width = width - 1;
        ss->ss3.pitch = pitch - 1;
        ss->ss7.shader_chanel_select_r = 4;
        ss->ss7.shader_chanel_select_g = 5;
        ss->ss7.shader_chanel_select_b = 6;
        ss->ss7.shader_chanel_select_a = 7;
        batch[bt / 4] = ss_at;
    }
    uint32_t vb = state(&c, 3 * 32, 8), at = vb;
    for (unsigned i = 0; i < 3; i++) {
        put_f(&at, v[i].x); put_f(&at, v[i].y);
        put_f(&at, v[i].r); put_f(&at, v[i].g); put_f(&at, v[i].b); put_f(&at, v[i].a);
        put_f(&at, 0); put_f(&at, 0);
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
    out(&c, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_PS); out(&c, 0);
    out(&c, GEN6_3DSTATE_WM | (2 - 2));
    out(&c, GEN8_3DSTATE_PS_PERSPECTIVE_PIXEL_BARYCENTRIC | GEN7_WM_STATISTICS_ENABLE);
    out(&c, GEN6_3DSTATE_CONSTANT_PS | (11 - 2)); zeros(&c, 10);
    out(&c, GEN7_3DSTATE_PS | (12 - 2));
    out(&c, 0); out(&c, 0);                          /* kernel at the instruction base */
    out(&c, 1u << GEN6_3DSTATE_WM_BINDING_TABLE_ENTRY_COUNT_SHIFT);
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
    out(&c, 3 * 32);
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
    out(&c, GEN4_3DPRIMITIVE | (7 - 2)); out(&c, 0); out(&c, 3); out(&c, 0); out(&c, 1); out(&c, 0); out(&c, 0);
    out(&c, MI_BATCH_BUFFER_END);
    if (c.cmd & 7) out(&c, 0);
    return c.cmd > STATE_OFFSET || c.state > TRI_BYTES ? 0 : c.state;
}

static uint16_t rgb565(float r, float g, float b)
{
    return (uint16_t)((unsigned)(r * 31.0f + 0.5f) << 11 | (unsigned)(g * 63.0f + 0.5f) << 5 |
                      (unsigned)(b * 31.0f + 0.5f));
}

static int test_triangle(void)
{
    int vga = find(0x11111234u, 0);
    if (vga < 0) { printf("  no QEMU VGA\n"); return 0; }
    uint32_t lfb_phys = pci_read((unsigned)vga, 0x10) & ~15u;
    volatile uint16_t *lfb = map_phys(lfb_phys, 640 * 480 * 2);
    if (!lfb) { printf("  could not map the frame buffer at %08lx\n", (unsigned long)lfb_phys); return 0; }
    for (unsigned i = 0; i < sizeof kernel_gouraud / 4; i++) AP32(KERNEL + 4 * i) = kernel_gouraud[i];
    const Vertex v[3] = {
        { 320, 40, 1, 0, 0, 1 },
        { 600, 440, 0, 1, 0, 1 },
        { 40, 440, 0, 0, 1, 1 },
    };
    uint32_t bytes = build_triangle(ap_gfx + TRI_BATCH, ap_gfx + KERNEL, lfb_gfx, 1280, 640, 480, v);
    if (!bytes) { printf("  the batch does not fit\n"); return 0; }
    memcpy((void *)(ap + TRI_BATCH), batch, bytes);

    union REGS r;
    memset(&r, 0, sizeof r);
    r.w.ax = 0x4f02;
    r.w.bx = 0x4111;                                  /* 640x480x16, linear */
    int386(0x10, &r, &r);
    if (r.w.ax != 0x004f) { printf("  VBE mode 111h refused (%04x)\n", r.w.ax); return 0; }
    for (unsigned i = 0; i < 640 * 480; i++) lfb[i] = 0x1082;   /* dark grey */
    flush_range(lfb, 640 * 480 * 2);                  /* our writes reach memory before the GPU's */
    uint64_t t0 = rdtsc();
    uint32_t res = submit(TRI_BATCH, bytes);
    int ok = res == WGPU_SUBMIT_OK && wait_fence(fence_value, 2000);
    uint32_t took = us_since(t0);
    flush_range(lfb, 640 * 480 * 2);                  /* drop lines older than the GPU's writes */
    /* inside: the centroid; outside: the corners of the screen */
    uint16_t centre = lfb[(307 * 640) + 320], corner = lfb[0], want = rgb565(1.0f / 3, 1.0f / 3, 1.0f / 3);
    unsigned inside = 0;
    for (unsigned y = 100; y < 430; y += 10)
        for (unsigned x = 0; x < 640; x += 10) {
            float fy = y + 0.5f, fx = x + 0.5f;
            float half = (fy - 40.0f) * 280.0f / 400.0f;                   /* half width at this row */
            if (fx > 320.0f - half + 2 && fx < 320.0f + half - 2) inside += lfb[y * 640 + x] != 0x1082;
        }
    getch();
    memset(&r, 0, sizeof r);
    r.w.ax = 0x0003;
    int386(0x10, &r, &r);
    printf("  triangle: submit %lu, %s in %lu us; centre %04x (about %04x), corner %04x, %u sampled inside drawn\n",
           (unsigned long)res, ok ? "done" : "NOT done", (unsigned long)took, centre, want, corner, inside);
    return ok && corner == 0x1082 && centre != 0x1082;
}

int main(int argc, char **argv)
{
    int tri = argc > 1 && !stricmp(argv[1], "tri");
    calibrate();
    int dev = find(0x22b08086u, WGPU_SUBSYSTEM_ID << 16 | WGPU_SUBSYSTEM_VENDOR);
    if (dev < 0) { printf("WGPU: no render channel on the PCI bus (config.ini wgpu=64?)\n"); return 1; }
    uint32_t bar0 = pci_read((unsigned)dev, 0x10) & ~15u, bar2 = pci_read((unsigned)dev, 0x18) & ~15u;
    printf("WGPU at 00:%02x.0: registers %08lx, aperture %08lx; TSC %lu MHz\n", dev, (unsigned long)bar0,
           (unsigned long)bar2, (unsigned long)(tsc_per_ms / 1000));
    regs = map_phys(bar0, 4096);
    ap = map_phys(bar2, AP_MAP);
    if (!regs || !ap) { printf("  DPMI could not map the BARs\n"); return 1; }
    if (REG(WGPU_REG_MAGIC) != WGPU_MAGIC) { printf("  magic %08lx\n", (unsigned long)REG(WGPU_REG_MAGIC)); return 1; }
    uint32_t status = REG(WGPU_REG_STATUS);
    ap_gfx = REG(WGPU_REG_APERTURE_GFX);
    lfb_gfx = REG(WGPU_REG_LFB_GFX);
    printf("  version %08lx, GPU %08lx, status %lx, aperture %lu MB at %08lx, frame buffer %lu KB at %08lx\n",
           (unsigned long)REG(WGPU_REG_VERSION), (unsigned long)REG(WGPU_REG_GPU_ID), (unsigned long)status,
           (unsigned long)(REG(WGPU_REG_APERTURE_SIZE) >> 20), (unsigned long)ap_gfx,
           (unsigned long)(REG(WGPU_REG_LFB_SIZE) >> 10), (unsigned long)lfb_gfx);
    if (!(status & WGPU_STATUS_READY)) { printf("  engine not ready\n"); return 1; }
    if (status & WGPU_STATUS_ERROR) REG(WGPU_REG_ERROR_ACK) = 1;
    REG(WGPU_REG_FENCE_OFFSET) = FENCE;
    fence_value = AP32(FENCE);
    int ok = test_stores();
    if (ok && tri) ok = test_triangle();
    printf("WGPU guest test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
