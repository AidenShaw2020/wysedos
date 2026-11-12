/*
 * The guest's render channel (pc/wgpu.h) on the Cherryview GPU: the aperture
 * is RAM below 4 GB allocated at start, mapped write-combining into the guest
 * (wyse_map_wgpu) and, when the guest first touches the channel's registers,
 * into gpu3d's GGTT region together with the emulated VGA's frame buffer.
 * Batches go through gpu3d's ring (gpu3d_submit_guest).
 */
#include "gpu3d.h"
#include "../../pc/wgpu.h"
#include "../include/efi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern EFI_BOOT_SERVICES *gBS;
void wyse_map_wgpu(uint32_t addr, void *ptr, uint32_t len, int enabled);

static struct {
    uint8_t *aperture, *lfb;
    uint32_t size, lfb_size;
    uint32_t aperture_gfx, lfb_gfx;
} gg;

static uint32_t host_pci(unsigned reg)
{
    uint32_t previous, value;
    __asm__ volatile("inl %1, %0" : "=a"(previous) : "Nd"((uint16_t)0xcf8));
    __asm__ volatile("outl %0, %1" :: "a"(0x80001000u | (reg & 0xfc)), "Nd"((uint16_t)0xcf8));
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"((uint16_t)0xcfc));
    __asm__ volatile("outl %0, %1" :: "a"(previous), "Nd"((uint16_t)0xcf8));
    return value;
}

static bool gg_open(void *opaque, WgpuInfo *info)
{
    (void)opaque;
    if (!gpu3d_open()) {
        printf("wgpu: the render engine could not be taken\n");
        return false;
    }
    if (!gg.aperture_gfx) {           /* once: the GGTT entries stay across gpu3d reopens */
        gg.aperture_gfx = gpu3d_map(gg.aperture, gg.size);
        gg.lfb_gfx = gg.aperture_gfx ? gpu3d_map(gg.lfb, gg.lfb_size) : 0;
        if (!gg.aperture_gfx || !gg.lfb_gfx) {
            printf("wgpu: no room in the GGTT region\n");
            gg.aperture_gfx = 0;      /* the region is not given back; a retry maps again */
            return false;
        }
        printf("wgpu: aperture %u MB at graphics address %08x, frame buffer %u KB at %08x\n", gg.size >> 20,
               gg.aperture_gfx, gg.lfb_size >> 10, gg.lfb_gfx);
    }
    info->aperture_gfx = gg.aperture_gfx;
    info->lfb_gfx = gg.lfb_gfx;
    info->gpu_id = (host_pci(0) >> 16) | (host_pci(8) & 0xff) << 16;
    return true;
}

static int gg_submit(void *opaque, uint32_t batch_gfx, uint32_t fence_gfx, uint32_t fence_value)
{
    (void)opaque;
    return gpu3d_submit_guest(batch_gfx, fence_gfx, fence_value);
}

static uint32_t gg_status(void *opaque)
{
    (void)opaque;
    gpu3d_poll();
    return (gpu3d_is_open() ? WGPU_STATUS_READY : 0) | (gpu3d_busy() ? WGPU_STATUS_BUSY : 0);
}

static uint32_t gg_resets(void *opaque) { (void)opaque; return gpu3d_resets(); }

static void gg_sync(void *opaque, const void *cpu, uint32_t bytes)
{
    (void)opaque;
    gpu3d_flush_cpu(cpu, bytes);
}

static void gg_movnti(uint32_t *at, uint32_t value)
{
    __asm__ volatile("movnti %1, %0" : "=m"(*at) : "r"(value) : "memory");
}

/* WgpuBackend.write: the guest's interpreted stores into the aperture,
 * without a write-back of the rest of their cache lines. The lines are
 * flushed first (a clean copy the monitor read earlier would otherwise take
 * the store and later be written back whole, over a fence the GPU stored
 * meanwhile - it lost guest fences); whole dwords then go out as
 * non-temporal stores, which write only their own bytes; a byte or word is
 * merged into its dword as memory holds it, the line flushed again, and the
 * dword stored the same way. */
static void gg_write(void *opaque, uint8_t *cpu, const void *src, uint32_t bytes)
{
    const uint8_t *from = src;
    (void)opaque;
    gpu3d_flush_cpu(cpu, bytes);
    while (bytes) {
        uintptr_t at = (uintptr_t)cpu;
        if (!(at & 3) && bytes >= 4) {
            uint32_t v;
            memcpy(&v, from, 4);
            gg_movnti((uint32_t *)cpu, v);
            cpu += 4;
            from += 4;
            bytes -= 4;
        } else {
            uint32_t *word = (uint32_t *)(at & ~(uintptr_t)3);
            uint32_t v, part = 4 - (uint32_t)(at & 3);
            if (part > bytes) part = bytes;
            gpu3d_flush_cpu(word, 4);
            v = *(volatile uint32_t *)word;
            memcpy((uint8_t *)&v + (at & 3), from, part);
            gpu3d_flush_cpu(word, 4);
            gg_movnti(word, v);
            cpu += part;
            from += part;
            bytes -= part;
        }
    }
    __asm__ volatile("sfence" ::: "memory");
}

static void gg_map(void *opaque, uint32_t gpa, bool enabled)
{
    (void)opaque;
    wyse_map_wgpu(gpa, gg.aperture, gg.size, enabled);
}

bool wgpu_platform_backend(unsigned aperture_mb, uint8_t *lfb, uint32_t lfb_size, WgpuBackend *out)
{
    if (aperture_mb != 32 && aperture_mb != 64 && aperture_mb != 128) {
        printf("wgpu: aperture of %u MB not offered (32, 64 or 128)\n", aperture_mb);
        return false;
    }
    if (((uintptr_t)lfb & 4095) || !lfb_size) return false;
    if (!gg.aperture) {
        uint32_t bytes = aperture_mb << 20;
        EFI_PHYSICAL_ADDRESS at = 0xffffffffu;
        if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, bytes / 4096, &at))) {
            printf("wgpu: no %u MB below 4 GB for the aperture\n", aperture_mb);
            return false;
        }
        gg.aperture = (uint8_t *)(uintptr_t)at;
        gg.size = bytes;
        memset(gg.aperture, 0, bytes);
        gpu3d_flush_cpu(gg.aperture, bytes);  /* no write-back line may later land on what the GPU wrote */
        gg.lfb = lfb;
        gg.lfb_size = (lfb_size + 4095) & ~4095u;
    }
    memset(out, 0, sizeof *out);
    out->aperture = gg.aperture;
    out->aperture_size = gg.size;
    out->lfb_size = gg.lfb_size;
    out->open = gg_open;
    out->submit = gg_submit;
    out->status = gg_status;
    out->resets = gg_resets;
    out->sync = gg_sync;
    out->map_aperture = gg_map;
    out->write = gg_write;
    return true;
}

/* ---- "wgpu" monitor command: the channel exercised as a guest would ---- */

uint64_t wyse_ticks_us(void);

#define TEST_BYTES (256u * 1024u)     /* the top of the aperture: the guest must not be using it */
#define MI_STORE_DATA_IMM_GGTT ((0x20u << 23) | (1u << 22) | 2)
#define MI_BATCH_BUFFER_END (0x0au << 23)

static uint8_t *ap(WgpuState *s, uint32_t *test)
{
    uint32_t size;
    uint8_t *a = wgpu_aperture(s, &size);
    *test = size - TEST_BYTES;
    return a;
}

/* Submit the batch at aperture offset batch (bytes long) with fence and wait
 * for COMPLETED to show it. */
static bool run(WgpuState *s, uint32_t batch, uint32_t bytes, uint32_t fence, uint32_t timeout_us, bool wait,
                uint64_t *took)
{
    uint32_t t;
    uint8_t *a = ap(s, &t);
    uint32_t *d = (uint32_t *)(a + t + 64);
    d[0] = batch; d[1] = bytes; d[2] = fence; d[3] = 0;
    gpu3d_flush_cpu(d, 16);
    wgpu_reg_write(s, WGPU_REG_FENCE_OFFSET, t);
    uint64_t t0 = wyse_ticks_us();
    wgpu_reg_write(s, WGPU_REG_SUBMIT, t + 64);
    uint32_t r = wgpu_reg_read(s, WGPU_REG_SUBMIT);
    if (r != WGPU_SUBMIT_OK) {
        printf("wgpu: submit refused (%u)\n", r);
        return false;
    }
    if (!wait) return true;
    while (wyse_ticks_us() - t0 < timeout_us)
        if (wgpu_reg_read(s, WGPU_REG_COMPLETED) == fence) {
            if (took) *took = wyse_ticks_us() - t0;
            return true;
        }
    printf("wgpu: fence %08x not seen in %u ms (completed %08x, status %x)\n", fence, timeout_us / 1000,
           wgpu_reg_read(s, WGPU_REG_COMPLETED), wgpu_reg_read(s, WGPU_REG_STATUS));
    return false;
}

static uint32_t fence_counter = 0x10000;
/* The target is on another page than the fence on purpose. The monitor sees
 * the aperture write-back; while it polls the fence (CLFLUSH, read), the
 * CPU's prefetchers fetch neighbouring lines, and a fetch issued before the
 * GPU's stores landed fills the cache with old data after the CLFLUSH of the
 * target: with the target two lines from the fence the first read after the
 * fence was stale every time, on another page never (physical, 2026-11-08).
 * The GPU's ordering is right; the guest maps the aperture write-combining,
 * which is not cached at all. */
static const uint32_t target_off = 12288;

static bool test_basic(WgpuState *s)
{
    uint32_t t;
    uint8_t *a = ap(s, &t);
    const uint32_t gfx = wgpu_reg_read(s, WGPU_REG_APERTURE_GFX);
    uint32_t *target = (uint32_t *)(a + t + target_off), *b = (uint32_t *)(a + t + 4096), n = 0;
    for (unsigned i = 0; i < 16; i++) target[i] = 0;
    gpu3d_flush_cpu(target, 64);
    for (unsigned i = 0; i < 16; i++) {
        b[n++] = MI_STORE_DATA_IMM_GGTT; b[n++] = gfx + t + target_off + 4 * i; b[n++] = 0; b[n++] = 0xc0de0000u + i;
    }
    b[n++] = MI_BATCH_BUFFER_END; b[n++] = 0;
    gpu3d_flush_cpu(b, n * 4);
    uint64_t took = 0;
    if (!run(s, t + 4096, n * 4, ++fence_counter, 1000000, true, &took)) return false;
    /* how long after the fence the batch's own stores show (diagnosis) */
    uint64_t f0 = wyse_ticks_us(), lag = 0;
    unsigned polls = 0;
    for (;;) {
        gpu3d_flush_cpu(target, 64);
        polls++;
        if (target[0] == 0xc0de0000u && target[15] == 0xc0de000fu) { lag = wyse_ticks_us() - f0; break; }
        if (wyse_ticks_us() - f0 > 100000) { lag = ~0ull; break; }
    }
    printf("wgpu test: stores visible %llu us after the fence (%u polls)\n", (unsigned long long)lag, polls);
    gpu3d_flush_cpu(target, 64);
    unsigned right = 0;
    for (unsigned i = 0; i < 16; i++) right += target[i] == 0xc0de0000u + i;
    printf("wgpu test: %u of 16 stores arrived, first submit %llu us\n", right, (unsigned long long)took);
    if (right != 16) return false;
    /* latency: one at a time; throughput: a queue of them, waiting for the last */
    uint64_t sum = 0;
    for (unsigned i = 0; i < 200; i++) {
        if (!run(s, t + 4096, n * 4, ++fence_counter, 1000000, true, &took)) return false;
        sum += took;
    }
    uint64_t t0 = wyse_ticks_us();
    for (unsigned i = 0; i < 1000; i++)
        if (!run(s, t + 4096, n * 4, ++fence_counter, 1000000, i == 999, NULL)) return false;
    uint64_t queue = wyse_ticks_us() - t0;
    printf("wgpu test: submit-to-fence %llu us on average (200), 1000 queued in %llu us - PASS\n",
           (unsigned long long)(sum / 200), (unsigned long long)queue);
    return true;
}

/* A gradient block written into the emulated VGA's frame buffer by the GPU. */
static void test_lfb(WgpuState *s, uint32_t pitch)
{
    uint32_t t;
    uint8_t *a = ap(s, &t);
    const uint32_t lfb = wgpu_reg_read(s, WGPU_REG_LFB_GFX);
    if (pitch < 128 || pitch > 8192 || 64 * pitch > gg.lfb_size) { printf("wgpu lfb: pitch %u?\n", pitch); return; }
    uint32_t *b = (uint32_t *)(a + t + 4096), n = 0;
    for (unsigned y = 0; y < 64; y++)
        for (unsigned x = 0; x < 32; x++) {
            b[n++] = MI_STORE_DATA_IMM_GGTT; b[n++] = lfb + y * pitch + 4 * x; b[n++] = 0;
            b[n++] = 0x01010101u * ((y * 4 + x / 8) & 0xff);
        }
    b[n++] = MI_BATCH_BUFFER_END; b[n++] = 0;
    gpu3d_flush_cpu(b, n * 4);
    for (unsigned y = 0; y < 64; y++) gpu3d_flush_cpu(gg.lfb + y * pitch, 128);   /* no CPU line over the block */
    uint64_t took = 0;
    if (!run(s, t + 4096, n * 4, ++fence_counter, 1000000, true, &took)) return;
    unsigned right = 0;
    for (unsigned y = 0; y < 64; y++) {
        gpu3d_flush_cpu(gg.lfb + y * pitch, 128);
        for (unsigned x = 0; x < 32; x++)
            right += ((uint32_t *)(gg.lfb + y * pitch))[x] == 0x01010101u * ((y * 4 + x / 8) & 0xff);
    }
    printf("wgpu lfb: %u of 2048 dwords at the frame buffer's top left (pitch %u), %llu us - %s\n", right, pitch,
           (unsigned long long)took, right == 2048 ? "PASS" : "FAIL");
}

/* A batch that jumps to itself: the channel must reset the engine, write the
 * owed fence, report ERROR, and work again after ERROR_ACK. */
static void test_hang(WgpuState *s)
{
    uint32_t t;
    uint8_t *a = ap(s, &t);
    const uint32_t gfx = wgpu_reg_read(s, WGPU_REG_APERTURE_GFX), resets = wgpu_reg_read(s, WGPU_REG_RESETS);
    uint32_t *b = (uint32_t *)(a + t + 4096);
    b[0] = (0x31u << 23) | 1; b[1] = gfx + t + 4096; b[2] = 0; b[3] = 0;   /* MI_BATCH_BUFFER_START to itself */
    gpu3d_flush_cpu(b, 16);
    uint32_t fence = ++fence_counter;
    if (!run(s, t + 4096, 16, fence, 0, false, NULL)) return;
    uint64_t t0 = wyse_ticks_us();
    uint32_t status = 0;
    while (wyse_ticks_us() - t0 < 8000000) {
        status = wgpu_reg_read(s, WGPU_REG_STATUS);
        if (status & WGPU_STATUS_ERROR) break;
        uint64_t t1 = wyse_ticks_us();
        while (wyse_ticks_us() - t1 < 50000) {}
    }
    uint32_t completed = wgpu_reg_read(s, WGPU_REG_COMPLETED), now = wgpu_reg_read(s, WGPU_REG_RESETS);
    printf("wgpu hang: after %llu ms status %x, resets %u -> %u, owed fence %s\n",
           (unsigned long long)((wyse_ticks_us() - t0) / 1000), status, resets, now,
           completed == fence ? "written" : "NOT written");
    wgpu_reg_write(s, WGPU_REG_ERROR_ACK, 1);
    bool again = (status & WGPU_STATUS_ERROR) && completed == fence && test_basic(s);
    printf("wgpu hang: %s\n", again ? "recovered - PASS" : "FAIL");
}

/* Diagnosis: what the CPU sees of the test batch and target, the batch
 * registers, and the same batch through the monitor's own path. */
static void dump(WgpuState *s, bool exec)
{
    uint32_t t;
    uint8_t *a = ap(s, &t);
    uint32_t *b = (uint32_t *)(a + t + 4096), *target = (uint32_t *)(a + t + 128);
    const uint32_t gfx = wgpu_reg_read(s, WGPU_REG_APERTURE_GFX);
    if (exec) {
        for (unsigned i = 0; i < 16; i++) target[i] = 0;
        gpu3d_flush_cpu(target, 64);
        bool ok = gpu3d_execute(gfx + t + 4096, gpu3d_next_seqno(), 1000000);
        printf("wgpu dump: the test batch through gpu3d_execute: %s\n", ok ? "finished" : "TIMEOUT");
    }
    gpu3d_flush_cpu(b, 80);
    gpu3d_flush_cpu(target, 64);
    printf("wgpu dump: batch at %08x: %08x %08x %08x %08x  %08x %08x %08x %08x ... %08x %08x\n", gfx + t + 4096,
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[64], b[65]);
    printf("wgpu dump: target at %08x: %08x %08x %08x %08x\n", gfx + t + 128, target[0], target[1], target[2],
           target[15]);
    printf("wgpu dump: BB_ADDR %08x BB_STATE %08x ACTHD %08x IPEHR %08x INSTDONE %08x EIR %08x ESR %08x\n",
           gpu3d_read(0x2140), gpu3d_read(0x2110), gpu3d_read(0x2074), gpu3d_read(0x2068), gpu3d_read(0x206c),
           gpu3d_read(0x20b0), gpu3d_read(0x20b8));
}

/* What the guest's driver keeps in the aperture: `count` dwords from a hex
 * offset, read past the monitor's cache (the guest writes them
 * write-combined, the GPU without snooping). */
static void peek(WgpuState *s, const char *args)
{
    uint32_t size;
    uint8_t *a = wgpu_aperture(s, &size);
    char *end;
    uint32_t offset = (uint32_t)strtoul(args, &end, 16) & ~3u;
    uint32_t count = *end ? (uint32_t)strtoul(end, NULL, 0) : 8;
    if (!a || !size) { printf("wgpu peek: no aperture yet\n"); return; }
    if (count == 0 || count > 64) count = 8;
    if (offset >= size || count * 4 > size - offset) { printf("wgpu peek: outside the aperture\n"); return; }
    gpu3d_flush_cpu(a + offset, count * 4);
    for (uint32_t i = 0; i < count; i += 8) {
        printf("wgpu peek %06x:", (unsigned)(offset + i * 4));
        for (uint32_t j = i; j < count && j < i + 8; j++) printf(" %08x", (unsigned)((uint32_t *)(a + offset))[j]);
        printf("\n");
    }
}

void wgpu_command(WgpuState *s, const char *args)
{
    if (!s) {
        printf("wgpu: no render channel (config.ini [pc] wgpu=32|64|128, then restart)\n");
        return;
    }
    while (*args == ' ') args++;
    if (!strcmp(args, "dump")) dump(s, false);
    else if (!strcmp(args, "exec")) dump(s, true);
    else if (!strcmp(args, "test")) test_basic(s);
    else if (!strncmp(args, "lfb", 3)) test_lfb(s, args[3] == ' ' ? (uint32_t)strtoul(args + 4, NULL, 0) : 640);
    else if (!strcmp(args, "hang")) test_hang(s);
    else if (!strncmp(args, "peek ", 5)) peek(s, args + 5);
    else if (*args) printf("wgpu: test | lfb [pitch] | hang\n");
    uint32_t status = wgpu_reg_read(s, WGPU_REG_STATUS);
    printf("wgpu: status %x (ready %u error %u busy %u), aperture %u MB at %08x, lfb %u KB at %08x, gpu %08x\n",
           status, status & 1, (status >> 1) & 1, (status >> 2) & 1, wgpu_reg_read(s, WGPU_REG_APERTURE_SIZE) >> 20,
           wgpu_reg_read(s, WGPU_REG_APERTURE_GFX), wgpu_reg_read(s, WGPU_REG_LFB_SIZE) >> 10,
           wgpu_reg_read(s, WGPU_REG_LFB_GFX), wgpu_reg_read(s, WGPU_REG_GPU_ID));
    printf("wgpu: submits %u, last fence %08x, completed %08x, resets %u\n", wgpu_reg_read(s, WGPU_REG_SUBMITS),
           wgpu_reg_read(s, WGPU_REG_SUBMITTED), wgpu_reg_read(s, WGPU_REG_COMPLETED),
           wgpu_reg_read(s, WGPU_REG_RESETS));
    printf("wgpu: features %x, accepted Windows 3D submits %u\n",
           wgpu_reg_read(s, WGPU_REG_FEATURES),
           wgpu_reg_read(s, WGPU_REG_USER_3D_SUBMITS));
    gpu3d_report();
}
