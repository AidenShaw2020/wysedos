/* The render channel (pc/wgpu.c) against a fake engine: PCI identity and
 * BARs, the register map, submit validation, fences, and the reset path. The
 * fake runs MI_STORE_DATA_IMM (GGTT) and MI_BATCH_BUFFER_END from the
 * aperture, then writes the fence as the ring would. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../pc/wgpu.h"

void *pcmalloc(long size) { return calloc(1, (size_t)size); }

#define APERTURE (1u << 20)
#define GFX 0x10000000u
#define LFB_GFX 0x20000000u
#define BAR0 0xf0000000u
#define BAR2 0xf8000000u

static uint8_t *aperture;
static struct {
    int opens, submits, syncs, maps, writes;
    uint32_t mapped_at;
    bool mapped, hang, refuse_open, ring_full;
    uint32_t resets;
    bool busy;
    uint32_t owed_fence_gfx, owed_value;
} fake;

static uint32_t *at(uint32_t gfx)
{
    assert(gfx >= GFX && gfx - GFX < APERTURE);
    return (uint32_t *)(aperture + (gfx - GFX));
}

static bool fake_open(void *o, WgpuInfo *info)
{
    (void)o;
    fake.opens++;
    if (fake.refuse_open) return false;
    info->aperture_gfx = GFX;
    info->lfb_gfx = LFB_GFX;
    info->gpu_id = 0x22b0 | 0x22 << 16;
    return true;
}

static int fake_submit(void *o, uint32_t batch, uint32_t fence_gfx, uint32_t value)
{
    (void)o;
    if (fake.ring_full) return WGPU_SUBMIT_RING_FULL;
    fake.submits++;
    if (fake.hang) {
        fake.busy = true;
        fake.owed_fence_gfx = fence_gfx;
        fake.owed_value = value;
        return WGPU_SUBMIT_OK;
    }
    for (uint32_t *p = at(batch);;) {
        uint32_t cmd = *p;
        if (cmd == (0x0au << 23)) break;
        assert(cmd == ((0x20u << 23) | (1u << 22) | 2));
        *at(p[1]) = p[3];
        p += 4;
    }
    *at(fence_gfx) = value;
    return WGPU_SUBMIT_OK;
}

static uint32_t fake_status(void *o)
{
    (void)o;
    if (fake.busy && fake.hang) {           /* "3 s" later: reset, nothing completes */
        fake.resets++;
        fake.busy = false;
    }
    return WGPU_STATUS_READY | (fake.busy ? WGPU_STATUS_BUSY : 0);
}

static uint32_t fake_resets(void *o) { (void)o; return fake.resets; }
static void fake_sync(void *o, const void *p, uint32_t n) { (void)o; (void)p; (void)n; fake.syncs++; }
static void fake_map(void *o, uint32_t gpa, bool en) { (void)o; fake.maps++; fake.mapped = en; fake.mapped_at = gpa; }
static void fake_write(void *o, uint8_t *p, const void *src, uint32_t n) { (void)o; memcpy(p, src, n); fake.writes++; }

static I440FXState *bridge;
static int devfn;

static uint32_t cfg_read(unsigned a)
{
    i440fx_write_addr(bridge, 0, 0x80000000u | ((uint32_t)devfn << 8) | (a & ~3u), 2);
    return i440fx_read_data(bridge, 0, 2);
}

static void cfg_write(unsigned a, uint32_t v)
{
    i440fx_write_addr(bridge, 0, 0x80000000u | ((uint32_t)devfn << 8) | (a & ~3u), 2);
    i440fx_write_data(bridge, 0, v, 2);
}

static uint32_t rd(WgpuState *s, uint32_t reg)
{
    uint32_t v;
    assert(wgpu_read(s, BAR0 + reg, 4, &v));
    return v;
}

static void wr(WgpuState *s, uint32_t reg, uint32_t v) { assert(wgpu_write(s, BAR0 + reg, 4, v)); }

static void put(uint32_t off, uint32_t v) { memcpy(aperture + off, &v, 4); }
static uint32_t get(uint32_t off) { uint32_t v; memcpy(&v, aperture + off, 4); return v; }

/* A batch at 0x1000 storing value to aperture offset 0x800, descriptor at 0x100. */
static void prepare(uint32_t value, uint32_t fence)
{
    uint32_t b = 0x1000;
    put(b, (0x20u << 23) | (1u << 22) | 2); put(b + 4, GFX + 0x800); put(b + 8, 0); put(b + 12, value);
    put(b + 16, 0x0au << 23); put(b + 20, 0);
    put(0x100, b); put(0x104, 24); put(0x108, fence); put(0x10c, 0);
}

int main(void)
{
    aperture = calloc(1, APERTURE);
    PCIBus *bus;
    int piix3;
    bridge = i440fx_init(&bus, &piix3);
    assert(bridge);
    WgpuBackend be = { NULL, aperture, APERTURE, 8u << 20, fake_open, fake_submit, fake_status,
                       fake_resets, fake_sync, fake_map, fake_write };
    WgpuBackend bad = be;
    bad.aperture_size = 3u << 20;
    assert(!wgpu_init(bus, &bad));
    WgpuState *s = wgpu_init(bus, &be);
    assert(s);

    /* PCI: find it, identity, BAR sizing, decode gating, aperture mapping */
    devfn = -1;
    for (int d = 0; d < 256 && devfn < 0; d += 8) {
        i440fx_write_addr(bridge, 0, 0x80000000u | ((uint32_t)d << 8), 2);
        if (i440fx_read_data(bridge, 0, 2) == 0x22b08086u) devfn = d;
    }
    assert(devfn >= 0);
    assert((cfg_read(8) >> 16) == 0x0b40);
    assert(cfg_read(0x2c) == (WGPU_SUBSYSTEM_ID << 16 | WGPU_SUBSYSTEM_VENDOR));
    cfg_write(0x10, 0xffffffffu);
    assert(cfg_read(0x10) == 0xfffff000u);
    cfg_write(0x18, 0xffffffffu);
    assert(cfg_read(0x18) == (0xfff00000u | PCI_ADDRESS_SPACE_MEM_PREFETCH));
    cfg_write(0x10, BAR0);
    cfg_write(0x18, BAR2);
    uint32_t v;
    assert(!wgpu_read(s, BAR0, 4, &v));
    assert(!fake.mapped);
    cfg_write(4, PCI_COMMAND_MEMORY);
    assert(fake.mapped && fake.mapped_at == BAR2);
    assert(fake.opens == 0);                /* nothing touches the GPU before the guest does */
    puts("WGPU PCI identity 8086:22B0 class 0B40, 4 KB + 1 MB prefetchable BARs, aperture mapped on decode: PASS");

    /* registers */
    assert(rd(s, WGPU_REG_MAGIC) == WGPU_MAGIC && rd(s, WGPU_REG_VERSION) == WGPU_VERSION);
    assert(rd(s, WGPU_REG_FEATURES) == WGPU_FEATURE_USER_3D);
    assert(rd(s, WGPU_REG_USER_3D_SUBMITS) == 0);
    assert(wgpu_read(s, BAR0 + 1, 1, &v) && v == 0x47);   /* 'G' */
    assert(rd(s, WGPU_REG_STATUS) == WGPU_STATUS_READY && fake.opens == 1);
    assert(rd(s, WGPU_REG_APERTURE_SIZE) == APERTURE && rd(s, WGPU_REG_APERTURE_GFX) == GFX);
    assert(rd(s, WGPU_REG_LFB_SIZE) == 8u << 20 && rd(s, WGPU_REG_LFB_GFX) == LFB_GFX);
    assert(rd(s, WGPU_REG_GPU_ID) == (0x22b0u | 0x22u << 16));
    assert(!wgpu_read(s, BAR0 + 4096, 4, &v) && !wgpu_read(s, BAR2 + APERTURE, 1, &v));
    puts("WGPU register map, byte reads, decode limits, engine opened on first access: PASS");

    /* the aperture through the interpreter path */
    int syncs = fake.syncs, writes = fake.writes;
    assert(wgpu_write(s, BAR2 + 0x2000, 4, 0x12345678u) && get(0x2000) == 0x12345678u);
    assert(wgpu_read(s, BAR2 + 0x2002, 2, &v) && v == 0x1234);
    const uint8_t text[5] = "abcd";
    assert(wgpu_write_buffer(s, BAR2 + 0x3000, text, 4) && !memcmp(aperture + 0x3000, "abcd", 4));
    assert(!wgpu_write_buffer(s, BAR2 + APERTURE - 2, text, 4));
    /* Stores go through the backend's store (no plain store and flush: the
     * lost-fence write-back of 2026-11-09); the read is synced. */
    assert(fake.syncs == syncs + 1 && fake.writes == writes + 2);
    puts("WGPU aperture reads, writes and string writes with cache maintenance: PASS");

    /* submits */
    wr(s, WGPU_REG_FENCE_OFFSET, 0x40);
    prepare(0xabcdef01u, 7);
    wr(s, WGPU_REG_SUBMIT, 0x100);
    assert(rd(s, WGPU_REG_SUBMIT) == WGPU_SUBMIT_OK);
    assert(get(0x800) == 0xabcdef01u && get(0x40) == 7);
    assert(rd(s, WGPU_REG_COMPLETED) == 7 && rd(s, WGPU_REG_SUBMITTED) == 7 && rd(s, WGPU_REG_SUBMITS) == 1);
    const struct { uint32_t desc, batch, bytes, flags, fence_offset; } refused[] = {
        { 0x108, 0x1000, 24, 0, 0x40 },             /* descriptor misaligned */
        { APERTURE - 8, 0x1000, 24, 0, 0x40 },      /* descriptor past the end */
        { 0x100, 0x1004, 24, 0, 0x40 },             /* batch misaligned */
        { 0x100, 0x1000, 0, 0, 0x40 },              /* empty batch */
        { 0x100, APERTURE - 16, 24, 0, 0x40 },      /* batch past the end */
        { 0x100, 0x1000, 0xfffffff0u, 0, 0x40 },    /* length wrapping */
        { 0x100, 0x1000, 24, 2, 0x40 },             /* unknown flags */
        { 0x100, 0x1000, 24, 0, 0x44 },             /* fence misaligned */
        { 0x100, 0x1000, 24, 0, APERTURE - 4 },     /* fence past the end */
    };
    for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        prepare(1, 8);
        if (refused[i].desc == 0x100) {
            put(0x100, refused[i].batch); put(0x104, refused[i].bytes); put(0x10c, refused[i].flags);
        }
        wr(s, WGPU_REG_FENCE_OFFSET, refused[i].fence_offset);
        int before = fake.submits;
        wr(s, WGPU_REG_SUBMIT, refused[i].desc);
        assert(rd(s, WGPU_REG_SUBMIT) == WGPU_SUBMIT_BAD_DESCRIPTOR && fake.submits == before);
    }
    wr(s, WGPU_REG_FENCE_OFFSET, 0x40);
    fake.ring_full = true;
    prepare(2, 9);
    put(0x10c, WGPU_FLAG_USER_3D);
    wr(s, WGPU_REG_SUBMIT, 0x100);
    assert(rd(s, WGPU_REG_SUBMIT) == WGPU_SUBMIT_RING_FULL && rd(s, WGPU_REG_SUBMITTED) == 7);
    fake.ring_full = false;
    assert(rd(s, WGPU_REG_USER_3D_SUBMITS) == 0); /* refused tag is not activity */
    assert(wgpu_write(s, BAR0 + WGPU_REG_SUBMIT, 2, 0x100) && rd(s, WGPU_REG_SUBMITS) == 1);   /* 16-bit: ignored */
    puts("WGPU submit: batch runs, fence written; 9 malformed descriptors and a full ring refused: PASS");

    /* a hung batch: reset, the owed fence written, ERROR until acknowledged */
    fake.hang = true;
    prepare(3, 10);
    wr(s, WGPU_REG_SUBMIT, 0x100);
    assert(rd(s, WGPU_REG_SUBMIT) == WGPU_SUBMIT_OK && get(0x40) == 7);
    uint32_t st = rd(s, WGPU_REG_STATUS);
    assert(st == (WGPU_STATUS_READY | WGPU_STATUS_ERROR));
    assert(get(0x40) == 10 && rd(s, WGPU_REG_COMPLETED) == 10 && rd(s, WGPU_REG_RESETS) == 1);
    assert(rd(s, WGPU_REG_STATUS) & WGPU_STATUS_ERROR);
    wr(s, WGPU_REG_ERROR_ACK, 1);
    assert(rd(s, WGPU_REG_STATUS) == WGPU_STATUS_READY);
    fake.hang = false;
    prepare(4, 11);
    wr(s, WGPU_REG_SUBMIT, 0x100);
    assert(rd(s, WGPU_REG_COMPLETED) == 11 && get(0x800) == 4);
    assert(fake.writes > writes + 2);   /* the owed fence went through it too */
    puts("WGPU hang: reset noticed, owed fence written, ERROR until ERROR_ACK, submits work again: PASS");

    /* Only accepted explicit application-3D work changes the activity counter. */
    prepare(3, 19);
    put(0x10c, WGPU_FLAG_USER_3D);
    wr(s, WGPU_REG_SUBMIT, 0x100);
    assert(rd(s, WGPU_REG_SUBMIT) == WGPU_SUBMIT_OK);
    assert(rd(s, WGPU_REG_USER_3D_SUBMITS) == 1);
    prepare(4, 20);
    wr(s, WGPU_REG_SUBMIT, 0x100);
    assert(rd(s, WGPU_REG_SUBMIT) == WGPU_SUBMIT_OK);
    assert(rd(s, WGPU_REG_USER_3D_SUBMITS) == 1);
    puts("WGPU explicit 3D classification: accepted tags count; rejected/GDI work does not: PASS");

    /* an engine that cannot be taken: not READY, submits refused, ERROR_ACK retries */
    WgpuState *s2;
    {
        fake.refuse_open = true;
        uint8_t *ap2 = calloc(1, APERTURE);
        WgpuBackend be2 = be;
        be2.aperture = ap2;
        s2 = wgpu_init(bus, &be2);
        assert(s2);
        assert(wgpu_reg_read(s2, WGPU_REG_STATUS) == 0 && wgpu_reg_read(s2, WGPU_REG_APERTURE_GFX) == 0);
        wgpu_reg_write(s2, WGPU_REG_SUBMIT, 0x100);
        assert(wgpu_reg_read(s2, WGPU_REG_SUBMIT) == WGPU_SUBMIT_NO_ENGINE);
        int opens = fake.opens;
        wgpu_reg_read(s2, WGPU_REG_STATUS);
        assert(fake.opens == opens);        /* not retried on every access */
        fake.refuse_open = false;
        wgpu_reg_write(s2, WGPU_REG_ERROR_ACK, 1);
        assert(wgpu_reg_read(s2, WGPU_REG_STATUS) == WGPU_STATUS_READY && fake.opens == opens + 1);
    }
    puts("WGPU without an engine: not ready, submits refused, ERROR_ACK retries the start: PASS");
    puts("WGPU render channel host test: PASS");
    return 0;
}
