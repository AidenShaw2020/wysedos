/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DirectDraw suballocates depth from its own linear video heap. Each
 * allocation has a linear Lock view and a separate Y-tiled GPU view.
 */
#define WGPU_DEPTH_TAG 0x5a300000ul
#define WGPU_DEPTH_LOCKS 0xfffful
#define WGPU_DEPTH_READY 0x10000ul

typedef struct WgpuD3dDepth {
    ULONG width, height, linear_pitch, rows, pitch, tiled;
    BYTE *linear;
} WgpuD3dDepth;

static BOOL wgpu_depth_format(const DDPIXELFORMAT *format)
{
    return format->dwFlags == DDPF_ZBUFFER && format->dwZBufferBitDepth == 16 &&
        (format->dwZBitMask == 0xffff || !format->dwZBitMask);
}

static BOOL wgpu_depth_owned(PDD_SURFACE_LOCAL local)
{
    return local && (local->ddsCaps.dwCaps & DDSCAPS_ZBUFFER) && local->lpGbl &&
        (local->lpGbl->dwReserved1 & ~(WGPU_DEPTH_LOCKS | WGPU_DEPTH_READY)) == WGPU_DEPTH_TAG;
}

static BOOL wgpu_depth_layout(ULONG width, ULONG height, WgpuD3dDepth *depth)
{
    if (!width || width > 2048 || !height || height > 2048) return FALSE;
    depth->width = width;
    depth->height = height;
    depth->linear_pitch = (width * 2 + 63) & ~63u;
    depth->rows = (height + 31) & ~31u;
    depth->pitch = (width * 2 + 127) & ~127u;
    return TRUE;
}

static ULONG wgpu_depth_allocation_size(const WgpuD3dDepth *depth)
{
    return depth->linear_pitch * depth->height + 4095 + depth->rows * depth->pitch;
}

static BOOL wgpu_depth_heap(PDEV *pd, ULONG *first, ULONG *end)
{
    WgpuD3dDepth depth;
    ULONG visible, available, budget, size;
    if (!pd->fb || pd->banked || pd->shadowed || pd->stride <= 0 ||
        !wgpu_depth_layout(pd->w, pd->h, &depth) ||
        pd->h > pd->vram / (ULONG)pd->stride) return FALSE;
    visible = pd->h * pd->stride;
    *end = pd->vram & ~4095u;
    if (*end <= visible) return FALSE;
    available = *end - visible;
    /* Keep an ordinary full-size backbuffer when the offscreen heap fits one. */
    budget = available >= visible ? available - visible : available / 2;
    size = (wgpu_depth_allocation_size(&depth) + 4095) & ~4095u;
    if (size > budget) size = budget & ~4095u;
    if (size < 65536) return FALSE;
    *first = *end - size;
    return TRUE;
}

static BOOL wgpu_depth_create(PDEV *pd, PDD_SURFACE_LOCAL local, DDSURFACEDESC *desc)
{
    WgpuD3dDepth depth;
    PDD_SURFACE_GLOBAL surface = local->lpGbl;
    ULONG first, end, size;
    if ((local->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ||
        !wgpu_depth_format(&surface->ddpfSurface) ||
        !wgpu_depth_layout(surface->wWidth, surface->wHeight, &depth) ||
        !wgpu_depth_heap(pd, &first, &end))
        return FALSE;
    size = wgpu_depth_allocation_size(&depth);
    if (size > end - first) return FALSE;
    surface->lPitch = depth.linear_pitch;
    surface->dwBlockSizeX = size;
    surface->dwBlockSizeY = 1;
    surface->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
    surface->dwReserved1 = WGPU_DEPTH_TAG;
    desc->lPitch = depth.linear_pitch;
    desc->dwFlags |= DDSD_PITCH;
    return TRUE;
}

static BOOL wgpu_depth_surface(PDEV *pd, PDD_SURFACE_LOCAL local, WgpuD3dDepth *depth)
{
    PDD_SURFACE_GLOBAL surface;
    ULONG offset, first, end;
    if (!wgpu_depth_owned(local) ||
        (local->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) || !pd->fb || pd->banked || pd->shadowed)
        return FALSE;
    surface = local->lpGbl;
    if (!wgpu_depth_format(&surface->ddpfSurface) ||
        !wgpu_depth_layout(surface->wWidth, surface->wHeight, depth) ||
        surface->lPitch != (LONG)depth->linear_pitch ||
        !wgpu_depth_heap(pd, &first, &end)) return FALSE;
    offset = (ULONG)surface->fpVidMem - (ULONG)DD_FPBASE(pd);
    if (offset < first || offset >= end || (offset & 63) ||
        wgpu_depth_allocation_size(depth) > end - offset) return FALSE;
    depth->tiled = (offset + depth->height * depth->linear_pitch + 4095) & ~4095u;
    if (depth->tiled >= end || depth->rows * depth->pitch > end - depth->tiled ||
        (unsigned long long)(ULONG_PTR)pd->fb + pd->vram > 0x100000000ull)
        return FALSE;
    depth->linear = (BYTE *)pd->fb + offset;
    return TRUE;
}

static void wgpu_depth_flush(PDEV *pd, const WgpuD3dDepth *depth)
{
    WgpuNtFill range = {0};
    range.offset = depth->tiled;
    range.pitch = depth->pitch;
    range.bpp = 16;
    range.right = depth->pitch / 2;
    range.bottom = depth->rows;
    wgpu_flush_fill(pd, &range);
}

static BOOL wgpu_depth_material(PDEV *pd, PDD_SURFACE_LOCAL local, WgpuNtMaterialDraw *draw,
                                ULONG flags, ULONG compare)
{
    WgpuD3dDepth depth;
    if (!wgpu_depth_surface(pd, local, &depth) ||
        (local->lpGbl->dwReserved1 & WGPU_DEPTH_LOCKS) ||
        depth.width < draw->target.width || depth.height < draw->target.height)
        return FALSE;
    if (!(local->lpGbl->dwReserved1 & WGPU_DEPTH_READY)) {
        wgpu_depth_flush(pd, &depth);
        local->lpGbl->dwReserved1 |= WGPU_DEPTH_READY;
    }
    draw->depth_offset = depth.tiled;
    draw->depth_pitch = depth.pitch;
    draw->depth_flags = flags;
    draw->depth_compare = compare;
    return TRUE;
}

static void wgpu_depth_convert(PDEV *pd, const WgpuD3dDepth *depth, BOOL to_linear)
{
    USHORT *tiles = (USHORT *)((BYTE *)pd->fb + depth->tiled);
    ULONG x, y;
    if (to_linear) wgpu_depth_flush(pd, depth);
    for (y = 0; y < depth->height; ++y) {
        USHORT *row = (USHORT *)(depth->linear + y * depth->linear_pitch);
        for (x = 0; x < depth->width; ++x) {
            ULONG word = wgpu_nt_depth_word(depth->pitch, x, y);
            if (to_linear) row[x] = tiles[word];
            else tiles[word] = row[x];
        }
    }
    if (!to_linear) wgpu_depth_flush(pd, depth);
}

static DWORD APIENTRY wgpu_depth_lock(PDD_LOCKDATA lock)
{
    PDD_SURFACE_LOCAL local = lock->lpDDSurface;
    PDEV *pd = DD_PDEV(lock->lpDD);
    WgpuD3dDepth depth;
    if (!wgpu_depth_owned(local)) return DdLock(lock);
    lock->ddRVal = DDERR_UNSUPPORTED;
    EngAcquireSemaphore(d3d_lock);
    if (wgpu_flush_supported() && wgpu_depth_surface(pd, local, &depth)) {
        PDD_SURFACE_GLOBAL surface = local->lpGbl;
        ULONG count = surface->dwReserved1 & WGPU_DEPTH_LOCKS;
        if (count < WGPU_DEPTH_LOCKS) {
            if (!count) wgpu_depth_convert(pd, &depth, TRUE);
            ++surface->dwReserved1;
            EngReleaseSemaphore(d3d_lock);
            return DdLock(lock);
        }
    }
    EngReleaseSemaphore(d3d_lock);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY wgpu_depth_unlock(PDD_UNLOCKDATA unlock)
{
    PDD_SURFACE_LOCAL local = unlock->lpDDSurface;
    PDEV *pd = DD_PDEV(unlock->lpDD);
    WgpuD3dDepth depth;
    if (!wgpu_depth_owned(local)) return DdUnlock(unlock);
    unlock->ddRVal = DDERR_INVALIDPARAMS;
    EngAcquireSemaphore(d3d_lock);
    if (wgpu_depth_surface(pd, local, &depth) && (local->lpGbl->dwReserved1 & WGPU_DEPTH_LOCKS)) {
        --local->lpGbl->dwReserved1;
        if (!(local->lpGbl->dwReserved1 & WGPU_DEPTH_LOCKS)) {
            wgpu_depth_convert(pd, &depth, FALSE);
            local->lpGbl->dwReserved1 |= WGPU_DEPTH_READY;
        }
        unlock->ddRVal = DD_OK;
    }
    EngReleaseSemaphore(d3d_lock);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD wgpu_depth_blt(PDD_BLTDATA blt)
{
    PDEV *pd = DD_PDEV(blt->lpDD);
    PDD_SURFACE_LOCAL local = blt->lpDDDestSurface;
    WgpuD3dDepth depth;
    RECTL rect = blt->rDest, bounds;
    LONG x, y;
    USHORT *pixels;
    blt->ddRVal = DDERR_UNSUPPORTED;
    if ((blt->dwFlags & ~DDBLT_WAIT) != DDBLT_DEPTHFILL || blt->IsClipped ||
        !(local->ddsCaps.dwCaps & DDSCAPS_ZBUFFER))
        return DDHAL_DRIVER_HANDLED;
    EngAcquireSemaphore(d3d_lock);
    if (!wgpu_flush_supported() || !wgpu_depth_surface(pd, local, &depth)) goto done;
    if (local->lpGbl->dwReserved1 & WGPU_DEPTH_LOCKS) {
        blt->ddRVal = DDERR_SURFACEBUSY;
        goto done;
    }
    bounds.left = bounds.top = 0;
    bounds.right = depth.width;
    bounds.bottom = depth.height;
    if (rect_and(&rect, &bounds)) {
        pixels = (USHORT *)((BYTE *)pd->fb + depth.tiled);
        wgpu_depth_flush(pd, &depth);
        for (y = rect.top; y < rect.bottom; ++y)
            for (x = rect.left; x < rect.right; ++x)
                pixels[wgpu_nt_depth_word(depth.pitch, x, y)] = (USHORT)blt->bltFX.dwFillDepth;
        wgpu_depth_flush(pd, &depth);
        local->lpGbl->dwReserved1 |= WGPU_DEPTH_READY;
    }
    blt->ddRVal = DD_OK;
done:
    EngReleaseSemaphore(d3d_lock);
    return DDHAL_DRIVER_HANDLED;
}
