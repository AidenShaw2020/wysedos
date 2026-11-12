#include "intel_gpu.h"
#include <stdio.h>
#include <string.h>

#define INTEL_GPU_MMIO_SIZE (16ull << 20)

bool intel_gpu_decode_pci(const IntelGpuPCI *pci, IntelGpuInfo *info)
{
    if (!info) return false;
    memset(info, 0, sizeof(*info));
    if (!pci) return false;
    info->identity = pci->identity;
    info->command = pci->command & 0xffff;
    info->ggc = pci->ggc;
    if (pci->identity != 0x22b08086 || !(pci->command & 2) ||
        (pci->mmio_low & 7) != 4 || (pci->aperture_low & 7) != 4) return false;
    uint64_t mmio = ((uint64_t)pci->mmio_high << 32) | (pci->mmio_low & ~15u);
    uint64_t aperture = ((uint64_t)pci->aperture_high << 32) | (pci->aperture_low & ~15u);
    if (!mmio || !aperture || (mmio & (INTEL_GPU_MMIO_SIZE - 1)) || (aperture & 4095) ||
        mmio > UINT64_MAX - (INTEL_GPU_MMIO_SIZE - 1) || aperture > UINT64_MAX - 4095 ||
        (aperture >= mmio && aperture - mmio < INTEL_GPU_MMIO_SIZE)) return false;
    info->mmio = mmio;
    info->aperture = aperture;
    info->bus_master_enabled = (pci->command & 4) != 0;
    return true;
}

#ifdef WYSE_BUILD
static uint32_t read_gpu_pci(unsigned reg)
{
    uint32_t previous, result;
    uint32_t selector = 0x80001000u | (reg & 0xfc);
    __asm__ volatile("inl %1, %0" : "=a"(previous) : "Nd"((uint16_t)0xcf8));
    __asm__ volatile("outl %0, %1" :: "a"(selector), "Nd"((uint16_t)0xcf8));
    __asm__ volatile("inl %1, %0" : "=a"(result) : "Nd"((uint16_t)0xcfc));
    __asm__ volatile("outl %0, %1" :: "a"(previous), "Nd"((uint16_t)0xcf8));
    return result;
}

void intel_gpu_log_probe(void)
{
    IntelGpuPCI pci = {
        read_gpu_pci(0), read_gpu_pci(4), read_gpu_pci(0x10), read_gpu_pci(0x14),
        read_gpu_pci(0x18), read_gpu_pci(0x1c), read_gpu_pci(0x50)
    };
    IntelGpuInfo info;
    if (!intel_gpu_decode_pci(&pci, &info)) {
        printf("intel-gpu: passive probe unavailable, pci=%08x command=%04x\n",
            info.identity, info.command);
        return;
    }
    volatile const uint64_t *ggtt = (volatile const uint64_t *)(uintptr_t)(info.mmio + (8u << 20));
    info.ggtt_first = ggtt[0];
    info.ggtt_vga_window = ggtt[0x2000];
    printf("intel-gpu: passive pci=%08x command=%04x ggc=%08x mmio=%llx aperture=%llx\n",
        info.identity, info.command, info.ggc, (unsigned long long)info.mmio,
        (unsigned long long)info.aperture);
    printf("intel-gpu: ggtt[0]=%llx ggtt[8192]=%llx; ownership unverified, submission disabled\n",
        (unsigned long long)info.ggtt_first, (unsigned long long)info.ggtt_vga_window);
}
/*
 * First commands on the GPU: the blitter (BCS) fills 32x32 pixels in ordinary
 * RAM, away from the screen ("gpu blt"); "gpu status" only reports.
 *
 * Nothing here touches the display engine, the GOP framebuffer or the GGTT
 * entries hwvga.c uses: the three pages are entered in the GGTT at 1 GB,
 * beyond the 256 MB aperture the CPU (and the GOP) see through, and their old
 * entries come back afterwards. The legacy ring buffer is used (Gen8 still has
 * it), so no execlist context has to be built. The test stops at the first
 * thing that is not as expected - an engine already in use, execlists on, a
 * forcewake that does not answer - and says so.
 *
 * Register offsets as in the Cherryview/Broadwell documentation (and i915):
 * BCS ring at 0x22000 (TAIL +0x30, HEAD +0x34, START +0x38, CTL +0x3c, ACTHD
 * +0x74, IPEHR +0x68, MI_MODE +0x9c, MODE +0x29c), forcewake render/media
 * 0x1300b0/0x1300b8 with their acknowledgements 4 bytes on, GGTT at BAR0 +
 * 8 MB, GFX_FLSH_CNTL 0x101008.
 */
#include "../include/efi.h"
extern EFI_BOOT_SERVICES *gBS;
uint64_t wyse_ticks_us(void);

#define RCS 0x2000u
#define BCS 0x22000u
#define RING_TAIL  0x30u
#define RING_HEAD  0x34u
#define RING_START 0x38u
#define RING_CTL   0x3cu
#define RING_IPEHR 0x68u
#define RING_ACTHD 0x74u
#define RING_MI_MODE 0x9cu
#define RING_MODE  0x29cu
#define FW_RENDER  0x1300b0u
#define FW_MEDIA   0x1300b8u
#define GGTT_TEST_INDEX 0x40000u    /* 1 GB into the GPU's global address space */

static volatile uint8_t *gpu_mmio;

static uint32_t gr(uint32_t reg) { return *(volatile uint32_t *)(gpu_mmio + reg); }
static void gw(uint32_t reg, uint32_t value) { *(volatile uint32_t *)(gpu_mmio + reg) = value; }

static bool gpu_open(IntelGpuInfo *info)
{
    IntelGpuPCI pci = {
        read_gpu_pci(0), read_gpu_pci(4), read_gpu_pci(0x10), read_gpu_pci(0x14),
        read_gpu_pci(0x18), read_gpu_pci(0x1c), read_gpu_pci(0x50)
    };
    if (!intel_gpu_decode_pci(&pci, info)) {
        printf("gpu: no Cherryview graphics with its BARs decoding\n");
        return false;
    }
    gpu_mmio = (volatile uint8_t *)(uintptr_t)info->mmio;
    return true;
}

/* Wake one power well; true when the acknowledgement came. */
static bool forcewake(uint32_t reg, bool on)
{
    gw(reg, on ? 0x00010001u : 0x00010000u);
    uint64_t until = wyse_ticks_us() + 50000;
    while (wyse_ticks_us() < until)
        if (((gr(reg + 4) & 1u) != 0) == on) return true;
    return false;
}

static void ring_report(const char *name, uint32_t base)
{
    printf("gpu: %s ctl %08x head %08x tail %08x start %08x acthd %08x ipehr %08x mi_mode %08x mode %08x\n",
           name, gr(base + RING_CTL), gr(base + RING_HEAD), gr(base + RING_TAIL), gr(base + RING_START),
           gr(base + RING_ACTHD), gr(base + RING_IPEHR), gr(base + RING_MI_MODE), gr(base + RING_MODE));
}

static bool gpu_wake(void)
{
    bool render = forcewake(FW_RENDER, true), media = forcewake(FW_MEDIA, true);
    printf("gpu: forcewake render %s, media %s\n", render ? "acknowledged" : "NO ANSWER",
           media ? "acknowledged" : "NO ANSWER");
    return render && media;
}

static void gpu_sleep(void)
{
    forcewake(FW_RENDER, false);
    forcewake(FW_MEDIA, false);
}

static void flush_lines(const volatile void *p, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i += 64)
        __asm__ volatile("clflush (%0)" :: "r"((const volatile uint8_t *)p + i) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

void intel_gpu_status(void)
{
    IntelGpuInfo info;
    if (!gpu_open(&info)) return;
    uint32_t ggms = (info.ggc >> 8) & 3;
    printf("gpu: ggc %08x: GGTT %u MB (%u entries), stolen %u MB\n", info.ggc,
           ggms ? 1u << ggms : 0, ggms ? (1u << (20 + ggms)) / 8 : 0, ((info.ggc >> 3) & 0x1f) * 32);
    bool awake = gpu_wake();
    ring_report("rcs", RCS);
    ring_report("bcs", BCS);
    if (awake) gpu_sleep();
}

void intel_gpu_blt_test(void)
{
    IntelGpuInfo info;
    if (!gpu_open(&info)) return;
    if (!info.bus_master_enabled) { printf("gpu blt: bus mastering is off, stop\n"); return; }
    uint32_t ggms = (info.ggc >> 8) & 3;
    uint32_t entries = ggms ? (1u << (20 + ggms)) / 8 : 0;
    if (entries < GGTT_TEST_INDEX + 3) {
        printf("gpu blt: GGTT has %u entries, the test needs %u, stop\n", entries, GGTT_TEST_INDEX + 3);
        return;
    }
    if (!gpu_wake()) { gpu_sleep(); printf("gpu blt: the GT did not wake, stop\n"); return; }
    ring_report("bcs before", BCS);
    uint32_t ctl = gr(BCS + RING_CTL), mode = gr(BCS + RING_MODE);
    if (mode & (1u << 15)) { printf("gpu blt: execlists are on, stop\n"); gpu_sleep(); return; }
    if ((ctl & 1) && ((gr(BCS + RING_HEAD) ^ gr(BCS + RING_TAIL)) & 0x1ffff8u)) {
        printf("gpu blt: the blitter ring is in use, stop\n");
        gpu_sleep();
        return;
    }

    EFI_PHYSICAL_ADDRESS pages = 0xffffffffu;
    if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 3, &pages))) {
        printf("gpu blt: no memory, stop\n");
        gpu_sleep();
        return;
    }
    volatile uint32_t *ring = (volatile uint32_t *)(uintptr_t)pages;
    volatile uint32_t *target = ring + 1024, *status = ring + 2048;
    const uint32_t gpu_ring = GGTT_TEST_INDEX * 4096u;
    const uint32_t gpu_target = gpu_ring + 4096u, gpu_status = gpu_ring + 8192u;
    const uint32_t color = 0x00ff8040u;

    for (uint32_t i = 0; i < 1024; i++) { ring[i] = 0; target[i] = 0x11111111u; status[i] = 0; }
    uint32_t n = 0;
    ring[n++] = (2u << 29) | (0x50u << 22) | (3u << 20) | (7 - 2);  /* XY_COLOR_BLT, RGB and alpha */
    ring[n++] = (3u << 24) | (0xf0u << 16) | 128;                   /* 32 bpp, PATCOPY, pitch */
    ring[n++] = 0;                                                  /* x1, y1 */
    ring[n++] = (32u << 16) | 32;                                   /* x2, y2 */
    ring[n++] = gpu_target;
    ring[n++] = 0;
    ring[n++] = color;
    ring[n++] = (0x26u << 23) | 2;                                  /* MI_FLUSH_DW */
    ring[n++] = 0;
    ring[n++] = 0;
    ring[n++] = 0;
    ring[n++] = (0x20u << 23) | (1u << 22) | 2;                     /* MI_STORE_DATA_IMM, GGTT */
    ring[n++] = gpu_status;
    ring[n++] = 0;
    ring[n++] = 0xc0de600du;
    while (n & 3) ring[n++] = 0;                                     /* MI_NOOP */
    flush_lines(ring, 3 * 4096);

    volatile uint64_t *ggtt = (volatile uint64_t *)(uintptr_t)(info.mmio + (8u << 20));
    uint64_t saved[3];
    for (unsigned i = 0; i < 3; i++) {
        saved[i] = ggtt[GGTT_TEST_INDEX + i];
        ggtt[GGTT_TEST_INDEX + i] = (pages + 4096u * i) | 1u;
    }
    (void)ggtt[GGTT_TEST_INDEX + 2];
    gw(0x101008, 1);
    (void)gr(0x101008);
    printf("gpu blt: ggtt[%x..] was %llx %llx %llx, now pages at %llx\n", GGTT_TEST_INDEX,
           (unsigned long long)saved[0], (unsigned long long)saved[1], (unsigned long long)saved[2],
           (unsigned long long)pages);

    gw(BCS + RING_CTL, 0);
    gw(BCS + RING_HEAD, 0);
    gw(BCS + RING_TAIL, 0);
    gw(BCS + RING_START, gpu_ring);
    gw(BCS + RING_MI_MODE, (1u << 8) << 16);                        /* STOP_RING off */
    gw(BCS + RING_CTL, 1);                                          /* 4 KB, valid */
    if (gr(BCS + RING_START) != gpu_ring || !(gr(BCS + RING_CTL) & 1)) {
        ring_report("bcs set-up", BCS);
        printf("gpu blt: the ring registers did not take, stop\n");
        gw(BCS + RING_CTL, 0);
        for (unsigned i = 0; i < 3; i++) ggtt[GGTT_TEST_INDEX + i] = saved[i];
        gw(0x101008, 1);
        gpu_sleep();
        return;
    }
    uint64_t t0 = wyse_ticks_us();
    gw(BCS + RING_TAIL, n * 4);
    bool done = false;
    while (wyse_ticks_us() - t0 < 200000) {
        flush_lines(status, 64);
        if (status[0] == 0xc0de600du) { done = true; break; }
    }
    uint64_t took = wyse_ticks_us() - t0;
    ring_report("bcs after", BCS);
    if (!done) {
        printf("gpu blt: NO completion in 200 ms (status %08x); ring left stopped, pages %llx kept\n",
               status[0], (unsigned long long)pages);
        gw(BCS + RING_CTL, 0);
        gpu_sleep();
        return;
    }
    flush_lines(target, 4096);
    unsigned right = 0, first_bad = 1024;
    for (unsigned i = 0; i < 1024; i++) {
        if (target[i] == color) right++;
        else if (first_bad == 1024) first_bad = i;
    }
    printf("gpu blt: completed in %llu us, %u of 1024 pixels %08x%s", (unsigned long long)took, right, color,
           right == 1024 ? " - PASS\n" : "");
    if (right != 1024) printf(", first wrong at %u: %08x - FAIL\n", first_bad, target[first_bad]);
    gw(BCS + RING_CTL, 0);
    for (unsigned i = 0; i < 3; i++) ggtt[GGTT_TEST_INDEX + i] = saved[i];
    (void)ggtt[GGTT_TEST_INDEX + 2];
    gw(0x101008, 1);
    gpu_sleep();
    gBS->FreePages(pages, 3);
}

/*
 * The 3D engine (RCS): "gpu copy" draws a 64x64 rectangle whose pixel shader
 * samples a 64x64 source texture and writes it to a destination surface, both
 * in RAM away from the screen, then compares the destination with the source.
 *
 * The command sequence and the pixel-shader kernel are IGT GPU Tools'
 * gen8_render_copyfunc (lib/rendercopy_gen8.c, MIT, see igt/README.md).
 * There, i915 relocations put the batch's GPU address into the commands; here
 * every buffer has a fixed GGTT address, so the same values are written
 * directly. The batch runs from the ring through MI_BATCH_BUFFER_START in the
 * GGTT (privileged), ends with a PIPE_CONTROL flush and a completion dword.
 */
#include "igt/gen8_render.h"

#define BASE_ADDRESS_MODIFY 1u
#define GPU_PAGES 12u                 /* ring, batch x2, source x4, destination x4, status */
#define COPY_W 64u
#define COPY_H 64u

/* lib/i915/shaders/ps/blit.g7a, as in rendercopy_gen8.c */
static const uint32_t copy_ps_kernel[][4] = {
    { 0x0080005a, 0x2f403ae8, 0x3a0000c0, 0x008d0040 },
    { 0x0080005a, 0x2f803ae8, 0x3a0000d0, 0x008d0040 },
    { 0x02800031, 0x2e203a48, 0x0e8d0f40, 0x08840001 },
    { 0x05800031, 0x20003a40, 0x0e8d0e20, 0x90031000 },
};

/* The same file's other kernel: every channel 1.0, no texture ("gpu fill") */
static const uint32_t fill_ps_kernel[][4] = {
    { 0x00600001, 0x2e000608, 0x00000000, 0x3f800000 },
    { 0x00600001, 0x2e200608, 0x00000000, 0x3f800000 },
    { 0x00600001, 0x2e400608, 0x00000000, 0x3f800000 },
    { 0x00600001, 0x2e600608, 0x00000000, 0x3f800000 },
    { 0x00600001, 0x2e800608, 0x00000000, 0x3f800000 },
    { 0x00600001, 0x2ea00608, 0x00000000, 0x3f800000 },
    { 0x00600001, 0x2ec00608, 0x00000000, 0x3f800000 },
    { 0x00600001, 0x2ee00608, 0x00000000, 0x3f800000 },
    { 0x05800031, 0x200022e0, 0x0e000e00, 0x90031000 },
};

/* Pipeline statistics (low dwords), to see how far a draw got */
static const struct { const char *name; uint32_t reg; } gpu_stats[] = {
    { "ia_vert", 0x2310 }, { "ia_prim", 0x2318 }, { "vs", 0x2320 }, { "cl_inv", 0x2338 },
    { "cl_prim", 0x2340 }, { "ps", 0x2348 }, { "ps_depth", 0x2350 },
};

static void stats_read(uint32_t *v)
{
    for (unsigned i = 0; i < sizeof gpu_stats / sizeof gpu_stats[0]; i++) v[i] = gr(gpu_stats[i].reg);
}

static void stats_report(const uint32_t *before, const uint32_t *after)
{
    char line[200];
    int len = snprintf(line, sizeof line, "gpu: pipeline");
    for (unsigned i = 0; i < sizeof gpu_stats / sizeof gpu_stats[0]; i++)
        len += snprintf(line + len, sizeof line - len, " %s %u", gpu_stats[i].name, after[i] - before[i]);
    printf("%s\n", line);
}

typedef struct {
    volatile uint32_t *map;           /* CPU view of the batch */
    uint32_t gpu;                     /* its GGTT address */
    uint32_t cmd, state;              /* byte offsets: commands from 0, state from 4096 */
} Batch;

static void out(Batch *b, uint32_t v) { b->map[b->cmd / 4] = v; b->cmd += 4; }

static uint32_t state_alloc(Batch *b, uint32_t bytes, uint32_t align)
{
    b->state = (b->state + align - 1) & ~(align - 1);
    uint32_t offset = b->state;
    for (uint32_t i = 0; i < bytes; i += 4) b->map[(offset + i) / 4] = 0;
    b->state += bytes;
    return offset;
}

static void *state_ptr(Batch *b, uint32_t offset) { return (void *)(uintptr_t)((volatile uint8_t *)b->map + offset); }

/* Memory object control for the two surfaces ("gpu fill 0": uncached) */
uint32_t g_gpu_copy_mocs = CHV_MOCS_WB | CHV_MOCS_L3;

static uint32_t bind_surface(Batch *b, uint32_t gpu_address, bool dst)
{
    uint32_t offset = state_alloc(b, sizeof(struct gen8_surface_state), 64);
    struct gen8_surface_state *ss = state_ptr(b, offset);
    ss->ss0.surface_type = SURFACE_2D;
    ss->ss0.surface_format = SURFACEFORMAT_B8G8R8A8_UNORM;
    ss->ss0.render_cache_read_write = 1;
    ss->ss0.vertical_alignment = 1;
    ss->ss0.horizontal_alignment = 1;
    ss->ss1.memory_object_control = g_gpu_copy_mocs;
    ss->ss8.base_addr = gpu_address;
    ss->ss9.base_addr_hi = 0;
    ss->ss2.height = COPY_H - 1;
    ss->ss2.width = COPY_W - 1;
    ss->ss3.pitch = COPY_W * 4 - 1;
    ss->ss7.shader_chanel_select_r = 4;
    ss->ss7.shader_chanel_select_g = 5;
    ss->ss7.shader_chanel_select_b = 6;
    ss->ss7.shader_chanel_select_a = 7;
    (void)dst;
    return offset;
}

static void put_float(Batch *b, uint32_t *at, float f)
{
    union { float f; uint32_t u; } v = { f };
    b->map[*at / 4] = v.u;
    *at += 4;
}

static void put_xy(Batch *b, uint32_t *at, int16_t x, int16_t y)
{
    b->map[*at / 4] = (uint32_t)(uint16_t)y << 16 | (uint16_t)x;
    *at += 4;
}

static void build_copy_batch(Batch *b, uint32_t src, uint32_t dst, uint32_t status_gpu, bool fill)
{
    b->cmd = 0;
    b->state = 4096;
    /* state, in the order rendercopy_gen8.c builds it */
    uint32_t bt = state_alloc(b, 8, 32);
    uint32_t dst_ss = bind_surface(b, dst, true), src_ss = bind_surface(b, src, false);
    b->map[bt / 4] = dst_ss;
    b->map[bt / 4 + 1] = src_ss;
    uint32_t sampler = state_alloc(b, sizeof(struct gen8_sampler_state), 64);
    {
        struct gen8_sampler_state *ss = state_ptr(b, sampler);
        ss->ss0.min_filter = GEN4_MAPFILTER_NEAREST;
        ss->ss0.mag_filter = GEN4_MAPFILTER_NEAREST;
        ss->ss3.r_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
        ss->ss3.s_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
        ss->ss3.t_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
        ss->ss3.non_normalized_coord = 0;
    }
    const uint32_t *code = fill ? (const uint32_t *)fill_ps_kernel : (const uint32_t *)copy_ps_kernel;
    const uint32_t code_bytes = fill ? sizeof(fill_ps_kernel) : sizeof(copy_ps_kernel);
    uint32_t kernel = state_alloc(b, code_bytes, 64);
    for (uint32_t i = 0; i < code_bytes / 4; i++) b->map[kernel / 4 + i] = code[i];
    uint32_t vb = state_alloc(b, 3 * 12, 8), at = vb;
    put_xy(b, &at, COPY_W, COPY_H); put_float(b, &at, 1.0f); put_float(b, &at, 1.0f);
    put_xy(b, &at, 0, COPY_H);      put_float(b, &at, 0.0f); put_float(b, &at, 1.0f);
    put_xy(b, &at, 0, 0);           put_float(b, &at, 0.0f); put_float(b, &at, 0.0f);
    uint32_t cc_state = state_alloc(b, sizeof(struct gen6_color_calc_state), 64);
    uint32_t blend = state_alloc(b, sizeof(struct gen8_blend_state), 64);
    {
        struct gen8_blend_state *bs = state_ptr(b, blend);
        for (int i = 0; i < 16; i++) {
            bs->bs[i].dest_blend_factor = GEN6_BLENDFACTOR_ZERO;
            bs->bs[i].source_blend_factor = GEN6_BLENDFACTOR_ONE;
            bs->bs[i].color_blend_func = GEN6_BLENDFUNCTION_ADD;
            bs->bs[i].pre_blend_color_clamp = 1;
            bs->bs[i].color_buffer_blend = 0;
        }
    }
    uint32_t cc_vp = state_alloc(b, sizeof(struct gen4_cc_viewport), 32);
    {
        struct gen4_cc_viewport *vp = state_ptr(b, cc_vp);
        vp->min_depth = -1.e35f;
        vp->max_depth = 1.e35f;
    }
    uint32_t sf_vp = state_alloc(b, sizeof(struct gen7_sf_clip_viewport), 64);
    {
        struct gen7_sf_clip_viewport *v = state_ptr(b, sf_vp);
        v->guardband.xmin = 0;
        v->guardband.xmax = 1.0f;
        v->guardband.ymin = 0;
        v->guardband.ymax = 1.0f;
    }
    uint32_t scissor = state_alloc(b, sizeof(struct gen6_scissor_rect), 64);

    /* commands */
    out(b, G4X_PIPELINE_SELECT | PIPELINE_SELECT_3D);
    out(b, GEN6_3DSTATE_VF_STATISTICS | 1);   /* count what the pipeline does */
    out(b, GEN4_STATE_SIP | (3 - 2)); out(b, 0); out(b, 0);
    out(b, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_VS); out(b, 0);
    out(b, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_HS); out(b, 0);
    out(b, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_DS); out(b, 0);
    out(b, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_GS); out(b, 0);
    out(b, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_PS); out(b, 0);
    out(b, GEN4_STATE_BASE_ADDRESS | (16 - 2));
    out(b, 0 | BASE_ADDRESS_MODIFY); out(b, 0);          /* general */
    out(b, 0 | BASE_ADDRESS_MODIFY);                     /* stateless data port */
    out(b, b->gpu | BASE_ADDRESS_MODIFY); out(b, 0);     /* surface */
    out(b, b->gpu | BASE_ADDRESS_MODIFY); out(b, 0);     /* dynamic */
    out(b, 0); out(b, 0);                                /* indirect */
    out(b, b->gpu | BASE_ADDRESS_MODIFY); out(b, 0);     /* instruction */
    /* sizes in pages: the dynamic state and the kernel lie in the batch's second page */
    out(b, 0xfffff000u | 1); out(b, 2u << 12 | 1); out(b, 0xfffff000u | 1); out(b, 2u << 12 | 1);
    out(b, GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_CC); out(b, cc_vp);
    out(b, GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP); out(b, sf_vp);
    out(b, GEN7_3DSTATE_URB_VS); out(b, 64 | ((2 - 1) << 16) | (2u << 25));
    out(b, GEN7_3DSTATE_URB_GS); out(b, 2u << 25);
    out(b, GEN7_3DSTATE_URB_HS); out(b, 2u << 25);
    out(b, GEN7_3DSTATE_URB_DS); out(b, 2u << 25);
    out(b, GEN7_3DSTATE_BLEND_STATE_POINTERS); out(b, blend | 1);
    out(b, GEN6_3DSTATE_CC_STATE_POINTERS); out(b, cc_state | 1);
    out(b, GEN8_3DSTATE_MULTISAMPLE); out(b, 0);
    out(b, GEN6_3DSTATE_SAMPLE_MASK); out(b, 1);
    /* null state: WM_HZ_OP, HS, TE, GS, DS, VS */
    out(b, GEN8_3DSTATE_WM_HZ_OP | (5 - 2)); for (int i = 0; i < 4; i++) out(b, 0);
    out(b, GEN7_3DSTATE_CONSTANT_HS | (11 - 2)); for (int i = 0; i < 10; i++) out(b, 0);
    out(b, GEN7_3DSTATE_HS | (9 - 2)); for (int i = 0; i < 8; i++) out(b, 0);
    out(b, GEN7_3DSTATE_BINDING_TABLE_POINTERS_HS); out(b, 0);
    out(b, GEN8_3DSTATE_SAMPLER_STATE_POINTERS_HS); out(b, 0);
    out(b, GEN7_3DSTATE_TE | (4 - 2)); out(b, 0); out(b, 0); out(b, 0);
    out(b, GEN6_3DSTATE_CONSTANT_GS | (11 - 2)); for (int i = 0; i < 10; i++) out(b, 0);
    out(b, GEN6_3DSTATE_GS | (10 - 2)); for (int i = 0; i < 9; i++) out(b, 0);
    out(b, GEN7_3DSTATE_BINDING_TABLE_POINTERS_GS); out(b, 0);
    out(b, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_GS); out(b, 0);
    out(b, GEN7_3DSTATE_CONSTANT_DS | (11 - 2)); for (int i = 0; i < 10; i++) out(b, 0);
    out(b, GEN7_3DSTATE_DS | (9 - 2)); for (int i = 0; i < 8; i++) out(b, 0);
    out(b, GEN7_3DSTATE_BINDING_TABLE_POINTERS_DS); out(b, 0);
    out(b, GEN8_3DSTATE_SAMPLER_STATE_POINTERS_DS); out(b, 0);
    out(b, GEN7_3DSTATE_BINDING_TABLE_POINTERS_VS); out(b, 0);
    out(b, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_VS); out(b, 0);
    out(b, GEN6_3DSTATE_CONSTANT_VS | (11 - 2)); for (int i = 0; i < 10; i++) out(b, 0);
    out(b, GEN6_3DSTATE_VS | (9 - 2)); for (int i = 0; i < 8; i++) out(b, 0);
    out(b, GEN7_3DSTATE_STREAMOUT | (5 - 2)); for (int i = 0; i < 4; i++) out(b, 0);
    out(b, GEN6_3DSTATE_CLIP | (4 - 2)); out(b, 1u << 10 /* statistics */); out(b, 0); out(b, 0);
    out(b, GEN7_3DSTATE_SBE | (4 - 2));
    out(b, 1u << GEN7_SBE_NUM_OUTPUTS_SHIFT | GEN8_SBE_FORCE_URB_ENTRY_READ_LENGTH |
           GEN8_SBE_FORCE_URB_ENTRY_READ_OFFSET | 1u << GEN7_SBE_URB_ENTRY_READ_LENGTH_SHIFT |
           1u << GEN8_SBE_URB_ENTRY_READ_OFFSET_SHIFT);
    out(b, 0); out(b, 0);
    out(b, GEN8_3DSTATE_SBE_SWIZ | (11 - 2)); for (int i = 0; i < 10; i++) out(b, 0);
    out(b, GEN8_3DSTATE_RASTER | (5 - 2));
    out(b, GEN8_RASTER_FRONT_WINDING_CCW | GEN8_RASTER_CULL_NONE); out(b, 0); out(b, 0); out(b, 0);
    out(b, GEN6_3DSTATE_SF | (4 - 2)); out(b, 0); out(b, 0); out(b, 0);
    out(b, GEN7_3DSTATE_BINDING_TABLE_POINTERS_PS); out(b, bt);
    out(b, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_PS); out(b, sampler);
    out(b, GEN6_3DSTATE_WM | (2 - 2)); out(b, GEN8_3DSTATE_PS_PERSPECTIVE_PIXEL_BARYCENTRIC | GEN7_WM_STATISTICS_ENABLE);
    out(b, GEN6_3DSTATE_CONSTANT_PS | (11 - 2)); for (int i = 0; i < 10; i++) out(b, 0);
    out(b, GEN7_3DSTATE_PS | (12 - 2));
    out(b, kernel); out(b, 0);
    out(b, 1u << GEN6_3DSTATE_WM_SAMPLER_COUNT_SHIFT | 2u << GEN6_3DSTATE_WM_BINDING_TABLE_ENTRY_COUNT_SHIFT);
    out(b, 0); out(b, 0);
    out(b, (63u - 1) << GEN8_3DSTATE_PS_MAX_THREADS_SHIFT | GEN6_3DSTATE_WM_16_DISPATCH_ENABLE);
    out(b, 6u << GEN6_3DSTATE_WM_DISPATCH_START_GRF_0_SHIFT);
    out(b, 0); out(b, 0); out(b, 0); out(b, 0);
    out(b, GEN8_3DSTATE_PS_BLEND | (2 - 2)); out(b, GEN8_PS_BLEND_HAS_WRITEABLE_RT);
    out(b, GEN8_3DSTATE_PS_EXTRA | (2 - 2)); out(b, GEN8_PSX_PIXEL_SHADER_VALID | GEN8_PSX_ATTRIBUTE_ENABLE);
    out(b, GEN8_3DSTATE_SCISSOR_STATE_POINTERS); out(b, scissor);
    out(b, GEN8_3DSTATE_WM_DEPTH_STENCIL | (3 - 2)); out(b, 0); out(b, 0);
    out(b, GEN7_3DSTATE_DEPTH_BUFFER | (8 - 2)); for (int i = 0; i < 7; i++) out(b, 0);
    out(b, GEN8_3DSTATE_HIER_DEPTH_BUFFER | (5 - 2)); for (int i = 0; i < 4; i++) out(b, 0);
    out(b, GEN8_3DSTATE_STENCIL_BUFFER | (5 - 2)); for (int i = 0; i < 4; i++) out(b, 0);
    out(b, GEN7_3DSTATE_CLEAR_PARAMS | (3 - 2)); out(b, 0); out(b, 1);
    out(b, GEN4_3DSTATE_DRAWING_RECTANGLE | (4 - 2)); out(b, 0);
    out(b, (COPY_H - 1) << 16 | (COPY_W - 1)); out(b, 0);
    out(b, GEN4_3DSTATE_VERTEX_BUFFERS | (1 + 4 - 2));
    out(b, 0u << GEN6_VB0_BUFFER_INDEX_SHIFT | GEN8_VB0_BUFFER_ADDR_MOD_EN | 12u << VB0_BUFFER_PITCH_SHIFT);
    out(b, b->gpu + vb); out(b, 0);
    out(b, 3 * 12);
    out(b, GEN4_3DSTATE_VERTEX_ELEMENTS | (3 * 2 + 1 - 2));
    out(b, GEN6_VE0_VALID | SURFACEFORMAT_R32G32B32A32_FLOAT << VE0_FORMAT_SHIFT | 0u << VE0_OFFSET_SHIFT);
    out(b, GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_0_SHIFT | GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_1_SHIFT |
           GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT | GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT);
    out(b, GEN6_VE0_VALID | SURFACEFORMAT_R16G16_SSCALED << VE0_FORMAT_SHIFT | 0u << VE0_OFFSET_SHIFT);
    out(b, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT | GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
           GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT | GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);
    out(b, GEN6_VE0_VALID | SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT | 4u << VE0_OFFSET_SHIFT);
    out(b, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT | GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
           GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT | GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);
    out(b, GEN8_3DSTATE_VF_TOPOLOGY); out(b, _3DPRIM_RECTLIST);
    out(b, GEN8_3DSTATE_VF_INSTANCING | (3 - 2)); out(b, 0); out(b, 0);
    out(b, GEN4_3DPRIMITIVE | (7 - 2)); out(b, 0); out(b, 3); out(b, 0); out(b, 1); out(b, 0); out(b, 0);
    /* render target and data cache to memory, then the completion dword */
    out(b, GEN6_PIPE_CONTROL | (6 - 2));
    out(b, GEN7_PIPE_CONTROL_CS_STALL | GEN6_PIPE_CONTROL_WC_FLUSH | (1u << 5) /* DC flush */);
    out(b, 0); out(b, 0); out(b, 0); out(b, 0);
    out(b, (0x20u << 23) | (1u << 22) | 2);   /* MI_STORE_DATA_IMM, GGTT */
    out(b, status_gpu); out(b, 0); out(b, 0xc0de3d00u);
    out(b, 0x0au << 23);                       /* MI_BATCH_BUFFER_END */
    if (b->cmd & 7) out(b, 0);
}

static void reset_render(void)
{
    gw(0x941c, 1u << 1);                       /* GEN6_GDRST: render domain only */
    uint64_t until = wyse_ticks_us() + 50000;
    while (wyse_ticks_us() < until && (gr(0x941c) & (1u << 1))) {}
    printf("gpu copy: render domain reset %s\n", (gr(0x941c) & (1u << 1)) ? "did not finish" : "done");
}

void intel_gpu_copy_test(bool fill)
{
    IntelGpuInfo info;
    if (!gpu_open(&info)) return;
    if (!info.bus_master_enabled) { printf("gpu copy: bus mastering is off, stop\n"); return; }
    uint32_t ggms = (info.ggc >> 8) & 3;
    if ((ggms ? (1u << (20 + ggms)) / 8 : 0) < GGTT_TEST_INDEX + GPU_PAGES) {
        printf("gpu copy: GGTT too small, stop\n");
        return;
    }
    if (!gpu_wake()) { gpu_sleep(); printf("gpu copy: the GT did not wake, stop\n"); return; }
    ring_report("rcs before", RCS);
    if (gr(RCS + RING_MODE) & (1u << 15)) { printf("gpu copy: execlists are on, stop\n"); gpu_sleep(); return; }
    if ((gr(RCS + RING_CTL) & 1) && ((gr(RCS + RING_HEAD) ^ gr(RCS + RING_TAIL)) & 0x1ffff8u)) {
        printf("gpu copy: the render ring is in use, stop\n");
        gpu_sleep();
        return;
    }
    EFI_PHYSICAL_ADDRESS pages = 0xffffffffu;
    if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, GPU_PAGES, &pages))) {
        printf("gpu copy: no memory, stop\n");
        gpu_sleep();
        return;
    }
    volatile uint32_t *base = (volatile uint32_t *)(uintptr_t)pages;
    volatile uint32_t *ring = base, *src = base + 3 * 1024, *dst = base + 7 * 1024, *status = base + 11 * 1024;
    const uint32_t gpu = GGTT_TEST_INDEX * 4096u;
    Batch batch = { base + 1024, gpu + 4096u, 0, 0 };
    for (uint32_t i = 0; i < GPU_PAGES * 1024; i++) base[i] = 0;
    for (uint32_t y = 0; y < COPY_H; y++)
        for (uint32_t x = 0; x < COPY_W; x++)
            src[y * COPY_W + x] = 0xff000000u | (x * 4) << 16 | (y * 4) << 8 | ((x ^ y) & 0xff);
    for (uint32_t i = 0; i < COPY_W * COPY_H; i++) dst[i] = 0x11111111u;
    build_copy_batch(&batch, gpu + 3 * 4096u, gpu + 7 * 4096u, gpu + 11 * 4096u, fill);
    uint32_t n = 0;
    ring[n++] = (0x31u << 23) | 1;             /* MI_BATCH_BUFFER_START, GGTT */
    ring[n++] = batch.gpu;
    ring[n++] = 0;
    ring[n++] = 0;
    flush_lines(base, GPU_PAGES * 4096);

    volatile uint64_t *ggtt = (volatile uint64_t *)(uintptr_t)(info.mmio + (8u << 20));
    uint64_t saved[GPU_PAGES];
    for (unsigned i = 0; i < GPU_PAGES; i++) {
        saved[i] = ggtt[GGTT_TEST_INDEX + i];
        ggtt[GGTT_TEST_INDEX + i] = (pages + 4096u * i) | 1u;
    }
    (void)ggtt[GGTT_TEST_INDEX + GPU_PAGES - 1];
    gw(0x101008, 1);
    (void)gr(0x101008);
    printf("gpu copy: batch %u bytes of commands, %u of state, pages at %llx\n", batch.cmd, batch.state - 4096,
           (unsigned long long)pages);

    gw(RCS + RING_CTL, 0);
    gw(RCS + RING_HEAD, 0);
    gw(RCS + RING_TAIL, 0);
    gw(RCS + RING_START, gpu);
    gw(RCS + RING_MI_MODE, (1u << 8) << 16);
    gw(RCS + RING_CTL, 1);
    if (gr(RCS + RING_START) != gpu || !(gr(RCS + RING_CTL) & 1)) {
        ring_report("rcs set-up", RCS);
        printf("gpu copy: the ring registers did not take, stop\n");
        gw(RCS + RING_CTL, 0);
        for (unsigned i = 0; i < GPU_PAGES; i++) ggtt[GGTT_TEST_INDEX + i] = saved[i];
        gw(0x101008, 1);
        gpu_sleep();
        return;
    }
    uint32_t stats_before[8], stats_after[8];
    stats_read(stats_before);
    uint64_t t0 = wyse_ticks_us();
    gw(RCS + RING_TAIL, n * 4);
    bool done = false;
    while (wyse_ticks_us() - t0 < 200000) {
        flush_lines(status, 64);
        if (status[0] == 0xc0de3d00u) { done = true; break; }
    }
    uint64_t took = wyse_ticks_us() - t0;
    ring_report("rcs after", RCS);
    stats_read(stats_after);
    stats_report(stats_before, stats_after);
    if (!done) {
        printf("gpu copy: NO completion in 200 ms (status %08x, instdone %08x); pages %llx kept\n",
               status[0], gr(RCS + 0x6c), (unsigned long long)pages);
        gw(RCS + RING_CTL, 0);
        reset_render();
        gpu_sleep();
        return;
    }
    flush_lines(dst, COPY_W * COPY_H * 4);
    unsigned right = 0, first_bad = COPY_W * COPY_H;
    for (unsigned i = 0; i < COPY_W * COPY_H; i++) {
        if (dst[i] == (fill ? 0xffffffffu : src[i])) right++;
        else if (first_bad == COPY_W * COPY_H) first_bad = i;
    }
    printf("gpu %s: completed in %llu us, %u of %u pixels %s%s", fill ? "fill" : "copy", (unsigned long long)took,
           right, COPY_W * COPY_H, fill ? "white" : "equal the source", right == COPY_W * COPY_H ? " - PASS\n" : "");
    if (right != COPY_W * COPY_H)
        printf(", first wrong at %u: %08x, source %08x - FAIL\n", first_bad, dst[first_bad], src[first_bad]);
    gw(RCS + RING_CTL, 0);
    for (unsigned i = 0; i < GPU_PAGES; i++) ggtt[GGTT_TEST_INDEX + i] = saved[i];
    (void)ggtt[GGTT_TEST_INDEX + GPU_PAGES - 1];
    gw(0x101008, 1);
    gpu_sleep();
    gBS->FreePages(pages, GPU_PAGES);
}

#endif