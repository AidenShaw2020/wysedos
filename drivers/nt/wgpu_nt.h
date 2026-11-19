/* SPDX-License-Identifier: GPL-3.0-or-later
 * NT 5 WGPU bridge ABI. Protocol authority: pc/wgpu.h.
 * This interface exposes queries and a fixed GPU self-test, not raw batches.
 */
#ifndef WGPU_NT_H
#define WGPU_NT_H

typedef unsigned long WgpuNtU32;
typedef char WgpuNtU32MustBe32Bits[sizeof(WgpuNtU32) == 4 ? 1 : -1];

#define WGPU_NT_ABI_VERSION 1ul
/* CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800/0x801, METHOD_BUFFERED, READ/READ|WRITE). */
#define WGPU_NT_IOCTL_QUERY    0x00226000ul
#define WGPU_NT_IOCTL_SELFTEST 0x0022e004ul
#define WGPU_NT_IOCTL_TRIANGLE 0x0022e008ul
#define WGPU_NT_IOCTL_DRAW_TEST 0x0022e018ul
#define WGPU_NT_IOCTL_TIMING   0x00226020ul
#define WGPU_NT_IOCTL_PIPELINE_STATS 0x00226030ul
#define WGPU_NT_IOCTL_TEMPLATE_STATS 0x00226034ul
#define WGPU_NT_IOCTL_QUEUE_STATS 0x00226038ul
#define WGPU_NT_MAGIC 0x55504757ul
#define WGPU_NT_PROTOCOL 0x00010000ul
#define WGPU_NT_REGISTER_BYTES 4096ul
#define WGPU_NT_APERTURE_MIN 8192ul
#define WGPU_NT_DESCRIPTOR 0x0100ul
#define WGPU_NT_FLAG_USER_3D 1ul
#define WGPU_NT_BATCH 0x1000ul
#define WGPU_NT_TARGET 0x0010ul

/* Fixed offscreen RGB565 diagnostic; no caller-provided GPU addresses. */
#define WGPU_NT_TRI_WIDTH 128ul
#define WGPU_NT_TRI_HEIGHT 128ul
#define WGPU_NT_TRI_KERNEL 0x8000ul
#define WGPU_NT_TRI_BATCH 0x10000ul
#define WGPU_NT_TRI_BATCH_BYTES 0x10000ul
#define WGPU_NT_TRI_TARGET 0x20000ul
#define WGPU_NT_TRI_MIN 0x28000ul
#define WGPU_NT_TRI_BACKGROUND 0x1082u

typedef struct WgpuNtTriangle {
    WgpuNtU32 size, abi_version, width, height, pitch, fence, flags;
    unsigned short pixels[WGPU_NT_TRI_WIDTH * WGPU_NT_TRI_HEIGHT];
} WgpuNtTriangle;
typedef char WgpuNtTriangleMustBe32796Bytes[sizeof(WgpuNtTriangle) == 32796 ? 1 : -1];
int wgpu_nt_check_draw_test(const WgpuNtTriangle *image);

typedef struct WgpuNtInfo {
    WgpuNtU32 size, abi_version;
    WgpuNtU32 magic, protocol, gpu_id, status;
    WgpuNtU32 aperture_bytes, aperture_gfx, lfb_bytes, lfb_gfx;
    WgpuNtU32 submitted, completed, resets, submits, user_3d_submits;
    WgpuNtU32 smoke_value, smoke_expected, smoke_fence, smoke_result;
} WgpuNtInfo;
typedef char WgpuNtInfoMustBe76Bytes[sizeof(WgpuNtInfo) == 76 ? 1 : -1];

typedef struct WgpuNtTiming {
    WgpuNtU32 size, abi_version, surface_requests, scheduler_sleeps;
    long long frequency, prepare_ticks, build_ticks, doorbell_ticks, fence_ticks;
    unsigned long long input_bytes;
} WgpuNtTiming;
typedef char WgpuNtTimingMustBe64Bytes[sizeof(WgpuNtTiming) == 64 ? 1 : -1];
typedef struct WgpuNtPipelineStats {
    WgpuNtU32 queued, prepared_while_pending, retired, pending_fence;
} WgpuNtPipelineStats;
typedef struct WgpuNtTemplateStats {
    WgpuNtU32 lookups, hits;
} WgpuNtTemplateStats;
typedef struct WgpuNtQueueStats {
    WgpuNtU32 slot_retirements, max_pending;
} WgpuNtQueueStats;

/* The driver only compares fences within its bounded in-flight queue. */
static __inline int wgpu_nt_fence_reached(WgpuNtU32 completed, WgpuNtU32 requested)
{
    return completed && requested && (WgpuNtU32)(completed - requested) < 0x80000000ul;
}

/* Only writes fixed ranges [0,4), [0x10,0x14), [0x100,0x110), [0x1000,0x1018).
 * Caller serializes ownership, verifies idle and fences WC stores before submit.
 */
int wgpu_nt_build_smoke(volatile WgpuNtU32 *aperture, WgpuNtU32 aperture_bytes,
                       WgpuNtU32 aperture_gfx, WgpuNtU32 completed,
                       WgpuNtU32 fence, WgpuNtU32 value);
WgpuNtU32 wgpu_nt_build_triangle(volatile WgpuNtU32 *aperture, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags);
/* Integer barycentric reference, tolerating RGB565 rounding and raster edges. */
int wgpu_nt_check_triangle(const WgpuNtTriangle *image);
#endif
