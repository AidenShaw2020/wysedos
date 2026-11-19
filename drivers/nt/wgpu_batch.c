/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wgpu_nt.h"

int wgpu_nt_build_smoke(volatile WgpuNtU32 *aperture, WgpuNtU32 aperture_bytes,
                       WgpuNtU32 aperture_gfx, WgpuNtU32 completed,
                       WgpuNtU32 fence, WgpuNtU32 value)
{
    volatile WgpuNtU32 *descriptor;
    volatile WgpuNtU32 *batch;
    if (!aperture || aperture_bytes < WGPU_NT_APERTURE_MIN || !fence ||
        (aperture_gfx & 4095ul) || aperture_gfx > 0xfffffffful - aperture_bytes) {
        return 0;
    }
    descriptor = aperture + WGPU_NT_DESCRIPTOR / 4;
    batch = aperture + WGPU_NT_BATCH / 4;
    aperture[0] = completed;
    aperture[WGPU_NT_TARGET / 4] = 0;
    /* Gen8 PRM Vol 2a: MI_STORE_DATA_IMM, global GTT, length 2;
     * same fixed maintenance batch as Velocity9x eng_wgpu.c's smoke test. */
    batch[0] = 0x10400002ul;
    batch[1] = aperture_gfx + WGPU_NT_TARGET;
    batch[2] = 0;
    batch[3] = value;
    batch[4] = 0x05000000ul; /* MI_BATCH_BUFFER_END */
    batch[5] = 0;
    descriptor[0] = WGPU_NT_BATCH;
    descriptor[1] = 24;
    descriptor[2] = fence;
    descriptor[3] = 0; /* maintenance; never claim a Direct3D submission */
    return 1;
}
