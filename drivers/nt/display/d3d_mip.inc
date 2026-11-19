/* SPDX-License-Identifier: GPL-3.0-or-later */
typedef struct WgpuMipAllocation {
    struct WgpuMipAllocation *next;
    PDEV *device;
    PDD_SURFACE_GLOBAL root;
    ULONG offset, bytes;
    ULONG live, levels;
} WgpuMipAllocation;

/* The mip pool occupies the end of the color heap, before the depth heap.
 * Keep room for ordinary textures and a fullscreen backbuffer when there
 * is room for both. At tighter desktop modes reserve a smaller share.
 */
static ULONG wgpu_mip_pool_first(PDEV *pd, ULONG end)
{
    ULONG visible = pd->h * pd->stride;
    ULONG first = (end / pd->stride) * pd->stride;
    ULONG available, bytes;
    if ((pd->stride & 63) || first <= visible) return first;
    available = first - visible;
    if (available > visible && available - visible >= 128 * 1024)
        bytes = (available - visible) / 2;
    else
        bytes = available / 4;
    if (bytes > 4 * 1024 * 1024) bytes = 4 * 1024 * 1024;
    return first - (bytes / pd->stride) * pd->stride;
}
static WgpuMipAllocation *wgpu_mip_allocations;

static WgpuMipAllocation **wgpu_mip_find(PDD_SURFACE_GLOBAL surface)
{
    WgpuMipAllocation **entry = &wgpu_mip_allocations;
    while (*entry && (ULONG_PTR)*entry != surface->dwReserved1)
        entry = &(*entry)->next;
    return entry;
}

static void wgpu_mip_create(PDEV *pd, PDD_CREATESURFACEDATA data)
{
    const DWORD levels = data->dwSCnt;
    WgpuNtMipLayout layout;
    WgpuMipAllocation *allocation;
    PDD_SURFACE_GLOBAL top;
    ULONG width, height, texel, bytes, i, format, first, end, offset;
    WgpuMipAllocation **entry;
    HRESULT status = DDERR_UNSUPPORTED;

    if (!data->lplpSList || levels < 2 || levels > WGPU_NT_MIP_LEVELS_MAX ||
        !data->lplpSList[0] || !(top = data->lplpSList[0]->lpGbl)) goto failed;
    width = top->wWidth;
    height = top->wHeight;
    format = wgpu_d3d_texture_format(&top->ddpfSurface);
    texel = top->ddpfSurface.dwRGBBitCount / 8;
    if (!format || !wgpu_nt_mip_layout(width, height, levels, texel, &layout) ||
        !wgpu_depth_heap(pd, &first, &end)) goto failed;
    end = (first / pd->stride) * pd->stride;
    offset = wgpu_mip_pool_first(pd, first);
    bytes = (layout.pitch * layout.rows + 63) & ~63ul;
    for (i = 0; i < levels; ++i) {
        PDD_SURFACE_LOCAL local = data->lplpSList[i];
        PDD_SURFACE_GLOBAL level;
        if (!local || !(level = local->lpGbl) ||
            (local->ddsCaps.dwCaps & (DDSCAPS_TEXTURE | DDSCAPS_MIPMAP)) !=
                (DDSCAPS_TEXTURE | DDSCAPS_MIPMAP) ||
            (local->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ||
            level->wWidth != width || level->wHeight != height) goto failed;
        if (width > 1) width /= 2;
        if (height > 1) height /= 2;
    }

    status = DDERR_OUTOFVIDEOMEMORY;
    EngAcquireSemaphore(d3d_lock);
    /* Sort by offset and fill gaps, so deletion recycles space immediately. */
    entry = &wgpu_mip_allocations;
    while (*entry) {
        WgpuMipAllocation *current = *entry;
        if (current->device == pd) {
            if (offset <= current->offset && bytes <= current->offset - offset)
                break;
            offset = current->offset + current->bytes;
        }
        entry = &current->next;
    }
    D3D_TRACE(texture_pitch, layout.pitch);
    D3D_TRACE(texture_block, end > offset ? end - offset : 0);
    allocation = offset < end && bytes <= end - offset ?
        EngAllocMem(0, sizeof(*allocation), 'pMgW') : NULL;
    if (allocation) {
        allocation->device = pd;
        allocation->root = top;
        allocation->offset = offset;
        allocation->bytes = bytes;
        allocation->live = allocation->levels = levels;
        allocation->next = *entry;
        *entry = allocation;
        for (i = 0; i < levels; ++i) {
            PDD_SURFACE_GLOBAL level = data->lplpSList[i]->lpGbl;
            level->fpVidMem = offset + layout.offset[i];
            level->lPitch = layout.pitch;
            level->lpVidMemHeap = NULL;
            level->fpHeapOffset = level->fpVidMem;
            level->dwReserved1 = (ULONG_PTR)allocation;
        }
        data->lpDDSurfaceDesc->lPitch = layout.pitch;
        data->lpDDSurfaceDesc->dwFlags |= DDSD_PITCH;
        status = DD_OK;
    }
    EngReleaseSemaphore(d3d_lock);
failed:
    D3D_TRACE(state, levels);
    D3D_TRACE(value, status);
    data->ddRVal = status;
}

/* Called with d3d_lock by the material setup. Only the root has a mip tree. */
static ULONG wgpu_mip_levels(PDEV *pd, PDD_SURFACE_GLOBAL surface)
{
    WgpuMipAllocation *allocation;
    if (!surface->dwReserved1) return 0;
    allocation = *wgpu_mip_find(surface);
    return allocation && allocation->device == pd && allocation->root == surface &&
        allocation->live == allocation->levels ? allocation->levels : 0;
}

static BOOL wgpu_mip_release(PDEV *pd, PDD_SURFACE_LOCAL surface)
{
    WgpuMipAllocation **entry, *allocation;
    BOOL owned = FALSE;
    if (!surface || !(surface->ddsCaps.dwCaps & DDSCAPS_MIPMAP) ||
        !surface->lpGbl || !surface->lpGbl->dwReserved1) return FALSE;
    EngAcquireSemaphore(d3d_lock);
    entry = wgpu_mip_find(surface->lpGbl);
    allocation = *entry;
    if (allocation && allocation->device == pd) {
        surface->lpGbl->dwReserved1 = 0;
        owned = TRUE;
        if (!--allocation->live) {
            *entry = allocation->next;
            EngFreeMem(allocation);
        }
    }
    EngReleaseSemaphore(d3d_lock);
    return owned;
}

/* Release bookkeeping when DirectDraw discards the device's surfaces. */
static void wgpu_mip_forget_device(PDEV *pd)
{
    WgpuMipAllocation **entry;
    if (!d3d_lock) return;
    EngAcquireSemaphore(d3d_lock);
    entry = &wgpu_mip_allocations;
    while (*entry) {
        WgpuMipAllocation *allocation = *entry;
        if (allocation->device == pd) {
            *entry = allocation->next;
            EngFreeMem(allocation);
        } else {
            entry = &allocation->next;
        }
    }
    EngReleaseSemaphore(d3d_lock);
}
