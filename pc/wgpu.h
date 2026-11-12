/*
 * The wyseDOS GPU render channel ("WGPU"): a PCI device that lends the guest
 * the 3D engine of the Wyse 3040's Intel GPU (Cherryview, Gen8) without the
 * GPU's display, rings or GGTT. The monitor keeps owning all three: the guest
 * builds batch buffers in an aperture of memory the GPU can see and asks the
 * monitor to run them.
 *
 * PCI 8086:22B0, class 0B40 (co-processor, so that Windows does not take it
 * for a second display adapter), subsystem 1234:5747.
 *
 *  BAR0  4 KB      registers, 32-bit accesses (slow: every access is a VM exit)
 *  BAR2  aperture  ordinary memory, mapped write-combining into the guest. The
 *                  GPU sees offset o of it at graphics address APERTURE_GFX + o.
 *
 * The emulated VGA's frame buffer (the QEMU VGA's BAR0, which the monitor
 * scans out) is mapped for the GPU as well, at LFB_GFX. The guest maps that
 * one write-back, so its driver keeps it coherent: CLFLUSH a range the CPU
 * wrote before a batch reads it, and a range a batch wrote before the CPU
 * reads it again (GPU writes do not invalidate CPU caches on Cherryview).
 *
 * Registers (offsets in BAR0):
 *  00  MAGIC          R   'WGPU' (0x55504757)
 *  04  VERSION        R   0x00010000
 *  08  GPU_ID         R   host GPU PCI device id | revision << 16
 *  0C  STATUS         R   bit 0 READY: the engine is usable
 *                         bit 1 ERROR: the engine was reset since ERROR_ACK
 *                               (a batch hung; its work is lost)
 *                         bit 2 BUSY: submitted work has not completed
 *                         Reading STATUS also lets the monitor notice a hang.
 *  10  APERTURE_SIZE  R
 *  14  APERTURE_GFX   R   graphics address of aperture offset 0
 *  18  LFB_SIZE       R
 *  1C  LFB_GFX        R   graphics address of frame buffer offset 0
 *  20  FENCE_OFFSET   RW  aperture offset (8-aligned) that completion values
 *                         are written to; reset value 0
 *  24  SUBMIT         W   aperture offset (16-aligned) of a submit descriptor
 *                     R   result of the last submit (WGPU_SUBMIT_*)
 *  28  SUBMITTED      R   fence value of the last accepted submit
 *  2C  COMPLETED      R   the value at FENCE_OFFSET now
 *  30  ERROR_ACK      W   any value: clear ERROR (and retry a failed start)
 *  34  RESETS         R   engine resets so far
 *  38  SUBMITS        R   accepted submits so far
 *  3C  FEATURES       R   bit 0: descriptor USER_3D classification supported
 *  40  USER_3D_SUBMITS R   successfully accepted USER_3D submissions
 *
 * Submit descriptor (16 bytes in the aperture):
 *  +0 batch offset in the aperture (8-aligned)  +4 batch bytes
 *  +8 fence value                               +12 flags (bit 0: USER_3D)
 * The batch runs as a privileged GGTT batch and must end with
 * MI_BATCH_BUFFER_END. Before it the monitor invalidates the GPU's read
 * caches; after it, flushes the render, depth and data caches with a command
 * streamer stall and then writes the fence value to FENCE_OFFSET. Submits
 * complete in order. If the engine has to be reset, the monitor writes the
 * last accepted fence value itself, so that nothing waits forever, and sets
 * ERROR.
 *
 * Coherence: CPU writes to the aperture are write-combined; a driver issues
 * SFENCE before SUBMIT (the monitor fences again before it starts the batch).
 * GPU writes to the aperture are visible to the CPU once the fence is.
 */
#ifndef WGPU_H
#define WGPU_H

#include <stdbool.h>
#include <stdint.h>
#include "pci.h"

#define WGPU_MAGIC 0x55504757u
#define WGPU_VERSION 0x00010000u
#define WGPU_FEATURE_USER_3D 1u
#define WGPU_FLAG_USER_3D 1u
#define WGPU_SUBSYSTEM_VENDOR 0x1234u
#define WGPU_SUBSYSTEM_ID 0x5747u

enum {
    WGPU_REG_MAGIC = 0x00, WGPU_REG_VERSION = 0x04, WGPU_REG_GPU_ID = 0x08, WGPU_REG_STATUS = 0x0c,
    WGPU_REG_APERTURE_SIZE = 0x10, WGPU_REG_APERTURE_GFX = 0x14, WGPU_REG_LFB_SIZE = 0x18,
    WGPU_REG_LFB_GFX = 0x1c, WGPU_REG_FENCE_OFFSET = 0x20, WGPU_REG_SUBMIT = 0x24,
    WGPU_REG_SUBMITTED = 0x28, WGPU_REG_COMPLETED = 0x2c, WGPU_REG_ERROR_ACK = 0x30,
    WGPU_REG_RESETS = 0x34, WGPU_REG_SUBMITS = 0x38,
    WGPU_REG_FEATURES = 0x3c, WGPU_REG_USER_3D_SUBMITS = 0x40,
};

enum { WGPU_STATUS_READY = 1, WGPU_STATUS_ERROR = 2, WGPU_STATUS_BUSY = 4 };

enum {
    WGPU_SUBMIT_OK = 0,
    WGPU_SUBMIT_BAD_DESCRIPTOR = 1,     /* misaligned or outside the aperture */
    WGPU_SUBMIT_NO_ENGINE = 2,          /* the engine could not be started */
    WGPU_SUBMIT_RING_FULL = 3,          /* the GPU did not make room in time */
};

typedef struct {
    uint32_t aperture_gfx, lfb_gfx, gpu_id;
} WgpuInfo;

/* What the platform provides. The aperture is allocated by the platform
 * because only it knows where memory the GPU can reach comes from. */
typedef struct {
    void *opaque;
    uint8_t *aperture;
    uint32_t aperture_size;             /* a power of two, at least 1 MB */
    uint32_t lfb_size;
    /* Take the engine and map the aperture and frame buffer for the GPU; on
     * the guest's first register access. */
    bool (*open)(void *opaque, WgpuInfo *info);
    /* Queue a batch; returns WGPU_SUBMIT_*. */
    int (*submit)(void *opaque, uint32_t batch_gfx, uint32_t fence_gfx, uint32_t fence_value);
    /* WGPU_STATUS_READY | WGPU_STATUS_BUSY of the engine; may reset a hung one. */
    uint32_t (*status)(void *opaque);
    uint32_t (*resets)(void *opaque);
    /* Cache maintenance for the monitor's own (write-back) accesses to the
     * aperture: write back and invalidate. */
    void (*sync)(void *opaque, const void *cpu, uint32_t bytes);
    /* Map the aperture into the guest at a guest-physical address, or unmap. */
    void (*map_aperture)(void *opaque, uint32_t gpa, bool enabled);
    /* Store bytes into the aperture for the guest (its accesses the monitor
     * interprets) without writing back any other byte of the cache line: a
     * plain store to a line the monitor once read, then a flush, writes the
     * whole stale line back over what the GPU stored since - it lost guest
     * fences (2026-11-09). NULL: store and sync. */
    void (*write)(void *opaque, uint8_t *cpu, const void *src, uint32_t bytes);
} WgpuBackend;

typedef struct WgpuState WgpuState;

WgpuState *wgpu_init(PCIBus *bus, const WgpuBackend *backend);

/* The platform's back end (wyseDOS: gpu3d_guest.c); a weak default in pc.c
 * says there is none. lfb/lfb_size: the emulated VGA's frame buffer. */
bool wgpu_platform_backend(unsigned aperture_mb, uint8_t *lfb, uint32_t lfb_size, WgpuBackend *out);

/* Guest-physical accesses through the interpreter; false: not this device's. */
bool wgpu_read(WgpuState *s, uint32_t addr, unsigned width, uint32_t *value);
bool wgpu_write(WgpuState *s, uint32_t addr, unsigned width, uint32_t value);
bool wgpu_write_buffer(WgpuState *s, uint32_t addr, const uint8_t *buf, unsigned len);

/* Register access by offset, for the monitor's tests. */
uint32_t wgpu_reg_read(WgpuState *s, uint32_t reg);
void wgpu_reg_write(WgpuState *s, uint32_t reg, uint32_t value);
uint8_t *wgpu_aperture(WgpuState *s, uint32_t *size);

#endif
