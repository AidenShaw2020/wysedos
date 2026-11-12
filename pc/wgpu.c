/*
 * The wyseDOS GPU render channel; see wgpu.h. This file is the PCI device and
 * its registers, independent of the GPU: the platform's WgpuBackend does the
 * engine work.
 */
#include "wgpu.h"
#include <stdlib.h>
#include <string.h>

void *pcmalloc(long size);

#define REG_BYTES 4096u

struct WgpuState {
    PCIDevice *pci;
    WgpuBackend be;
    uint32_t bar0, bar2;                /* 0: unmapped */
    bool opened, open_failed;
    WgpuInfo info;
    uint32_t fence_offset, submit_result, submitted, submits;
    uint32_t resets_seen, resets_acked;
    uint32_t user_3d_submits;
};

static void set_bar(void *opaque, int bar, uint32_t addr, bool enabled)
{
    WgpuState *s = opaque;
    if (bar == 0) {
        s->bar0 = enabled ? addr : 0;
    } else if (bar == 2) {
        s->bar2 = enabled ? addr : 0;
        if (s->be.map_aperture) s->be.map_aperture(s->be.opaque, addr, enabled);
    }
}

WgpuState *wgpu_init(PCIBus *bus, const WgpuBackend *backend)
{
    if (!backend || !backend->aperture || backend->aperture_size < (1u << 20) ||
        (backend->aperture_size & (backend->aperture_size - 1)))
        return NULL;
    WgpuState *s = pcmalloc(sizeof *s);
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    s->be = *backend;
    s->pci = pci_register_device(bus, "WGPU", -1, 0x8086, 0x22b0, 0x00, 0x0b40);
    if (!s->pci) return NULL;
    pci_device_set_config16(s->pci, PCI_SUBSYSTEM_VENDOR_ID, WGPU_SUBSYSTEM_VENDOR);
    pci_device_set_config16(s->pci, PCI_SUBSYSTEM_ID, WGPU_SUBSYSTEM_ID);
    pci_register_bar(s->pci, 0, REG_BYTES, PCI_ADDRESS_SPACE_MEM, s, set_bar);
    pci_register_bar(s->pci, 2, s->be.aperture_size, PCI_ADDRESS_SPACE_MEM_PREFETCH, s, set_bar);
    return s;
}

static bool ensure_open(WgpuState *s)
{
    if (s->opened) return true;
    if (s->open_failed) return false;
    WgpuInfo info = { 0 };
    if (!s->be.open || !s->be.open(s->be.opaque, &info)) {
        s->open_failed = true;
        return false;
    }
    s->info = info;
    s->opened = true;
    s->resets_seen = s->resets_acked = s->be.resets ? s->be.resets(s->be.opaque) : 0;
    return true;
}

static uint32_t aperture_word(WgpuState *s, uint32_t offset)
{
    uint32_t v;
    s->be.sync(s->be.opaque, s->be.aperture + offset, 4);
    memcpy(&v, s->be.aperture + offset, 4);
    return v;
}

/* Every store of the monitor's into the aperture: see WgpuBackend.write. */
static void aperture_store(WgpuState *s, uint8_t *p, const void *src, uint32_t bytes)
{
    if (s->be.write) {
        s->be.write(s->be.opaque, p, src, bytes);
        return;
    }
    memcpy(p, src, bytes);
    s->be.sync(s->be.opaque, p, bytes);
}

static void set_aperture_word(WgpuState *s, uint32_t offset, uint32_t v)
{
    aperture_store(s, s->be.aperture + offset, &v, 4);
}

static bool fence_offset_valid(const WgpuState *s)
{
    return !(s->fence_offset & 7) && s->fence_offset <= s->be.aperture_size - 8;
}

/* After an engine reset nothing will write the fences still owed: write the
 * last one so that waiters wake, and report ERROR. */
static void notice_resets(WgpuState *s)
{
    if (!s->opened || !s->be.resets) return;
    uint32_t resets = s->be.resets(s->be.opaque);
    if (resets == s->resets_seen) return;
    s->resets_seen = resets;
    if (s->submits && fence_offset_valid(s)) set_aperture_word(s, s->fence_offset, s->submitted);
}

static uint32_t status(WgpuState *s)
{
    if (!ensure_open(s)) return 0;
    uint32_t engine = s->be.status ? s->be.status(s->be.opaque) : 0;
    notice_resets(s);
    return (engine & (WGPU_STATUS_READY | WGPU_STATUS_BUSY)) |
           (s->resets_seen != s->resets_acked ? WGPU_STATUS_ERROR : 0);
}

static uint32_t submit(WgpuState *s, uint32_t desc)
{
    if (!ensure_open(s)) return WGPU_SUBMIT_NO_ENGINE;
    notice_resets(s);
    const uint32_t size = s->be.aperture_size;
    if ((desc & 15) || desc > size - 16 || !fence_offset_valid(s)) return WGPU_SUBMIT_BAD_DESCRIPTOR;
    uint32_t d[4];
    s->be.sync(s->be.opaque, s->be.aperture + desc, 16);
    memcpy(d, s->be.aperture + desc, 16);
    const uint32_t batch = d[0], bytes = d[1], fence = d[2];
    if ((batch & 7) || !bytes || batch >= size || bytes > size - batch ||
        (d[3] & ~WGPU_FLAG_USER_3D)) return WGPU_SUBMIT_BAD_DESCRIPTOR;
    int r = s->be.submit(s->be.opaque, s->info.aperture_gfx + batch, s->info.aperture_gfx + s->fence_offset, fence);
    if (r == WGPU_SUBMIT_OK) {
        s->submitted = fence;
        s->submits++;
        if (d[3] & WGPU_FLAG_USER_3D) s->user_3d_submits++;
    }
    return (uint32_t)r;
}

uint32_t wgpu_reg_read(WgpuState *s, uint32_t reg)
{
    switch (reg) {
    case WGPU_REG_MAGIC: return WGPU_MAGIC;
    case WGPU_REG_VERSION: return WGPU_VERSION;
    case WGPU_REG_GPU_ID: return ensure_open(s) ? s->info.gpu_id : 0;
    case WGPU_REG_STATUS: return status(s);
    case WGPU_REG_APERTURE_SIZE: return s->be.aperture_size;
    case WGPU_REG_APERTURE_GFX: return ensure_open(s) ? s->info.aperture_gfx : 0;
    case WGPU_REG_LFB_SIZE: return s->be.lfb_size;
    case WGPU_REG_LFB_GFX: return ensure_open(s) ? s->info.lfb_gfx : 0;
    case WGPU_REG_FENCE_OFFSET: return s->fence_offset;
    case WGPU_REG_SUBMIT: return s->submit_result;
    case WGPU_REG_SUBMITTED: return s->submitted;
    case WGPU_REG_COMPLETED: return fence_offset_valid(s) ? aperture_word(s, s->fence_offset) : 0;
    case WGPU_REG_RESETS: notice_resets(s); return s->resets_seen;
    case WGPU_REG_SUBMITS: return s->submits;
    case WGPU_REG_FEATURES: return WGPU_FEATURE_USER_3D;
    case WGPU_REG_USER_3D_SUBMITS: return s->user_3d_submits;
    default: return 0;
    }
}

void wgpu_reg_write(WgpuState *s, uint32_t reg, uint32_t value)
{
    switch (reg) {
    case WGPU_REG_FENCE_OFFSET: s->fence_offset = value; break;
    case WGPU_REG_SUBMIT: s->submit_result = submit(s, value); break;
    case WGPU_REG_ERROR_ACK:
        notice_resets(s);
        s->resets_acked = s->resets_seen;
        s->open_failed = false;
        break;
    default: break;
    }
}

uint8_t *wgpu_aperture(WgpuState *s, uint32_t *size)
{
    *size = s->be.aperture_size;
    return s->be.aperture;
}

static bool in_regs(const WgpuState *s, uint32_t addr)
{
    return s->bar0 && addr - s->bar0 < REG_BYTES;
}

static bool in_aperture(const WgpuState *s, uint32_t addr, unsigned len)
{
    return s->bar2 && addr - s->bar2 < s->be.aperture_size && len <= s->be.aperture_size - (addr - s->bar2);
}

bool wgpu_read(WgpuState *s, uint32_t addr, unsigned width, uint32_t *value)
{
    if (!s) return false;
    if (in_regs(s, addr)) {
        uint32_t offset = addr - s->bar0, word = wgpu_reg_read(s, offset & ~3u);
        word >>= (offset & 3) * 8;
        *value = width >= 4 ? word : word & ((1u << (width * 8)) - 1);
        return true;
    }
    if (in_aperture(s, addr, width)) {
        uint8_t *p = s->be.aperture + (addr - s->bar2);
        uint32_t v = 0;
        s->be.sync(s->be.opaque, p, width);
        memcpy(&v, p, width);
        *value = v;
        return true;
    }
    return false;
}

bool wgpu_write(WgpuState *s, uint32_t addr, unsigned width, uint32_t value)
{
    if (!s) return false;
    if (in_regs(s, addr)) {
        uint32_t offset = addr - s->bar0;
        if (width == 4 && !(offset & 3)) wgpu_reg_write(s, offset, value);   /* narrower writes are ignored */
        return true;
    }
    if (in_aperture(s, addr, width)) {
        aperture_store(s, s->be.aperture + (addr - s->bar2), &value, width);
        return true;
    }
    return false;
}

bool wgpu_write_buffer(WgpuState *s, uint32_t addr, const uint8_t *buf, unsigned len)
{
    if (!s) return false;
    if (in_regs(s, addr)) return true;
    if (in_aperture(s, addr, len)) {
        aperture_store(s, s->be.aperture + (addr - s->bar2), buf, len);
        return true;
    }
    return false;
}
