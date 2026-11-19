/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded framebuffer colour fill. Addresses are derived by the WDM owner;
 * callers supply only a surface offset and integer geometry, never a batch.
 */
#ifndef WGPU_NT_FILL_H
#define WGPU_NT_FILL_H
#include "wgpu_nt.h"
#define WGPU_NT_IOCTL_FILL 0x0022e00cul
#define WGPU_NT_IOCTL_COPY 0x0022e010ul
typedef struct WgpuNtFill {
    WgpuNtU32 size, abi_version, offset, pitch, width, height, bpp;
    WgpuNtU32 left, top, right, bottom, color, reserved;
} WgpuNtFill;
/* Two colorless surface rectangles. Version and size apply to each rectangle. */
typedef struct WgpuNtCopy {
    WgpuNtFill src, dst;
} WgpuNtCopy;
typedef char WgpuNtCopySize[sizeof(WgpuNtCopy)==104 ? 1 : -1];
typedef struct WgpuNtFillResult {
    WgpuNtU32 size, abi_version, fence, reserved;
} WgpuNtFillResult;
typedef char WgpuNtFillSize[sizeof(WgpuNtFill)==52 ? 1 : -1];
typedef char WgpuNtFillResultSize[sizeof(WgpuNtFillResult)==16 ? 1 : -1];

static __inline int wgpu_nt_fill_valid(const WgpuNtFill *q,
                                      WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx)
{
    unsigned long long end;
    if (!q || q->size!=sizeof(*q) || q->abi_version!=WGPU_NT_ABI_VERSION ||
        q->reserved || (q->bpp!=16 && q->bpp!=32) ||
        !q->width || q->width>2048 || !q->height || q->height>2048 ||
        !q->pitch || q->pitch>16384 || (q->pitch & 63) || (q->offset & 63) ||
        q->width*(q->bpp/8)>q->pitch ||
        q->left>=q->right || q->right>q->width ||
        q->top>=q->bottom || q->bottom>q->height ||
        (q->bpp==16 && q->color>0xffff) || !lfb_gfx || (lfb_gfx & 4095)) return 0;
    end=(unsigned long long)q->offset+(unsigned long long)q->pitch*q->height;
    return end<=lfb_bytes && (unsigned long long)lfb_gfx+end<=0x100000000ull;
}

WgpuNtU32 wgpu_nt_build_fill(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags,
    const WgpuNtFill *fill, WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx);

static __inline int wgpu_nt_copy_valid(const WgpuNtCopy *q,
                                      WgpuNtU32 bytes, WgpuNtU32 gfx)
{
    if (!q || !wgpu_nt_fill_valid(&q->src,bytes,gfx) ||
        !wgpu_nt_fill_valid(&q->dst,bytes,gfx) ||
        q->src.color || q->dst.color || q->src.bpp!=q->dst.bpp ||
        q->src.right-q->src.left!=q->dst.right-q->dst.left ||
        q->src.bottom-q->src.top!=q->dst.bottom-q->dst.top) return 0;
    /* Disjoint full allocations avoid texture/render aliasing, including
     * padding and prefetch. Same-surface copies retain CPU semantics. */
    return (unsigned long long)q->src.offset+q->src.pitch*q->src.height<=q->dst.offset ||
           (unsigned long long)q->dst.offset+q->dst.pitch*q->dst.height<=q->src.offset;
}

WgpuNtU32 wgpu_nt_build_copy(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags,
    const WgpuNtCopy *copy, WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx);
#endif
