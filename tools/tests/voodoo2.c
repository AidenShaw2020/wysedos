#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../pc/voodoo2.h"
#include "../../efi/platform/intel_gpu.h"
#include "../../efi/src/x87_store.h"
#include "../../pc/voodoo/voodoo_port.h"
#include <86box/vid_voodoo_common.h>
#include <86box/vid_voodoo_regs.h>
#include <86box/vid_voodoo_render.h>

#define BAR 0xe0000000u
#define LFB (BAR + 0x400000u)
#define TEX (BAR + 0x800000u)
#define FIFO (BAR + 0x200000u)

static I440FXState *bridge;
static int voodoo_devfn;
static uint64_t test_time_us;
static Voodoo2State *state;

uint64_t time_us_64(void)
{
    return test_time_us;
}

#ifdef _WIN32
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, size_t size, unsigned long type, unsigned long protect);
__declspec(dllimport) int __stdcall VirtualFree(void *address, size_t size, unsigned long type);

void *voodoo2_platform_alloc_exec(size_t size)
{
    return VirtualAlloc(NULL, size, 0x1000 | 0x2000 /* MEM_COMMIT | MEM_RESERVE */, 0x40 /* PAGE_EXECUTE_READWRITE */);
}

void voodoo2_platform_free_exec(void *pointer, size_t size)
{
    (void)size;
    VirtualFree(pointer, 0, 0x8000 /* MEM_RELEASE */);
}
#endif

static uint32_t config_read(unsigned address, int size_log2)
{
    i440fx_write_addr(bridge, 0, 0x80000000u | ((uint32_t)voodoo_devfn << 8) | (address & ~3u), 2);
    return i440fx_read_data(bridge, address & 3u, size_log2);
}

static void config_write(unsigned address, uint32_t value, int size_log2)
{
    i440fx_write_addr(bridge, 0, 0x80000000u | ((uint32_t)voodoo_devfn << 8) | (address & ~3u), 2);
    i440fx_write_data(bridge, address & 3u, value, size_log2);
}

static void reg(uint32_t offset, uint32_t value)
{
    assert(voodoo2_write(state, BAR + offset, 4, value));
}

static uint32_t reg_read(uint32_t offset)
{
    uint32_t value;
    assert(voodoo2_read(state, BAR + offset, 4, &value));
    return value;
}

static uint16_t lfb_pixel(unsigned x, unsigned y)
{
    uint32_t value;
    assert(voodoo2_read(state, LFB + y * 2048 + x * 2, 2, &value));
    return value;
}

/* Worker core stand-in for the asynchronous pass: a host thread polling the
 * board.  pause_polls > 0 holds it back for that many loop turns. */
#ifdef _WIN32
__declspec(dllimport) void *__stdcall CreateThread(void *attributes, size_t stack,
                                                    unsigned long (__stdcall *start)(void *), void *argument,
                                                    unsigned long flags, unsigned long *id);
__declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void *handle, unsigned long milliseconds);
static volatile bool worker_stop, worker_hold, helper_stop;
static volatile unsigned long long helper_jobs;

static unsigned long __stdcall helper_thread(void *argument)
{
    (void)argument;
    while (!helper_stop)
        if (voodoo2_helper_poll(state)) helper_jobs++;
    return 0;
}
static volatile long long worker_pause_polls;
static volatile unsigned long long worker_polls;

static unsigned long __stdcall worker_thread(void *argument)
{
    (void)argument;
    while (!worker_stop) {
        worker_polls++;
        if (worker_hold) continue;
        if (worker_pause_polls > 0) {
            worker_pause_polls--;
            continue;
        }
        voodoo2_worker_poll(state);
    }
    return 0;
}
#endif

static uint32_t test_caller(uint16_t *cs)
{
    *cs = 0x28;
    return 0xc0005678;
}

/* 640x480: 20 blocks of 32 pixels per row, 150 pages per buffer, as Glide
 * programs a 12 MB board. */
static void glide_style_init(void)
{
    config_write(0x40, 1, 0);
    reg(SST_fbiInit1, 10u << 4);
    reg(SST_fbiInit2, 150u << 11);
    reg(SST_videoDimensions, 639u | (480u << 16));
    reg(SST_fbiInit0, 0x1c10 | FBIINIT0_GRAPHICS_RESET);
    reg(SST_fbiInit0, 0x1c10);
    reg(SST_clipLeftRight, 640);
    reg(SST_clipLowYHighY, 480);
}

static void pci_tests(void)
{
    assert(config_read(0, 2) == 0x0002121a);
    assert(config_read(8, 2) == 0x04000002);
    assert(config_read(0x40, 2) == 0x00005000);
    config_write(0x10, 0xffffffff, 2);
    assert(config_read(0x10, 2) == 0xff000000);
    config_write(0x10, BAR, 2);
    uint32_t value;
    assert(!voodoo2_read(state, BAR, 4, &value));
    config_write(4, PCI_COMMAND_MEMORY, 1);
    assert(voodoo2_read(state, BAR, 4, &value));
    assert(!voodoo2_read(state, BAR - 4, 4, &value));
    assert(!voodoo2_read(state, BAR + VOODOO2_BAR_SIZE, 4, &value));
    assert(!voodoo2_read(state, BAR + VOODOO2_BAR_SIZE - 2, 4, &value));
    config_write(0x41, 0x07, 0);
    assert(config_read(0x40, 2) == 0x00005700);
    config_write(0x40, 0, 2);
    assert(config_read(0x40, 2) == 0x00005000);
    /* Physical Glide trace: siProcess 0x54 loaded with a 0xfff PCI count, then
     * polled until bits 27:16 clear. */
    config_write(0x54, 0x1fff0000, 2);
    assert(config_read(0x54, 2) == 0);
    config_write(0x4c, 0xffffffff, 2);
    config_write(0x50, 0x12345678, 2);
    assert(config_read(0x4c, 2) == 0 && config_read(0x50, 2) == 0 && config_read(0x40, 2) == 0x00005000);
    puts("Voodoo2 PCI identity, 16 MB BAR sizing, decode gating, initEnable byte 1, zero siProcess: PASS");

    assert(reg_read(SST_fbiInit0) == 0x1c10);
    reg(SST_fbiInit0, 0x1c11);
    assert(reg_read(SST_fbiInit0) == 0x1c10);
    config_write(0x40, 1, 0);
    reg(SST_fbiInit0, 0x1c11);
    assert(reg_read(SST_fbiInit0) == 0x1c11);
    reg(SST_fbiInit0, 0x1c10);
    assert(reg_read(SST_fbiInit5) == 0x08408000);
    reg(SST_fbiInit7, 0xff);
    assert((reg_read(SST_fbiInit7) & 0xff) == 0);
    reg(SST_fbiInit7, 0);
    /* DAC identification: index 7 selects the part, a read of index 5
     * returns its id through fbiInit2 while initEnable bit 2 is set. */
    reg(SST_dacData, (7u << 8) | 0x01);
    reg(SST_dacData, 0x800 | (5u << 8));
    config_write(0x40, 5, 0);
    assert(reg_read(SST_fbiInit2) == 0x55);
    reg(SST_dacData, (7u << 8) | 0x07);
    reg(SST_dacData, 0x800 | (5u << 8));
    assert(reg_read(SST_fbiInit2) == 0x71);
    config_write(0x40, 1, 0);
    assert(reg_read(SST_fbiInit2) == 0x80000040);
    puts("Voodoo2 initEnable write gating, board straps and DAC identification readback: PASS");
}

/* Bilinear texture x Gouraud colour, alpha blend, dither and depth test into
 * the back buffer; returns the 64x64 corner. */
static void render_scene(uint16_t *out)
{
    reg(SST_fbzMode, FBZ_DRAW_BACK | FBZ_RGB_WMASK | FBZ_DEPTH_WMASK);
    reg(SST_color1, 0x00203040);
    reg(SST_zaColor, 0xffff);
    reg(SST_fastfillCMD, 0);
    reg(SST_fbzMode, FBZ_DRAW_BACK | FBZ_RGB_WMASK | FBZ_DEPTH_WMASK | FBZ_DEPTH_ENABLE |
                     (DEPTHOP_LESSTHAN << 5) | FBZ_DITHER);
    reg(SST_fbzColorPath, FBZCP_TEXTURE_ENABLED | C_SEL_TEX | (1u << 10));
    reg(SST_alphaMode, (1u << 4) | (1u << 8) | (5u << 12));
    reg(SST_textureMode | 0x800, TEXTUREMODE_LOCAL | (TEX_R5G6B5 << 8) | (1u << 1) | (1u << 2));
    reg(SST_tLOD | 0x800, 0);
    reg(SST_texBaseAddr | 0x800, 0);
    reg(SST_vertexAx, 4 * 16);
    reg(SST_vertexAy, 4 * 16);
    reg(SST_vertexBx, 60 * 16);
    reg(SST_vertexBy, 10 * 16);
    reg(SST_vertexCx, 20 * 16);
    reg(SST_vertexCy, 58 * 16);
    reg(SST_startR, 128u << 12);
    reg(SST_startG, 64u << 12);
    reg(SST_startB, 200u << 12);
    reg(SST_startA, 160u << 12);
    reg(SST_startZ, 0x8000u << 12);
    reg(SST_dRdX, 1u << 12);
    reg(SST_dGdY, 2u << 12);
    reg(SST_dAdX, (uint32_t)-(1 << 12));
    reg(SST_dZdX, 0x10u << 12);
    reg(SST_startS, 0);
    reg(SST_startT, 0);
    reg(SST_startW, 0);
    reg(SST_dSdX, 1u << 16);
    reg(SST_dTdY, 1u << 16);
    reg(SST_dSdY, 1u << 14);
    reg(SST_triangleCMD, 0);
    reg(SST_lfbMode, 1u << 6);
    for (unsigned y = 0; y < 64; y++)
        for (unsigned x = 0; x < 64; x++)
            out[y * 64 + x] = lfb_pixel(x, y);
}

/* The x86-64 span recompiler must draw exactly what the C renderer draws. */
static void load_test_texture(void)
{
    for (unsigned t = 0; t < 256; t++)
        for (unsigned s = 0; s < 256; s += 2) {
            uint32_t a = (s * 0x0841u + t * 0x1003u) & 0xffff, b = (a * 7u + 0x3cf1u) & 0xffff;
            assert(voodoo2_write(state, TEX + (t << 9) + (s << 1), 4, a | (b << 16)));
        }
}

static void jit_tests(void)
{
    load_test_texture();
    static uint16_t jit[64 * 64], plain[64 * 64];
    assert(voodoo2_set_jit(state, true));
    uint32_t recompiles = voodoo2_stats(state).recompiles;
    render_scene(jit);
    assert(voodoo2_stats(state).recompiles > recompiles && voodoo2_stats(state).jit);
    assert(!voodoo2_set_jit(state, false) && !voodoo2_stats(state).jit);
    render_scene(plain);
    unsigned distinct = 0, background = 0;
    for (unsigned index = 0; index < 64 * 64; index++) {
        assert(jit[index] == plain[index]);
        if (index && plain[index] != plain[index - 1]) distinct++;
        if (plain[index] == plain[0]) background++;
    }
    assert(distinct > 500 && background < 64 * 64 - 900);
    assert(voodoo2_set_jit(state, true));
    reg(SST_alphaMode, 0);
    reg(SST_dSdX, 0);
    reg(SST_dTdY, 0);
    reg(SST_dSdY, 0);
    reg(SST_dRdX, 0);
    reg(SST_dGdY, 0);
    reg(SST_dAdX, 0);
    reg(SST_dZdX, 0);
    /* Leave the red back buffer the following FIFO tests start from. */
    reg(SST_fbzMode, FBZ_DRAW_BACK | FBZ_RGB_WMASK);
    reg(SST_color1, 0x00ff0000);
    reg(SST_fastfillCMD, 0);
    assert(lfb_pixel(16, 16) == 0xf800);
    puts("Voodoo2 x86-64 span recompiler matches the C renderer (bilinear, blend, dither, depth): PASS");
}

static void render_tests(void)
{
    glide_style_init();
    Voodoo2Stats stats = voodoo2_stats(state);
    assert(stats.row_width == 1280 && stats.front_offset == 0 && stats.draw_offset == 0);
    reg(SST_lfbMode, 1u << 6);
    reg(SST_color1, 0x00ff0000);
    reg(SST_zaColor, 0xffff);
    reg(SST_fbzMode, FBZ_DRAW_BACK | FBZ_RGB_WMASK | FBZ_DEPTH_WMASK);
    assert(voodoo2_stats(state).draw_offset == 150u * 4096);
    reg(SST_fastfillCMD, 0);
    assert(lfb_pixel(0, 0) == 0xf800 && lfb_pixel(639, 479) == 0xf800);
    reg(SST_fbzColorPath, (1u << 8) | (1u << 14));
    reg(SST_fbzMode, FBZ_DRAW_BACK | FBZ_RGB_WMASK | FBZ_DEPTH_WMASK | FBZ_DEPTH_ENABLE | (DEPTHOP_LESSTHAN << 5));
    reg(SST_vertexAx, 8 * 16);
    reg(SST_vertexAy, 8 * 16);
    reg(SST_vertexBx, 56 * 16);
    reg(SST_vertexBy, 8 * 16);
    reg(SST_vertexCx, 8 * 16);
    reg(SST_vertexCy, 56 * 16);
    reg(SST_startR, 0);
    reg(SST_startG, 255 << 12);
    reg(SST_startZ, 0x1234 << 12);
    reg(SST_triangleCMD, 0);
    assert(voodoo2_stats(state).triangles == 1);
    assert(lfb_pixel(16, 16) == 0x07e0 && lfb_pixel(60, 60) == 0xf800);
    reg(SST_startG, 0);
    reg(SST_startB, 255 << 12);
    reg(SST_startZ, 0x2345 << 12);
    reg(SST_triangleCMD, 0);
    assert(lfb_pixel(16, 16) == 0x07e0);
    reg(SST_lfbMode, 2u << 6);
    assert(lfb_pixel(16, 16) == 0x1234);
    puts("Voodoo2 fbiInit buffer layout, fast fill, Gouraud triangle and depth rejection: PASS");

    reg(SST_lfbMode, 0);
    reg(SST_swapbufferCMD, 0);
    stats = voodoo2_stats(state);
    assert(stats.front_offset == 150u * 4096 && stats.draw_offset == 0 && stats.swaps == 1);
    assert(lfb_pixel(16, 16) == 0x07e0);
    reg(SST_swapbufferCMD, 1);
    stats = voodoo2_stats(state);
    assert(stats.front_offset == 0 && stats.frame_count >= 1);
    assert(((reg_read(SST_status) >> 28) & 7) == 0);
    puts("Voodoo2 immediate and interval buffer swaps and swap-pending status: PASS");

    reg(SST_lfbMode, LFB_FORMAT_RGB565 | LFB_WRITE_FRONT);
    assert(voodoo2_write(state, LFB + 5 * 2048 + 7 * 2, 2, 0x001f));
    assert(voodoo2_write(state, LFB + 6 * 2048 + 8 * 2, 4, 0xf8000000 | 0x07e0));
    reg(SST_lfbMode, LFB_FORMAT_RGB565);
    assert(lfb_pixel(7, 5) == 0x001f && lfb_pixel(8, 6) == 0x07e0 && lfb_pixel(9, 6) == 0xf800);
    puts("Voodoo2 LFB word/dword writes and front-buffer reads: PASS");

    reg(SST_lfbMode, 1u << 6);
    reg(SST_fbzMode, FBZ_DRAW_BACK | FBZ_RGB_WMASK);
    reg(SST_fbzColorPath, FBZCP_TEXTURE_ENABLED | C_SEL_TEX);
    reg(SST_textureMode | 0x800, TEXTUREMODE_LOCAL | (TEX_R5G6B5 << 8));
    reg(SST_tLOD | 0x800, 0);
    reg(SST_texBaseAddr | 0x800, 0);
    reg(SST_startS, 0);
    reg(SST_startT, 0);
    reg(SST_startW, 0);
    assert(voodoo2_write(state, TEX, 4, 0x001f001f));
    reg(SST_triangleCMD, 0);
    assert(lfb_pixel(16, 16) == 0x001f);
    assert(voodoo2_write(state, TEX, 4, 0xf800f800));
    reg(SST_triangleCMD, 0);
    assert(lfb_pixel(16, 16) == 0xf800);
    uint32_t value;
    assert(voodoo2_read(state, TEX, 4, &value) && value == UINT32_MAX);
    puts("Voodoo2 TMU0 texture download, RGB565 sampling and texture cache refresh: PASS");
    jit_tests();
}

static void fifo_tests(void)
{
    reg(SST_fbzColorPath, (1u << 8) | (1u << 14));
    reg(SST_fbzMode, FBZ_DRAW_BACK | FBZ_RGB_WMASK);
    reg(SST_cmdFifoBaseAddr, 0x300u | (0x310u << 16));
    reg(SST_cmdFifoRdPtr, 0x300000);
    reg(SST_cmdFifoAMin, 0x300000 - 4);
    reg(SST_cmdFifoAMax, 0x300000 - 4);
    reg(SST_cmdFifoDepth, 0);
    reg(SST_fbiInit7, FBIINIT7_CMDFIFO_ENABLE);
    assert(voodoo2_stats(state).cmdfifo_enabled);
    reg(SST_color1, 0x000000ff);
    assert(lfb_pixel(16, 16) == 0xf800);
    /* Register writes outside the FIFO window are dropped in FIFO mode. */
    reg(SST_fastfillCMD, 0);
    assert(lfb_pixel(16, 16) == 0xf800);
    const uint32_t packets[] = {
        (1u << 16) | (SST_color1 << 1) | 1, 0x0000ff00,
        (1u << 16) | (SST_fastfillCMD << 1) | 1, 0,
    };
    for (unsigned index = 0; index < 3; index++) {
        assert(voodoo2_write(state, FIFO + index * 4, 4, packets[index]));
        assert(lfb_pixel(16, 16) == 0xf800);
    }
    assert(voodoo2_write(state, FIFO + 3 * 4, 4, packets[3]));
    assert(lfb_pixel(16, 16) == 0x07e0);
    Voodoo2Stats stats = voodoo2_stats(state);
    assert(stats.cmdfifo_packets == 2 && stats.cmdfifo_faults == 0);
    assert(reg_read(SST_cmdFifoRdPtr) == 0x300010 && reg_read(SST_cmdFifoDepth) == 0);
    /* A word written past a gap waits as a hole until the gap is filled
     * (the driver writes a packet's payload before its header). */
    const uint32_t more[] = {
        (1u << 16) | (SST_color1 << 1) | 1, 0x00ff0000,
        (1u << 16) | (SST_fastfillCMD << 1) | 1, 0,
    };
    assert(voodoo2_write(state, FIFO + 0x10 + 4, 4, more[1]));
    assert(reg_read(SST_cmdFifoDepth) == 0 && reg_read(SST_cmdFifoHoles) == 1);
    assert(voodoo2_write(state, FIFO + 0x10, 4, more[0]));
    assert(reg_read(SST_cmdFifoHoles) == 0);
    assert(voodoo2_write(state, FIFO + 0x10 + 8, 4, more[2]));
    assert(lfb_pixel(16, 16) == 0x07e0);
    assert(voodoo2_write(state, FIFO + 0x10 + 12, 4, more[3]));
    assert(lfb_pixel(16, 16) == 0xf800);
    /* JSR into a subroutine and RET resume the main stream. */
    /* The subroutine sits in frame buffer memory at 0x100000 (front buffer
     * row 819, column 128 with the 1280-byte stride), written through LFB. */
    uint32_t subroutine[] = {(1u << 16) | (SST_color1 << 1) | 1, 0x000000ff, (2u << 3)};
    reg(SST_lfbMode, LFB_FORMAT_RGB565 | LFB_WRITE_FRONT);
    for (unsigned index = 0; index < 3; index++)
        assert(voodoo2_write(state, LFB + 819 * 2048 + (128 + index * 2) * 2, 4, subroutine[index]));
    reg(SST_lfbMode, 1u << 6);
    const uint32_t call[] = {(0x100000u << 4) | (1u << 3), (1u << 16) | (SST_fastfillCMD << 1) | 1, 0};
    for (unsigned index = 0; index < 3; index++)
        assert(voodoo2_write(state, FIFO + 0x20 + index * 4, 4, call[index]));
    assert(lfb_pixel(16, 16) == 0x001f);
    assert(voodoo2_stats(state).cmdfifo_faults == 0);
    reg(SST_fbiInit7, 0);
    puts("Voodoo2 command FIFO publication, holes, JSR/RET and direct-write suppression: PASS");
}

static void video_tests(void)
{
    unsigned width = 0, height = 0;
    test_time_us = 100;
    uint32_t status = reg_read(SST_status);
    assert((status & 0x3f) == 0x3f && (status & 0x40) && !(status & 0x380));
    test_time_us = 16000;
    assert(!(reg_read(SST_status) & 0x40));
    reg(SST_vSync, 3u | (522u << 16));
    test_time_us = 16667 / 2;
    unsigned line = reg_read(SST_vRetrace);
    assert(line >= 260 && line <= 264);
    test_time_us = 0;
    puts("Voodoo2 clock-driven retrace status and vRetrace line counter: PASS");

    assert(!voodoo2_frame(state, NULL, 0, &width, &height));
    reg(SST_fbiInit0, 0x1c11);
    assert(voodoo2_frame(state, NULL, 0, &width, &height) && width == 640 && height == 480);
    uint32_t *pixels = calloc(640 * 480, sizeof(uint32_t));
    assert(pixels);
    assert(!voodoo2_frame(state, pixels, 640 * 479, &width, &height));
    assert(voodoo2_frame(state, pixels, 640 * 480, &width, &height));
    assert(pixels[5 * 640 + 7] == 0x0000f8 && pixels[6 * 640 + 9] == 0xf80000);
    assert(pixels[6 * 640 + 8] == 0x00fb00);
    reg(SST_clutData, (31u << 24) | 0x000010);
    assert(voodoo2_frame(state, pixels, 640 * 480, &width, &height));
    assert(pixels[5 * 640 + 7] == 0x000010);
    assert(voodoo2_stats(state).pass_through);

    /* Scanout cache: unchanged image is not converted again. */
    bool updated = false;
    uint64_t scanouts = voodoo2_stats(state).scanouts;
    assert(voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated) && !updated);
    assert(voodoo2_stats(state).scanouts == scanouts);
    reg(SST_fbzMode, FBZ_DRAW_BACK | FBZ_RGB_WMASK);
    reg(SST_fastfillCMD, 0);
    reg(SST_triangleCMD, 0);
    assert(voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated) && !updated);
    uint32_t before = pixels[5 * 640 + 7];
    reg(SST_lfbMode, LFB_FORMAT_RGB565 | LFB_WRITE_FRONT);
    assert(voodoo2_write(state, LFB + 5 * 2048 + 7 * 2, 2, 0xf800));
    reg(SST_lfbMode, LFB_FORMAT_RGB565);
    assert(voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated) && updated);
    assert(pixels[5 * 640 + 7] != before);
    assert(voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated) && !updated);
    reg(SST_lfbMode, LFB_FORMAT_RGB565 | LFB_WRITE_BACK);
    assert(voodoo2_write(state, LFB + 5 * 2048 + 7 * 2, 2, 0x07e0));
    reg(SST_lfbMode, LFB_FORMAT_RGB565);
    assert(voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated) && !updated);
    assert(voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, true, &updated) && updated);
    reg(SST_fbzMode, FBZ_DRAW_FRONT | FBZ_RGB_WMASK);
    reg(SST_triangleCMD, 0);
    assert(voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated) && updated);
    reg(SST_fastfillCMD, 0);
    assert(voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated) && updated);
    reg(SST_fbzMode, FBZ_DRAW_BACK | FBZ_RGB_WMASK);
    uint32_t front = voodoo2_stats(state).front_offset;
    reg(SST_swapbufferCMD, 0);
    uint32_t shown = voodoo2_stats(state).front_offset;
    assert(shown != front);
    assert(voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated) && updated);
    /* Two swaps between scanouts return to the shown buffer with new content. */
    reg(SST_swapbufferCMD, 0);
    reg(SST_swapbufferCMD, 0);
    assert(voodoo2_stats(state).front_offset == shown);
    assert(voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated) && updated);
    reg(SST_clutData, (31u << 24) | 0x000020);
    assert(voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated) && updated);
    assert(!voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, NULL));
    reg(SST_videoDimensions, 4095u | (4095u << 16));
    assert(!voodoo2_frame(state, NULL, 0, &width, &height));
    reg(SST_videoDimensions, 639u | (480u << 16));
    reg(SST_fbiInit0, 0x1c10);
    assert(!voodoo2_frame(state, NULL, 0, &width, &height));
    free(pixels);
    puts("Voodoo2 VGA pass-through scanout, gamma CLUT, capacity, geometry bounds and scanout cache: PASS");
}

static void misc_tests(void)
{
    Voodoo2Stats before = voodoo2_stats(state);
    assert(voodoo2_write(state, BAR + SST_color1, 1, 0xff));
    assert(voodoo2_stats(state).unsupported_writes == before.unsupported_writes + 1);
    uint8_t bytes[] = {0x1f, 0x00, 0xe0, 0x07, 0x00, 0xf8};
    reg(SST_lfbMode, LFB_FORMAT_RGB565 | LFB_WRITE_FRONT);
    assert(voodoo2_write_buffer(state, LFB + 100 * 2048 + 10 * 2, bytes, sizeof(bytes)));
    reg(SST_lfbMode, LFB_FORMAT_RGB565);
    assert(lfb_pixel(10, 100) == 0x001f && lfb_pixel(11, 100) == 0x07e0 && lfb_pixel(12, 100) == 0xf800);
    assert(!voodoo2_write_buffer(state, BAR + VOODOO2_BAR_SIZE - 2, bytes, 4));
    puts("Voodoo2 byte-write rejection and string writes into the LFB: PASS");

    Voodoo2CommandChange changes[VOODOO2_COMMAND_HISTORY];
    unsigned total = voodoo2_command_changes(state, changes, VOODOO2_COMMAND_HISTORY);
    voodoo2_set_caller_source(test_caller);
    config_write(4, 0, 1);
    assert(voodoo2_command_changes(state, changes, 1) == total + 1);
    assert(changes[0].before == PCI_COMMAND_MEMORY && changes[0].after == 0);
    assert(changes[0].cs == 0x28 && changes[0].eip == 0xc0005678);
    uint32_t value;
    assert(!voodoo2_read(state, BAR, 4, &value));
    config_write(4, PCI_COMMAND_MEMORY, 1);
    voodoo2_set_caller_source(NULL);
    puts("Voodoo2 PCI command change history with guest caller: PASS");

    assert(voodoo2_reset(state));
    assert(config_read(0x10, 2) == 0 && config_read(0x40, 2) == 0x5000 && (config_read(4, 1) & 2) == 0);
    config_write(0x10, BAR, 2);
    config_write(4, PCI_COMMAND_MEMORY, 1);
    Voodoo2Stats reset = voodoo2_stats(state);
    assert(reset.triangles == 0 && reset.swaps == 0 && reset.fbi_init0 == 0x1c10 && !reset.pass_through);
    glide_style_init();
    reg(SST_lfbMode, 1u << 6);
    reg(SST_fbzMode, FBZ_DRAW_BACK | FBZ_RGB_WMASK);
    reg(SST_color1, 0x0000ff00);
    reg(SST_fastfillCMD, 0);
    assert(lfb_pixel(320, 240) == 0x07e0);
    puts("Voodoo2 reset to power-on state and post-reset rendering: PASS");
}

static void intel_gpu_tests(void)
{
    IntelGpuPCI gpu_pci = {0x22b08086, 6, 0x90000004, 0, 0xa000000c, 0, 0};
    IntelGpuInfo gpu_info;
    assert(intel_gpu_decode_pci(&gpu_pci, &gpu_info));
    assert(gpu_info.mmio == 0x90000000 && gpu_info.aperture == 0xa0000000 && gpu_info.bus_master_enabled);
    gpu_pci.command = 2;
    assert(intel_gpu_decode_pci(&gpu_pci, &gpu_info) && !gpu_info.bus_master_enabled);
    gpu_pci.command = 4;
    assert(!intel_gpu_decode_pci(&gpu_pci, &gpu_info) && !gpu_info.mmio);
    gpu_pci.command = 6;
    gpu_pci.identity = UINT32_MAX;
    assert(!intel_gpu_decode_pci(&gpu_pci, &gpu_info));
    gpu_pci.identity = 0x22b08086;
    gpu_pci.mmio_low |= 1;
    assert(!intel_gpu_decode_pci(&gpu_pci, &gpu_info));
    gpu_pci.mmio_low = 0x90001004;
    assert(!intel_gpu_decode_pci(&gpu_pci, &gpu_info));
    gpu_pci.mmio_low = 0x90000004;
    gpu_pci.aperture_low = 0x9080000c;
    assert(!intel_gpu_decode_pci(&gpu_pci, &gpu_info));
    gpu_pci.aperture_low = 0xfffff80c;
    gpu_pci.aperture_high = UINT32_MAX;
    assert(!intel_gpu_decode_pci(&gpu_pci, &gpu_info));
    gpu_pci.aperture_low = 0xa000000c;
    gpu_pci.aperture_high = 1;
    assert(intel_gpu_decode_pci(&gpu_pci, &gpu_info) && gpu_info.aperture == 0x1a0000000ull);
    assert(!intel_gpu_decode_pci(NULL, &gpu_info) && !gpu_info.mmio);
    assert(!intel_gpu_decode_pci(&gpu_pci, NULL));
    puts("Passive Intel GPU PCI preflight, decode gating, BAR types/alignment and overlap rejection: PASS");
}

static void x87_tests(void)
{
    WyseFxState original_fx, guest_fx, next_fx, after_fx;
    const uint32_t x87_value = 0x43f00000;
    __asm__ volatile("fxsave64 %0\n\tfninit\n\tflds %2\n\tfxsave64 %1\n\tfxrstor64 %0"
        : "=m"(original_fx), "=m"(guest_fx) : "m"(x87_value) : "memory");
    WyseFxState unchanged_fx = guest_fx;
    uint64_t x87_bits;
    assert(wyse_x87_store(&guest_fx, &next_fx, &x87_bits, false, false));
    assert(x87_bits == x87_value && next_fx.tag == guest_fx.tag);
    assert(wyse_x87_store(&guest_fx, &next_fx, &x87_bits, false, true));
    assert(x87_bits == x87_value && next_fx.tag == 0);
    assert(((next_fx.status >> 11) & 7) == 0);
    assert(memcmp(&guest_fx, &unchanged_fx, sizeof(guest_fx)) == 0);
    assert(wyse_x87_store(&guest_fx, &next_fx, &x87_bits, true, false));
    assert(x87_bits == UINT64_C(0x407e000000000000) && next_fx.tag == guest_fx.tag);
    assert(wyse_x87_store(&guest_fx, &next_fx, &x87_bits, true, true));
    assert(x87_bits == UINT64_C(0x407e000000000000) && next_fx.tag == 0);
    __asm__ volatile("fxsave64 %0" : "=m"(after_fx));
    assert(after_fx.control == original_fx.control && after_fx.status == original_fx.status);
    assert(after_fx.tag == original_fx.tag && after_fx.mxcsr == original_fx.mxcsr);
    const uint64_t halfway_value = UINT64_C(0x407e000010000000);
    __asm__ volatile("fxsave64 %0\n\tfninit\n\tfldl %2\n\tfxsave64 %1\n\tfxrstor64 %0"
        : "=m"(original_fx), "=m"(guest_fx) : "m"(halfway_value) : "memory");
    assert(wyse_x87_store(&guest_fx, &next_fx, &x87_bits, false, false));
    assert(x87_bits == x87_value);
    guest_fx.control = (guest_fx.control & ~0xc00u) | 0x800u;
    assert(wyse_x87_store(&guest_fx, &next_fx, &x87_bits, false, false));
    assert(x87_bits == x87_value + 1);
    guest_fx.control &= ~1u;
    guest_fx.status |= 1u;
    next_fx = unchanged_fx;
    x87_bits = 123;
    assert(!wyse_x87_store(&guest_fx, &next_fx, &x87_bits, false, true));
    assert(x87_bits == 123 && memcmp(&next_fx, &unchanged_fx, sizeof(next_fx)) == 0);
    puts("Native x87 MMIO stores, transactional pop, host-state restoration and pending-exception rejection: PASS");
}

#ifdef _WIN32
static void wait_worker_idle(void)
{
    unsigned long long polls = worker_polls;
    while (worker_polls < polls + 3)
        ;
}

/* Non-strict asynchronous behaviour: live progress registers, the worker's
 * own scanout and a full write queue. */
static void async_tests(void)
{
    voodoo2_set_strict_reads(state, false);
    glide_style_init();
    reg(SST_fbzMode, FBZ_DRAW_BACK | FBZ_RGB_WMASK);
    reg(SST_color1, 0x0000ff00);
    worker_hold = true;
    wait_worker_idle();
    reg(SST_fastfillCMD, 0);
    reg(SST_swapbufferCMD, 0);
    uint32_t status = reg_read(SST_status);
    assert(!(status & 0x380) && ((status >> 28) & 7) == 1);
    voodoo2_set_queue_busy(state, true);
    status = reg_read(SST_status);
    assert((status & 0x380) == 0x380 && ((status >> 28) & 7) == 1);
    worker_hold = false;
    for (unsigned long spins = 0;; spins++) {
        status = reg_read(SST_status);
        if (!(status & 0x380) && !((status >> 28) & 7)) break;
        assert(spins < 400000000ul);
    }
    voodoo2_set_queue_busy(state, false);
    puts("Voodoo2 asynchronous status: queued swap pending, queued work busy only on request, clear after the worker: PASS");

    static uint32_t pixels[640 * 480];
    unsigned width = 0, height = 0;
    bool updated = false;
    reg(SST_fbiInit0, 0x1c11);
    for (unsigned long spins = 0;; spins++) {
        if (voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated) && updated) break;
        assert(spins < 400000000ul);
    }
    assert(width == 640 && height == 480 && pixels[0] == 0x00fb00 && pixels[479 * 640 + 639] == 0x00fb00);
    /* a front-buffer write is shown once the scanout interval has passed */
    reg(SST_lfbMode, LFB_FORMAT_RGB565 | LFB_WRITE_FRONT);
    assert(voodoo2_write(state, LFB + 3 * 2048 + 4 * 2, 2, 0x001f));
    test_time_us += 20000;
    for (unsigned long spins = 0;; spins++) {
        if (voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated) && updated &&
            pixels[3 * 640 + 4] == 0x0000f8) break;
        assert(spins < 400000000ul);
    }
    reg(SST_fbiInit0, 0x1c10);
    for (unsigned long spins = 0; voodoo2_frame_update(state, pixels, 640 * 480, &width, &height, false, &updated); spins++)
        assert(spins < 400000000ul);
    puts("Voodoo2 asynchronous scanout by the worker, front-buffer refresh and pass-through release: PASS");

    /* 1 << 17 queue entries (pc/voodoo2.c RING_ENTRIES): one more write waits */
    worker_hold = true;
    wait_worker_idle();
    for (unsigned index = 0; index < (1u << 17); index++)
        reg(SST_color0, index);
    worker_pause_polls = 50000000;
    worker_hold = false;
    reg(SST_color0, 0x12345678);
    Voodoo2Stats stats = voodoo2_stats(state);
    assert(stats.ring_full_waits == 1 && stats.async);
    assert(reg_read(SST_color0) == 0x12345678);
    voodoo2_set_strict_reads(state, true);
    puts("Voodoo2 asynchronous full write queue waits for the worker and keeps order: PASS");

    /* Odd lines on the helper thread draw exactly what one thread draws. */
    static uint16_t whole[64 * 64], split[64 * 64];
    glide_style_init();
    load_test_texture();
    assert(voodoo2_set_split(state, false, false));
    render_scene(whole);
    unsigned long long jobs = helper_jobs;
    assert(voodoo2_set_split(state, true, true));
    render_scene(split);
    Voodoo2Stats stats_split = voodoo2_stats(state);
    assert(stats_split.split && stats_split.split_helped + stats_split.split_local > 0);
    assert(!memcmp(whole, split, sizeof(whole)));
    printf("Voodoo2 split rendering (odd lines on a helper thread, %llu taken) matches one thread: PASS\n",
           helper_jobs - jobs);

    /* Textures used while rendering with one thread, then split again: the
     * cache must not count them as still in use by the second worker (the
     * physical hang: all 64 entries 'in use', voodoo_use_texture spun). */
    assert(voodoo2_set_split(state, false, false));
    reg(SST_fbzMode, FBZ_DRAW_BACK | FBZ_RGB_WMASK);
    reg(SST_fbzColorPath, FBZCP_TEXTURE_ENABLED | C_SEL_TEX);
    reg(SST_alphaMode, 0);
    for (unsigned round = 0; round < 4; round++) {
        assert(voodoo2_set_split(state, round & 1, round & 1));
        for (unsigned base = 0; base < 100; base++) {
            reg(SST_texBaseAddr | 0x800, (round * 100 + base) * 0x40);
            reg(SST_triangleCMD, 0);
        }
    }
    (void)reg_read(SST_status);
    puts("Voodoo2 split off/on switching with more textures than cache entries does not stall: PASS");
}
#endif

static void board_tests(void)
{
    pci_tests();
    render_tests();
    fifo_tests();
    video_tests();
    misc_tests();
}

static void new_board(void)
{
    PCIBus *bus;
    int piix3_devfn;
    bridge = i440fx_init(&bus, &piix3_devfn);
    assert(bridge);
    assert(!voodoo2_init(NULL, 12));
    state = voodoo2_init(bus, 12);
    assert(voodoo2_memory_mb(state) == 12);
    assert(state);
    voodoo_devfn = pci_device_get_devfn(voodoo2_pci_device(state));
}

int main(void)
{
    new_board();
    board_tests();
    intel_gpu_tests();
    x87_tests();
    voodoo2_free(state);
#ifdef _WIN32
    /* Again with every write queued for a worker thread; strict reads keep the
     * expectations of the synchronous suite. */
    test_time_us = 0;
    new_board();
    void *thread = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
    void *helper = CreateThread(NULL, 0, helper_thread, NULL, 0, NULL);
    assert(thread && helper);
    assert(voodoo2_set_async(state, true));
    assert(voodoo2_set_split(state, true, true));
    voodoo2_set_strict_reads(state, true);
    board_tests();
    printf("Voodoo2 whole board suite again with queued writes on a worker thread and odd lines on a helper (%llu taken): PASS\n",
           helper_jobs);
    async_tests();
    assert(voodoo2_set_async(state, false));
    worker_stop = helper_stop = true;
    WaitForSingleObject(thread, 0xffffffffu);
    WaitForSingleObject(helper, 0xffffffffu);
    voodoo2_free(state);
#endif
    puts("Voodoo2 host regression: PASS");
    return 0;
}
