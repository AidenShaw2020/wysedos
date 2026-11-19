/* SPDX-License-Identifier: GPL-3.0-or-later
 * Preserve the pinned CPU display driver, extending its escape callback.
 * This exercises the same display->miniport path future rendering uses.
 */
#include "wgpu_display.h"
#define DrvEscape wgpu_base_escape
#define DrvEnableDriver wgpu_base_enable
#define DrvEnableDirectDraw wgpu_base_enable_dd
#ifdef WGPU_NT_GDI_PIPELINE
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
static BOOL wgpu_gdi_hook_ready;
static BOOL wgpu_associate_surface(HSURF surface, HDEV device, FLONG hooks);
#define EngAssociateSurface wgpu_associate_surface
#endif
#ifdef WGPU_NT_FAST_STREAM
#define memcpy wgpu_base_memcpy
#endif
#ifdef WGPU_NT_ENABLE_DEPTH
#define DrvGetDirectDrawInfo wgpu_base_get_dd_info
#endif
#include "winvbe.c"
#undef DrvEscape
#undef DrvEnableDriver
#undef DrvEnableDirectDraw
#undef DrvGetDirectDrawInfo
#ifdef WGPU_NT_GDI_PIPELINE
#undef EngAssociateSurface
static BOOL wgpu_associate_surface(HSURF surface, HDEV device, FLONG hooks)
{
    /* The pinned WinVBE uses zero hooks for the direct linear bitmap and
     * explicit drawing hooks for the banked/shadowed device surface. */
    if (!hooks && wgpu_gdi_hook_ready) hooks = HOOK_SYNCHRONIZE;
    return EngAssociateSurface(surface, device, hooks);
}
#endif
#ifdef WGPU_NT_FAST_STREAM
#undef memcpy
void *memcpy(void *destination, const void *source, size_t bytes)
{
    void *result = destination;
    if (bytes >= 32) {
        __asm__ __volatile__("cld; rep movsb"
            : "+D"(destination), "+S"(source), "+c"(bytes) : : "memory", "cc");
    } else {
        wgpu_base_memcpy(destination, source, bytes);
    }
    return result;
}
#endif

#ifdef WGPU_COPY_DIAGNOSTICS
static WgpuCopyTrace copy_trace;
#define COPY_TRACE(member, value) (copy_trace.member = (value))
#else
#define COPY_TRACE(member, value) ((void)0)
#endif

/* Synchronous fills and disjoint surface copies share the cache handling. */
static int wgpu_probe_flush_support(void)
{
    ULONG a,b,c,d;
    __asm__ __volatile__("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1));
    return (d & ((1ul<<19)|(1ul<<26)))==((1ul<<19)|(1ul<<26)) && ((b>>8)&255)==8;
}
#ifdef WGPU_NT_CACHE_CPU_FEATURES
static int wgpu_cached_flush_support;
static int wgpu_flush_supported(void)
{
    return wgpu_cached_flush_support;
}
#else
#define wgpu_flush_supported wgpu_probe_flush_support
#endif

static void wgpu_flush_fill(PDEV *pd,const WgpuNtFill *q)
{
    ULONG y;
    __asm__ __volatile__("mfence":::"memory");
    for(y=q->top;y<q->bottom;++y){
        ULONG begin=(ULONG)pd->fb+q->offset+y*q->pitch+q->left*(q->bpp/8);
        ULONG end=(ULONG)pd->fb+q->offset+y*q->pitch+q->right*(q->bpp/8);
        ULONG at=begin & ~63ul;
        for(;at<end;at+=64) __asm__ __volatile__("clflush (%0)"::"r"(at):"memory");
    }
    __asm__ __volatile__("mfence":::"memory");
}
static void wgpu_surface_rect(PDEV *pd, const DSURF *surface, const RECTL *rect,
                              WgpuNtFill *request)
{
    memset(request,0,sizeof(*request));
    request->size=sizeof(*request);
    request->abi_version=WGPU_NT_ABI_VERSION;
    request->offset=(ULONG)surface->base-(ULONG)pd->fb;
    request->pitch=surface->pitch;
    request->width=surface->w;
    request->height=surface->h;
    request->bpp=pd->bpp;
    request->left=rect->left;
    request->top=rect->top;
    request->right=rect->right;
    request->bottom=rect->bottom;
}

static int wgpu_surface_fits(PDEV *pd, const WgpuNtFill *request)
{
    return wgpu_nt_fill_valid(request,pd->vram,0x1000ul) &&
        (unsigned long long)(ULONG)pd->fb+request->offset+
        (unsigned long long)request->pitch*request->height<=0xffffffffull;
}

#ifdef WGPU_D3D_HAL
#include "d3d_hal.inc"

#ifdef WGPU_NT_SCENE_GROUPS
#ifdef WGPU_NT_GDI_PIPELINE
static VOID APIENTRY wgpu_scene_gdi_sync(DHPDEV device, RECTL *rect)
{
    (void)device;
    (void)rect;
#ifdef WGPU_NT_PROFILE_TARGETS
    __asm__ __volatile__("lock incl %0" : "+m"(d3d_target_stats.gdi_syncs) :: "cc", "memory");
#endif
    wgpu_d3d_drain();
}
#endif
static DWORD APIENTRY wgpu_scene_lock(PDD_LOCKDATA data)
{
    data->ddRVal = wgpu_d3d_drain();
    if (data->ddRVal != DD_OK) return DDHAL_DRIVER_HANDLED;
    return wgpu_depth_lock(data);
}

static DWORD APIENTRY wgpu_scene_unlock(PDD_UNLOCKDATA data)
{
    /* Unlock must release the runtime lock even if the GPU failed. */
    HRESULT result = wgpu_d3d_drain();
    DWORD handled = wgpu_depth_unlock(data);
    if (result == DD_OK) return handled;
    data->ddRVal = result;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY wgpu_scene_destroy_surface(PDD_DESTROYSURFACEDATA data)
{
#ifdef WGPU_NT_PIPELINE
    data->ddRVal = wgpu_d3d_drain();
    if (data->ddRVal != DD_OK) return DDHAL_DRIVER_HANDLED;
#else
    wgpu_d3d_drain();
#endif
    DWORD handled = DdDestroySurface(data);
#ifdef WGPU_NT_ENABLE_MIPS
    if (wgpu_mip_release(DD_PDEV(data->lpDD), data->lpDDSurface)) {
        data->ddRVal = DD_OK;
        return DDHAL_DRIVER_HANDLED;
    }
#endif
    return handled;
}

static DWORD APIENTRY wgpu_scene_overlay(PDD_UPDATEOVERLAYDATA data)
{
    data->ddRVal = wgpu_d3d_drain();
    if (data->ddRVal != DD_OK) return DDHAL_DRIVER_HANDLED;
    return DdUpdateOverlay(data);
}

static DWORD APIENTRY wgpu_scene_map_memory(PDD_MAPMEMORYDATA data)
{
#ifdef WGPU_NT_PIPELINE
    HRESULT result = wgpu_d3d_drain();
    if (result != DD_OK && data->bMap) {
        data->ddRVal = result;
        return DDHAL_DRIVER_HANDLED;
    }
#else
    wgpu_d3d_drain();
#endif
    return DdMapMemory(data);
}

#ifdef WGPU_NT_PIPELINE
static VOID wgpu_scene_detach_pipeline(DHPDEV device)
{
    if (!d3d_lock) return;
    EngAcquireSemaphore(d3d_lock);
    /* The kernel owner still owns a timed-out fence. Never keep a PDEV
     * whose display mapping is about to be destroyed. */
    if (d3d_pipeline_device == (PDEV *)device) {
        d3d_pipeline_device = NULL;
#ifdef WGPU_NT_SCENE_PIPELINE
        d3d_pipeline_context = NULL;
#endif
    }
    EngReleaseSemaphore(d3d_lock);
}
#endif

static VOID APIENTRY wgpu_scene_disable_dd(DHPDEV device)
{
    wgpu_d3d_drain();
#ifdef WGPU_NT_ENABLE_MIPS
    wgpu_mip_forget_device((PDEV *)device);
#endif
    DrvDisableDirectDraw(device);
}

static VOID APIENTRY wgpu_scene_disable_surface(DHPDEV device)
{
    wgpu_d3d_drain();
#ifdef WGPU_NT_PIPELINE
    wgpu_scene_detach_pipeline(device);
#endif
    DrvDisableSurface(device);
}

static VOID APIENTRY wgpu_scene_disable_pdev(DHPDEV device)
{
    wgpu_d3d_drain();
#ifdef WGPU_NT_PIPELINE
    wgpu_scene_detach_pipeline(device);
#endif
    DrvDisablePDEV(device);
}

static BOOL APIENTRY wgpu_scene_assert_mode(DHPDEV device, BOOL enable)
{
#ifdef WGPU_NT_PIPELINE
    if (wgpu_d3d_drain() != DD_OK) return FALSE;
#else
    wgpu_d3d_drain();
#endif
    return DrvAssertMode(device, enable);
}
#endif

#ifdef WGPU_NT_ENABLE_DEPTH
static BOOL APIENTRY wgpu_get_dd_info(DHPDEV device, DD_HALINFO *info,
                                    DWORD *heap_count, VIDEOMEMORY *heaps,
                                    DWORD *fourcc_count, DWORD *fourccs)
{
    PDEV *pd = (PDEV *)device;
    ULONG first, end, rows;
    if (!wgpu_base_get_dd_info(device, info, heap_count, heaps, fourcc_count, fourccs))
        return FALSE;
    if (*heap_count != 1 || !wgpu_depth_heap(pd, &first, &end)) return TRUE;

    rows = (first - pd->h * pd->stride) / pd->stride;
#ifdef WGPU_NT_ENABLE_MIPS
    if (pd->stride > 0)
        rows = (wgpu_mip_pool_first(pd, first) - pd->h * pd->stride) / pd->stride;
#endif
    if (heaps) {
        VIDEOMEMORY *depth = &heaps[1];
        heaps[0].dwHeight = rows;
        heaps[0].ddsCaps.dwCaps |= DDSCAPS_ZBUFFER;
        heaps[0].ddsCapsAlt.dwCaps |= DDSCAPS_ZBUFFER;
        memset(depth, 0, sizeof(*depth));
        depth->dwFlags = VIDMEM_ISLINEAR;
        depth->fpStart = first;
        depth->fpEnd = end - 1;
        depth->ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_OFFSCREENPLAIN |
            DDSCAPS_TEXTURE | DDSCAPS_OVERLAY | DDSCAPS_FLIP | DDSCAPS_ALPHA |
            DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER | DDSCAPS_FRONTBUFFER;
        depth->ddsCapsAlt = depth->ddsCaps;
    }
    *heap_count = 2;
    info->ddCaps.dwVidMemFree = rows * pd->stride + end - first;
    return TRUE;
}
#endif

#ifdef WGPU_NT_ENABLE_MATERIAL
#ifdef WGPU_D3D_DIAGNOSTICS
static DWORD APIENTRY wgpu_dd_can_create_surface(PDD_CANCREATESURFACEDATA create)
{
    DDSURFACEDESC *desc = create->lpDDSurfaceDesc;
    DWORD result = DdCanCreateSurface(create);
    if (d3d_lock && desc && (desc->ddsCaps.dwCaps & DDSCAPS_TEXTURE)) {
        EngAcquireSemaphore(d3d_lock);
        ++d3d_trace.texture_queries;
        if (create->ddRVal != DD_OK) {
            ++d3d_trace.texture_denials;
            d3d_trace.texture_query_caps = desc->ddsCaps.dwCaps;
            d3d_trace.texture_query_flags = desc->ddpfPixelFormat.dwFlags;
            d3d_trace.texture_query_bits = desc->ddpfPixelFormat.dwRGBBitCount;
            d3d_trace.texture_query_width = desc->dwWidth;
            d3d_trace.texture_query_height = desc->dwHeight;
            d3d_trace.texture_query_r = desc->ddpfPixelFormat.dwRBitMask;
            d3d_trace.texture_query_g = desc->ddpfPixelFormat.dwGBitMask;
            d3d_trace.texture_query_b = desc->ddpfPixelFormat.dwBBitMask;
            d3d_trace.texture_query_a = desc->ddpfPixelFormat.dwRGBAlphaBitMask;
        }
        EngReleaseSemaphore(d3d_lock);
    }
    return result;
}
#endif
static DWORD APIENTRY wgpu_dd_create_surface(PDD_CREATESURFACEDATA create)
{
    PDEV *pd = DD_PDEV(create->lpDD);
    DWORD i;
    const DDSURFACEDESC *desc = create->lpDDSurfaceDesc;
    DWORD handled;
    if ((desc->ddsCaps.dwCaps & (DDSCAPS_TEXTURE | DDSCAPS_MIPMAP)) ==
        (DDSCAPS_TEXTURE | DDSCAPS_MIPMAP) &&
        (create->dwSCnt != 1 || !wgpu_d3d_single_texture_level(desc))) {
#ifdef WGPU_NT_ENABLE_MIPS
        D3D_TRACE(texture_creates, d3d_trace.texture_creates + 1);
        wgpu_mip_create(pd, create);
#else
        create->ddRVal = DDERR_UNSUPPORTED;
#endif
        return DDHAL_DRIVER_HANDLED;
    }
    handled = DdCreateSurface(create);
    if (handled == DDHAL_DRIVER_HANDLED) return handled;
    for (i = 0; i < create->dwSCnt; ++i) {
        PDD_SURFACE_LOCAL local = create->lplpSList[i];
        PDD_SURFACE_GLOBAL surface = local->lpGbl;
        DWORD caps = local->ddsCaps.dwCaps;
        ULONG bpp;
#ifdef WGPU_NT_ENABLE_DEPTH
        if ((caps & DDSCAPS_ZBUFFER) && !(caps & DDSCAPS_SYSTEMMEMORY)) {
            if (wgpu_depth_create(pd, local, create->lpDDSurfaceDesc)) continue;
            create->ddRVal = DDERR_UNSUPPORTED;
            return DDHAL_DRIVER_HANDLED;
        }
#endif
        if (!(caps & DDSCAPS_TEXTURE) || (caps & DDSCAPS_SYSTEMMEMORY)) continue;
        D3D_TRACE(texture_creates, d3d_trace.texture_creates + 1);
        bpp = local->dwFlags & DDRAWISURF_HASPIXELFORMAT ?
            surface->ddpfSurface.dwRGBBitCount : pd->bpp;
        if (pd->stride <= 0 || (pd->stride & 63) ||
            (bpp != 16 && bpp != 32) || surface->wWidth * (bpp / 8) > (ULONG)pd->stride ||
            surface->wWidth > 2048 || surface->wHeight > 2048) {
            create->ddRVal = DDERR_UNSUPPORTED;
            return DDHAL_DRIVER_HANDLED;
        }
        /* The heap uses the display stride. Reserve whole cache lines for
         * each texture row without consuming the rest of the scanline. */
        surface->lPitch = pd->stride;
        surface->dwBlockSizeX = (surface->wWidth * (bpp / 8) + 63) & ~63ul;
        surface->dwBlockSizeY = surface->wHeight;
        surface->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
        D3D_TRACE(texture_pitch, surface->lPitch);
        D3D_TRACE(texture_block, surface->dwBlockSizeX);
        create->lpDDSurfaceDesc->lPitch = pd->stride;
        create->lpDDSurfaceDesc->dwFlags |= DDSD_PITCH;
    }
    return DDHAL_DRIVER_NOTHANDLED;
}
#endif

static volatile ULONG wgpu_present_count;

#ifdef WGPU_NT_PROFILE_BARRIERS
static DWORD APIENTRY wgpu_dd_vblank(PDD_WAITFORVERTICALBLANKDATA wait)
{
    const BOOL blocking = wait->dwFlags == DDWAITVB_BLOCKBEGIN ||
        wait->dwFlags == DDWAITVB_BLOCKEND;
    unsigned long long start = blocking ? wgpu_d3d_ticks() : 0;
    DWORD handled = DdWaitForVerticalBlank(wait);
    if (blocking && d3d_lock) {
        const unsigned long long elapsed = wgpu_d3d_ticks() - start;
        EngAcquireSemaphore(d3d_lock);
        ++d3d_barriers.vblank_waits;
        d3d_barriers.vblank_ticks += elapsed;
        EngReleaseSemaphore(d3d_lock);
    }
    return handled;
}
#endif

static DWORD APIENTRY wgpu_dd_flip(PDD_FLIPDATA flip)
{
    PDEV *pd = DD_PDEV(flip->lpDD);
    ULONG previous = pd->start;
    DWORD handled;
#ifdef WGPU_NT_PROFILE_BARRIERS
    unsigned long long start;
#endif
#ifdef WGPU_NT_SCENE_GROUPS
    flip->ddRVal = wgpu_d3d_drain();
    if (flip->ddRVal != DD_OK) return DDHAL_DRIVER_HANDLED;
#endif
#ifdef WGPU_NT_PROFILE_BARRIERS
    start = wgpu_d3d_ticks();
#endif
    handled = DdFlip(flip);
#ifdef WGPU_NT_PROFILE_BARRIERS
    if (d3d_lock) {
        const unsigned long long elapsed = wgpu_d3d_ticks() - start;
        EngAcquireSemaphore(d3d_lock);
        ++d3d_barriers.flips;
        d3d_barriers.flip_ticks += elapsed;
        EngReleaseSemaphore(d3d_lock);
    }
#endif
    /* The callback changes the start address only after VBE accepts a
     * non-overlay flip. Count the distinct completed presentations. */
    if (handled == DDHAL_DRIVER_HANDLED && flip->ddRVal == DD_OK &&
        previous != pd->start)
        __asm__ __volatile__("lock incl %0" : "+m"(wgpu_present_count) :: "cc", "memory");
    return handled;
}
#endif

static DWORD APIENTRY wgpu_dd_blt(PDD_BLTDATA blt)
{
    PDEV *pd=DD_PDEV(blt->lpDD);
    DSURF surface;
    RECTL rect=blt->rDest,whole;
    WgpuNtCopy request;
    const void *input=&request.dst;
    ULONG input_bytes=sizeof(request.dst), code=WGPU_DISPLAY_FILL_IOCTL;
    struct {WgpuDisplayReply header;WgpuNtFillResult result;} reply;
    ULONG returned=0,error;
    DWORD flags=blt->dwFlags & ~DDBLT_WAIT;
    DWORD rop=blt->bltFX.dwROP;
    int fill=flags==DDBLT_COLORFILL;
#ifdef WGPU_NT_SCENE_GROUPS
    blt->ddRVal = wgpu_d3d_drain();
    if (blt->ddRVal != DD_OK) return DDHAL_DRIVER_HANDLED;
#endif
#ifdef WGPU_NT_ENABLE_DEPTH
    if (wgpu_depth_owned(blt->lpDDDestSurface) || wgpu_depth_owned(blt->lpDDSrcSurface))
        return wgpu_depth_blt(blt);
#endif
    if (!fill) {
        COPY_TRACE(stage,1);
        COPY_TRACE(flags,blt->dwFlags);
        COPY_TRACE(rop,blt->bltFX.dwROP);
        COPY_TRACE(clipped,blt->IsClipped);
    }
    /* Match WINVBE's CPU callback: the ROP3 index can arrive on its own
     * or in bits 16..23 (XP sends 0x00cc0000 for a plain Blt). */
    if ((!fill && flags && (flags!=DDBLT_ROP ||
         (rop>0xff ? (rop>>16)&0xff : rop)!=0xcc)) || blt->IsClipped ||
       pd->banked || pd->shadowed || (pd->bpp!=16 && pd->bpp!=32) ||
       pd->rmask!=(pd->bpp==16?0xf800ul:0xff0000ul) ||
       pd->gmask!=(pd->bpp==16?0x07e0ul:0x00ff00ul) ||
       pd->bmask!=(pd->bpp==16?0x001ful:0x0000fful) ||
       !dsurf(pd,blt->lpDDDestSurface,&surface) || !surface.screen ||
       surface.fourcc || surface.Bpp!=pd->bpp/8 || surface.pitch<=0 ||
       !wgpu_flush_supported()) return DdBlt(blt);
    whole.left=whole.top=0;whole.right=surface.w;whole.bottom=surface.h;
    if (fill && !rect_and(&rect,&whole)) {
        blt->ddRVal=DD_OK;
        return DDHAL_DRIVER_HANDLED;
    }
    wgpu_surface_rect(pd,&surface,&rect,&request.dst);
    if (fill) {
        request.dst.color=blt->bltFX.dwFillColor;
        if (request.dst.bpp==16) request.dst.color&=0xffff;
    } else {
        DSURF src;
        COPY_TRACE(stage,2);
        if (!blt->lpDDSrcSurface || !dsurf(pd,blt->lpDDSrcSurface,&src) ||
            !src.screen || src.fourcc || src.Bpp!=surface.Bpp || src.pitch<=0)
            return DdBlt(blt);
        wgpu_surface_rect(pd,&src,&blt->rSrc,&request.src);
        COPY_TRACE(stage,3);
        COPY_TRACE(src_offset,request.src.offset);
        COPY_TRACE(src_pitch,request.src.pitch);
        COPY_TRACE(dst_offset,request.dst.offset);
        COPY_TRACE(dst_pitch,request.dst.pitch);
        if (!wgpu_surface_fits(pd,&request.src) ||
            !wgpu_nt_copy_valid(&request,pd->vram,0x1000ul)) return DdBlt(blt);
        input=&request;
        input_bytes=sizeof(request);
        code=WGPU_DISPLAY_COPY_IOCTL;
    }
    if (!wgpu_surface_fits(pd,&request.dst)) return DdBlt(blt);
    if (!fill) COPY_TRACE(stage,4);
    memset(&reply,0,sizeof(reply));
    if (!fill) wgpu_flush_fill(pd,&request.src);
    wgpu_flush_fill(pd,&request.dst);
    error=ioctl(pd->hDriver,code,(PVOID)input,input_bytes,
                 &reply,sizeof(reply),&returned);
    /* Flush again after completion so an old CPU cache line cannot obscure
     * GPU writes, including when applications immediately Lock the surface. */
    wgpu_flush_fill(pd,&request.dst);
    if(error || returned!=sizeof(reply) || reply.header.size!=sizeof(reply) ||
       reply.header.abi_version!=WGPU_DISPLAY_ABI || reply.header.status ||
       reply.header.bytes!=sizeof(reply.result) || reply.result.size!=sizeof(reply.result) ||
       reply.result.abi_version!=WGPU_NT_ABI_VERSION || !reply.result.fence || reply.result.reserved){
        /* A failed submission may still be in flight: do not race it with a
         * CPU fallback and do not claim a successful accelerated operation. */
        blt->ddRVal=DDERR_GENERIC;return DDHAL_DRIVER_HANDLED;
    }
    blt->ddRVal=DD_OK;return DDHAL_DRIVER_HANDLED;
}
BOOL APIENTRY DrvEnableDirectDraw(DHPDEV device,DD_CALLBACKS *cb,
    DD_SURFACECALLBACKS *scb,DD_PALETTECALLBACKS *pcb)
{
    BOOL ok=wgpu_base_enable_dd(device,cb,scb,pcb);
    if (ok) {
        scb->Blt = wgpu_dd_blt;
#if defined(WGPU_D3D_HAL) && defined(WGPU_NT_ENABLE_MATERIAL)
        cb->CreateSurface = wgpu_dd_create_surface;
#ifdef WGPU_D3D_DIAGNOSTICS
        cb->CanCreateSurface = wgpu_dd_can_create_surface;
#endif
#endif
#ifdef WGPU_NT_SCENE_GROUPS
        scb->Lock = wgpu_scene_lock;
        scb->Unlock = wgpu_scene_unlock;
        scb->DestroySurface = wgpu_scene_destroy_surface;
        scb->UpdateOverlay = wgpu_scene_overlay;
        if (cb->MapMemory == DdMapMemory) cb->MapMemory = wgpu_scene_map_memory;
#elif defined(WGPU_NT_ENABLE_DEPTH)
        scb->Lock = wgpu_depth_lock;
        scb->Unlock = wgpu_depth_unlock;
#endif
#ifdef WGPU_D3D_HAL
        if (scb->Flip == DdFlip) scb->Flip = wgpu_dd_flip;
#endif
#ifdef WGPU_NT_PROFILE_BARRIERS
        if (cb->WaitForVerticalBlank == DdWaitForVerticalBlank)
            cb->WaitForVerticalBlank = wgpu_dd_vblank;
#endif
    }
    return ok;
}

static ULONG APIENTRY wgpu_escape(SURFOBJ *surface, ULONG escape, ULONG in_bytes,
                                  PVOID input, ULONG out_bytes, PVOID output)
{
    WgpuDisplayRequest request;
    ULONG returned;
    PDEV *device;
#ifdef WGPU_NT_OPENGL_ICD
    enum { OpenGlGetInfo = 0x1101 };
    static const struct {
        ULONG interface_version, driver_version;
        WCHAR name[7];
    } icd = {2, 1, {'W', 'G', 'G', 'L', 'N', 'T', 0}};
    ULONG query;

    if (escape == QUERYESCSUPPORT && input && in_bytes == sizeof query) {
        memcpy(&query, input, sizeof query);
        if (query == OpenGlGetInfo) return 1;
    }
    if (escape == OpenGlGetInfo) {
        if (!output || out_bytes < sizeof icd) return 0;
        memcpy(output, &icd, sizeof icd);
        return 1;
    }
#endif
#ifdef WGPU_NT_PROFILE_TRANSPORT
    if (escape == WGPU_DISPLAY_TRANSPORT_TIMING) {
        if (!surface || !surface->dhpdev || in_bytes || !output ||
            out_bytes != sizeof(WgpuDisplayTransportTiming)) return 0;
        device = (PDEV *)surface->dhpdev;
        return !ioctl(device->hDriver, WGPU_DISPLAY_TRANSPORT_TIMING_IOCTL,
                      NULL, 0, output, out_bytes, &returned) && returned == out_bytes;
    }
#endif
#ifdef WGPU_D3D_HAL
    if (escape == WGPU_DISPLAY_PRESENT_COUNT) {
        ULONG count = wgpu_present_count;
        if (in_bytes || !output || out_bytes != sizeof(count)) return 0;
        memcpy(output, &count, sizeof(count));
        return 1;
    }
#endif
#ifdef WGPU_D3D_TIMING
    if (escape == WGPU_DISPLAY_D3D_TIMING) {
        if (in_bytes || !output || (out_bytes != sizeof(WgpuD3dTiming) &&
            out_bytes != offsetof(WgpuD3dTiming, scene_group_holds) &&
            out_bytes != offsetof(WgpuD3dTiming, material_groups))) return 0;
        return wgpu_d3d_read_timing(output, out_bytes);
    }
#endif
#ifdef WGPU_NT_PROFILE_TARGETS
    if (escape == WGPU_DISPLAY_D3D_TARGETS) {
        if (in_bytes || !output || (out_bytes != sizeof(WgpuD3dTargetStats) &&
            out_bytes != offsetof(WgpuD3dTargetStats, deferred_gdi_primary))) return 0;
        return wgpu_d3d_read_targets(output, out_bytes);
    }
#endif
#ifdef WGPU_NT_PROFILE_BARRIERS
    if (escape == WGPU_DISPLAY_D3D_BARRIERS) {
        if (in_bytes || !output || (out_bytes != sizeof(WgpuD3dBarriers) &&
            out_bytes != offsetof(WgpuD3dBarriers, flips))) return 0;
        return wgpu_d3d_read_barriers(output, out_bytes);
    }
#endif
#ifdef WGPU_D3D_DIAGNOSTICS
    if (escape == WGPU_DISPLAY_D3D_TRACE) {
        if (in_bytes || !output || (out_bytes != sizeof(WgpuD3dTrace) &&
            out_bytes != offsetof(WgpuD3dTrace, init_state_count) &&
            out_bytes != offsetof(WgpuD3dTrace, unindexed_first) &&
            out_bytes != offsetof(WgpuD3dTrace, rejected_state) &&
            out_bytes != offsetof(WgpuD3dTrace, rejected_phase) &&
            out_bytes != offsetof(WgpuD3dTrace, texture_creates) &&
            out_bytes != offsetof(WgpuD3dTrace, clear_commands))) return 0;
        return wgpu_d3d_trace(output, out_bytes);
    }
#endif
#ifdef WGPU_COPY_DIAGNOSTICS
    if (escape == WGPU_DISPLAY_COPY_TRACE) {
        if (in_bytes || !output || out_bytes!=sizeof(copy_trace)) return 0;
        memcpy(output,&copy_trace,sizeof(copy_trace));
        return 1;
    }
#endif
    if (escape == QUERYESCSUPPORT && input && in_bytes == sizeof(ULONG) &&
        *(ULONG *)input == WGPU_DISPLAY_ESCAPE) return 1;
    if (escape != WGPU_DISPLAY_ESCAPE)
        return wgpu_base_escape(surface, escape, in_bytes, input, out_bytes, output);
    if (!surface || !surface->dhpdev || !input || !output || in_bytes != sizeof(request))
        return 0;
    memcpy(&request, input, sizeof(request));
    if (!wgpu_display_request_valid(&request, in_bytes, out_bytes)) return 0;
    device = (PDEV *)surface->dhpdev;
    return !ioctl(device->hDriver, WGPU_DISPLAY_IOCTL, &request, sizeof(request),
                   output, out_bytes, &returned) && returned == out_bytes;
}

/* Wrap upstream callbacks before DrvEnableDriver copies the table. */
BOOL APIENTRY DrvEnableDriver(ULONG engine, ULONG bytes,
                                                   DRVENABLEDATA *data)
{
    ULONG i;
    BOOL enabled;
#ifdef WGPU_NT_CACHE_CPU_FEATURES
    /* CPU features are fixed for the lifetime of the loaded display driver. */
    wgpu_cached_flush_support = wgpu_probe_flush_support();
#endif
    for (i = 0; i < sizeof(fns) / sizeof(fns[0]); ++i)
        if (fns[i].iFunc == INDEX_DrvEscape) fns[i].pfn = (PFN)wgpu_escape;
        else if (fns[i].iFunc == INDEX_DrvEnableDirectDraw) fns[i].pfn = (PFN)DrvEnableDirectDraw;
#ifdef WGPU_NT_ENABLE_DEPTH
        else if (fns[i].iFunc == INDEX_DrvGetDirectDrawInfo) fns[i].pfn = (PFN)wgpu_get_dd_info;
#endif
#ifdef WGPU_NT_SCENE_GROUPS
        else if (fns[i].iFunc == INDEX_DrvDisableDirectDraw) fns[i].pfn = (PFN)wgpu_scene_disable_dd;
        else if (fns[i].iFunc == INDEX_DrvDisableSurface) fns[i].pfn = (PFN)wgpu_scene_disable_surface;
        else if (fns[i].iFunc == INDEX_DrvDisablePDEV) fns[i].pfn = (PFN)wgpu_scene_disable_pdev;
        else if (fns[i].iFunc == INDEX_DrvAssertMode) fns[i].pfn = (PFN)wgpu_scene_assert_mode;
#endif
    enabled = wgpu_base_enable(engine, bytes, data);
#ifdef WGPU_NT_GDI_PIPELINE
    /* WinVBE reserves additional runtime entries for its pre-NT5 GL hooks. */
    wgpu_gdi_hook_ready = enabled && engine >= DDI_DRIVER_VERSION_NT5 && bytes >= sizeof(*data) &&
        data->pdrvfn == fns_run && data->c < sizeof(fns_run) / sizeof(fns_run[0]);
    if (wgpu_gdi_hook_ready) {
        data->pdrvfn[data->c].iFunc = INDEX_DrvSynchronize;
        data->pdrvfn[data->c].pfn = (PFN)wgpu_scene_gdi_sync;
        ++data->c;
    }
#endif
    return enabled;
}
