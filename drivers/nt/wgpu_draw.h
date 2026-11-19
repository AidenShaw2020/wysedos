/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WGPU_NT_DRAW_H
#define WGPU_NT_DRAW_H
#include "wgpu_fill.h"

#define WGPU_NT_IOCTL_DRAW 0x0022e014ul
#define WGPU_NT_DRAW_MAX_VERTICES 96u

typedef struct WgpuNtVertex {
    /* IEEE-754 screen coordinates and packed ARGB, without kernel FP. */
    WgpuNtU32 x, y, argb;
} WgpuNtVertex;

typedef struct WgpuNtDraw {
    WgpuNtFill target;
    WgpuNtU32 size, vertex_count;
    WgpuNtVertex vertices[WGPU_NT_DRAW_MAX_VERTICES];
} WgpuNtDraw;
typedef char WgpuNtDrawSize[sizeof(WgpuNtDraw)==1212 ? 1 : -1];

static __inline int wgpu_nt_draw_valid(const WgpuNtDraw *q,
                                      WgpuNtU32 bytes, WgpuNtU32 gfx)
{
    unsigned i;
    if (!q || q->size!=sizeof(*q) ||
        !wgpu_nt_fill_valid(&q->target,bytes,gfx) || q->target.color ||
        q->target.left || q->target.top ||
        q->target.right!=q->target.width || q->target.bottom!=q->target.height ||
        !q->vertex_count || q->vertex_count>WGPU_NT_DRAW_MAX_VERTICES ||
        q->vertex_count%3) return 0;
    for (i=0; i<q->vertex_count; ++i) {
        /* Reject NaNs and infinities and bound the magnitude. Signed screen
         * coordinates allow the rasterizer to clip at the top and left. */
        WgpuNtU32 x=q->vertices[i].x, y=q->vertices[i].y;
        if ((x&0x7fffffffu)>0x45000000u || (y&0x7fffffffu)>0x45000000u)
            return 0;
    }
    return 1;
}

WgpuNtU32 wgpu_nt_build_draw(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags,
    const WgpuNtDraw *draw, WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx);
#endif
