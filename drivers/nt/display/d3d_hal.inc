/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NT5 Direct3D 7 DDI. Included after the DirectDraw framebuffer helpers.
 * The opt-in build rasterizes pretransformed triangle lists.
 */
typedef DD_SURFACE_LOCAL *LPDDRAWI_DDRAWSURFACE_LCL;
typedef DD_DIRECTDRAW_LOCAL *LPDDRAWI_DIRECTDRAW_LCL;
typedef DD_DIRECTDRAW_GLOBAL *LPDDRAWI_DIRECTDRAW_GBL;
#include <stddef.h>
#include <d3dhal.h>

/* Legacy DDI constants absent from the MinGW header. */
#define D3DHAL3_CB32_DRAWPRIMITIVES2 0x00000008ul
#define D3DHAL3_CB32_VALIDATETEXTURESTAGESTATE 0x00000004ul
#define D3DHAL_OUTOFCONTEXTS ((HRESULT)0x8876006eL)
#define D3DERR_COMMAND_UNPARSED ((HRESULT)0x88760bb8L)
#define D3DRENDERSTATE_SCENECAPTURE 62ul
#define WGPU_D3D_CONTEXTS 16
#ifdef WGPU_NT_ENABLE_MATERIAL
#define WGPU_D3D_TARGETS 1024
#else
#define WGPU_D3D_TARGETS 128
#endif
#define WGPU_D3D_STREAM_LIMIT (256ul * 1024)
#define WGPU_D3D_USER_LIMIT 0x7fff0000ul
#define WGPU_D3D_RENDER_CAPS (DDSCAPS_3DDEVICE | DDSCAPS_PRIMARYSURFACE | \
                             DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER)

struct _D3DHAL_VALIDATETEXTURESTAGESTATEDATA {
    ULONG_PTR dwhContext;
    DWORD dwFlags;
    ULONG_PTR dwReserved;
    DWORD dwNumPasses;
    HRESULT ddrval;
};

typedef struct WgpuD3dContext {
    ULONG_PTR id;
    HANDLE process;
    PDD_DIRECTDRAW_LOCAL draw;
    PDD_SURFACE_LOCAL target;
#ifdef WGPU_NT_SCENE_GROUPS
    BOOL scene_open;
#endif
    DWORD shade, cull;
    DWORD textures[8];
#ifdef WGPU_NT_ENABLE_MATERIAL
    DWORD stage_state[25];
    DWORD legacy_blend;
    DWORD fragment_flags, blend_src, blend_dst, alpha_compare, alpha_ref;
    DWORD wrap_coordinates;
    BOOL perspective;
#endif
#ifdef WGPU_NT_ENABLE_DEPTH
    PDD_SURFACE_LOCAL depth;
    DWORD depth_compare;
    BOOL depth_enabled, depth_write;
#endif
    D3DHAL_DP2VIEWPORTINFO viewport;
} WgpuD3dContext;

typedef struct WgpuD3dTarget {
    PDD_DIRECTDRAW_LOCAL draw;
    PDD_SURFACE_LOCAL surface;
    PDD_SURFACE_MORE more;
    DWORD handle;
    BOOL flipping;
} WgpuD3dTarget;

static HSEMAPHORE d3d_lock;
static WgpuD3dContext d3d_contexts[WGPU_D3D_CONTEXTS];
static WgpuD3dTarget d3d_targets[WGPU_D3D_TARGETS];
static ULONG_PTR d3d_next_id;
static BOOL d3d_mode_supported;
#ifdef WGPU_D3D_DIAGNOSTICS
static WgpuD3dTrace d3d_trace;
#define D3D_TRACE(member, value) (d3d_trace.member = (value))
static ULONG wgpu_d3d_trace(void *output, ULONG bytes)
{
    if (!d3d_lock) return 0;
    EngAcquireSemaphore(d3d_lock);
    memcpy(output, &d3d_trace, bytes);
    EngReleaseSemaphore(d3d_lock);
    return 1;
}
#else
#define D3D_TRACE(member, value) ((void)0)
#endif
#ifdef WGPU_NT_PROFILE_BARRIERS
static WgpuD3dBarriers d3d_barriers;

static ULONG wgpu_d3d_read_barriers(void *output, ULONG bytes)
{
    if (!d3d_lock) return 0;
    EngAcquireSemaphore(d3d_lock);
    memcpy(output, &d3d_barriers, bytes);
    EngReleaseSemaphore(d3d_lock);
    return 1;
}
#endif
#ifdef WGPU_D3D_TIMING
static WgpuD3dTiming d3d_timing;

#ifdef WGPU_NT_PROFILE_TSC
#include "../wgpu_profile_clock.h"
#define wgpu_d3d_ticks wgpu_profile_ticks
#else
static LONGLONG wgpu_d3d_ticks(void)
{
    LONGLONG ticks;
    EngQueryPerformanceCounter(&ticks);
    return ticks;
}
#endif

static ULONG wgpu_d3d_read_timing(void *output, ULONG bytes)
{
    if (!d3d_lock) return 0;
    EngAcquireSemaphore(d3d_lock);
#ifndef WGPU_NT_PROFILE_TSC
    EngQueryPerformanceFrequency(&d3d_timing.frequency);
#endif
    memcpy(output, &d3d_timing, bytes);
    EngReleaseSemaphore(d3d_lock);
    return 1;
}
#define D3D_TIMER(start) LONGLONG start = wgpu_d3d_ticks()
#define D3D_ELAPSED(field, start) (d3d_timing.field += wgpu_d3d_ticks() - (start))
#define D3D_TIMING_COUNT(field) (++d3d_timing.field)
#define D3D_TIMING_ADD(field, value) (d3d_timing.field += (value))
#else
#define D3D_TIMER(start) ((void)0)
#define D3D_ELAPSED(field, start) ((void)0)
#define D3D_TIMING_COUNT(field) ((void)0)
#define D3D_TIMING_ADD(field, value) ((void)0)
#endif
#ifdef WGPU_NT_ENABLE_DEPTH
#include "d3d_depth.inc"
#endif
void (*d3dnt_log)(const char *, ULONG, ULONG);

static BOOL wgpu_d3d_format(ULONG bpp, ULONG r, ULONG g, ULONG b)
{
    return (bpp == 16 && r == 0xf800 && g == 0x07e0 && b == 0x1f) ||
           (bpp == 32 && r == 0xff0000 && g == 0xff00 && b == 0xff);
}

static BOOL wgpu_d3d_render_target(PDEV *pd, PDD_SURFACE_LOCAL local,
                                  WgpuNtFill *target)
{
    DSURF surface;
    RECTL rect;
    if (!pd || !pd->fb || pd->banked || pd->shadowed ||
        !wgpu_d3d_format(pd->bpp, pd->rmask, pd->gmask, pd->bmask) ||
        !local || !local->lpGbl || !(local->ddsCaps.dwCaps & WGPU_D3D_RENDER_CAPS) ||
        !dsurf(pd, local, &surface) || !surface.screen || surface.fourcc ||
        surface.pitch <= 0 || !wgpu_flush_supported()) return FALSE;
    rect.left = rect.top = 0;
    rect.right = surface.w;
    rect.bottom = surface.h;
    wgpu_surface_rect(pd, &surface, &rect, target);
    return wgpu_surface_fits(pd, target);
}

#ifdef WGPU_NT_PIPELINE
/* Protected by d3d_lock. Only a verified hidden target can be owned by the
 * GPU across two submits. The whole target is cache-clean until retirement. */
static PDEV *d3d_pipeline_device;
static WgpuNtFill d3d_pipeline_target;
#ifdef WGPU_NT_PIPELINE_DAMAGE
static WgpuNtFill d3d_pipeline_damage;
static BOOL d3d_pipeline_damage_valid;
#endif
#ifdef WGPU_NT_SCENE_PIPELINE
static const WgpuD3dContext *d3d_pipeline_context;
#endif

static BOOL wgpu_d3d_pipeline_target_matches(PDEV *pd, const WgpuNtFill *target)
{
    const WgpuNtFill *current = &d3d_pipeline_target;
    /* Both descriptors cover the full surface. */
    return d3d_pipeline_device == pd && target &&
        current->offset == target->offset && current->pitch == target->pitch &&
        current->width == target->width && current->height == target->height &&
        current->bpp == target->bpp;
}

static HRESULT wgpu_d3d_retire_pipeline(void)
{
    ULONG returned = 0;
    PDEV *pd = d3d_pipeline_device;
    if (!pd) return DD_OK;
    D3D_TIMER(transport_start);
    int error = ioctl(pd->hDriver, WGPU_DISPLAY_DRAIN_MATERIAL_IOCTL,
                      NULL, 0, NULL, 0, &returned);
    D3D_ELAPSED(transport_ticks, transport_start);
#ifdef WGPU_NT_PROFILE_BARRIERS
    d3d_barriers.queued_transport_ticks += wgpu_d3d_ticks() - transport_start;
#endif
    if (error || returned) return DDERR_GENERIC;
    D3D_TIMER(cache_start);
    wgpu_flush_fill(pd,
#ifdef WGPU_NT_PIPELINE_DAMAGE
        d3d_pipeline_damage_valid ? &d3d_pipeline_damage :
#endif
        &d3d_pipeline_target);
    D3D_ELAPSED(target_ticks, cache_start);
    d3d_pipeline_device = NULL;
#ifdef WGPU_NT_SCENE_PIPELINE
    d3d_pipeline_context = NULL;
#endif
    return DD_OK;
}
#endif

static HRESULT wgpu_d3d_submit(PDEV *pd, ULONG code, const WgpuNtFill *target,
                               void *input, ULONG bytes)
{
    struct { WgpuDisplayReply header; WgpuNtFillResult result; } reply;
    ULONG returned = 0;
    int error;
#ifdef WGPU_NT_PIPELINE
    const BOOL queue = code == WGPU_DISPLAY_QUEUE_MATERIAL_IOCTL;
    if (d3d_pipeline_device && (!queue || !wgpu_d3d_pipeline_target_matches(pd, target))) {
        HRESULT retired = wgpu_d3d_retire_pipeline();
        if (retired != DD_OK) return retired;
    }
    if (queue && !target) return DDERR_INVALIDPARAMS;
#endif
    D3D_TIMING_COUNT(submits);
    if (target
#ifdef WGPU_NT_PIPELINE
        && !d3d_pipeline_device
#endif
        ) {
        D3D_TIMER(start);
        wgpu_flush_fill(pd, target);
        D3D_ELAPSED(target_ticks, start);
    }
#ifdef WGPU_NT_PIPELINE
    if (queue) {
#ifdef WGPU_NT_PIPELINE_DAMAGE
        if (!d3d_pipeline_device) d3d_pipeline_damage_valid = FALSE;
#endif
        d3d_pipeline_device = pd;
        d3d_pipeline_target = *target;
    }
#endif
    memset(&reply, 0, sizeof(reply));
    D3D_TIMER(transport_start);
    error = ioctl(pd->hDriver, code, input, bytes, &reply, sizeof(reply), &returned);
    D3D_ELAPSED(transport_ticks, transport_start);
#ifdef WGPU_NT_PROFILE_BARRIERS
    if (queue) d3d_barriers.queued_transport_ticks += wgpu_d3d_ticks() - transport_start;
#endif
    if (target
#ifdef WGPU_NT_PIPELINE
        && !queue
#endif
        ) {
        D3D_TIMER(start);
        wgpu_flush_fill(pd, target);
        D3D_ELAPSED(target_ticks, start);
    }
    if (error || returned != sizeof(reply) || reply.header.size != sizeof(reply) ||
        reply.header.abi_version != WGPU_DISPLAY_ABI || reply.header.status ||
        reply.header.bytes != sizeof(reply.result) || reply.result.size != sizeof(reply.result) ||
        reply.result.abi_version != WGPU_NT_ABI_VERSION || !reply.result.fence ||
        reply.result.reserved) return DDERR_GENERIC;
    return DD_OK;
}

#ifdef WGPU_NT_ENABLE_MATERIAL
static void wgpu_d3d_flush_material_texture(PDEV *pd, const WgpuNtFill *texture)
{
    WgpuNtFill range;
    if (!texture->reserved) {
        wgpu_flush_fill(pd, texture);
    } else if (wgpu_nt_material_texture_range(texture, pd->vram, 0x1000ul, &range)) {
        wgpu_flush_fill(pd, &range);
    }
}
#endif

#ifdef WGPU_NT_ENABLE_GROUPS
/* Protected by d3d_lock. A group has one render target. The window bounds
 * video-port allocations even for long draws. */
static struct {
    WgpuNtMaterialGroup packet;
    DWORD entries[32768 / sizeof(DWORD)];
} d3d_group;
static WgpuNtFill d3d_group_damage;
static BOOL d3d_group_has_damage;
static DWORD d3d_group_command, d3d_group_first, d3d_group_last;
static DWORD *d3d_group_error_offset;
#ifdef WGPU_NT_CLEAN_TEXTURES
/* The NT texture heap allocates rectangles with the display stride. A
 * cache entry represents the rows that were flushed, without their gaps. */
typedef struct WgpuD3dCleanTexture {
    ULONG offset, pitch, row_bytes, height;
} WgpuD3dCleanTexture;
static WgpuD3dCleanTexture d3d_clean_textures[32];
static ULONG d3d_clean_textures_valid;
static PDEV *d3d_clean_textures_device;
#endif
#ifdef WGPU_NT_SCENE_GROUPS
static PDEV *d3d_group_device;
static const WgpuD3dContext *d3d_group_context;
#endif

static void wgpu_d3d_merge_damage(WgpuNtFill *combined, const WgpuNtFill *damage)
{
    if (damage->left < combined->left) combined->left = damage->left;
    if (damage->top < combined->top) combined->top = damage->top;
    if (damage->right > combined->right) combined->right = damage->right;
    if (damage->bottom > combined->bottom) combined->bottom = damage->bottom;
}

#ifdef WGPU_NT_LAZY_GROUP_DAMAGE
#ifdef WGPU_NT_PIPELINE_DAMAGE
#error Queued damage requires the eager group bounds
#endif
static const WgpuNtFill *wgpu_d3d_draw_damage(WgpuNtFill *, const WgpuNtFill *,
    const void *, DWORD, DWORD);

static const WgpuNtFill *wgpu_d3d_sync_group_damage(WgpuNtFill *combined)
{
    const BYTE *entry = (const BYTE *)d3d_group.entries;
    BOOL has_damage = FALSE;
    for (DWORD i = 0; i < d3d_group.packet.count; ++i) {
        const WgpuNtMaterialDraw *draw = (const void *)entry;
        WgpuNtFill damage;
        if (wgpu_d3d_draw_damage(&damage, &draw->target, draw->vertices,
                                 draw->vertex_count, sizeof(draw->vertices[0]))) {
            if (!has_damage) *combined = damage;
            else wgpu_d3d_merge_damage(combined, &damage);
            has_damage = TRUE;
        }
        entry += wgpu_nt_material_bytes(draw->vertex_count);
    }
    return has_damage ? combined : NULL;
}
#endif

static HRESULT wgpu_d3d_group_send(PDEV *pd, BOOL queue)
{
    HRESULT result;
#ifdef WGPU_NT_LAZY_GROUP_DAMAGE
    WgpuNtFill group_damage;
    const WgpuNtFill *damage;
#else
    const WgpuNtFill *damage = d3d_group_has_damage ? &d3d_group_damage : NULL;
#endif
    if (!d3d_group.packet.count) return DD_OK;
    D3D_TIMING_COUNT(material_groups);
    D3D_TIMING_ADD(material_records, d3d_group.packet.count);
    D3D_TIMING_ADD(chained_groups, d3d_group.packet.count > 1);
#ifdef WGPU_NT_PIPELINE
    if (queue) {
        const WgpuNtMaterialDraw *first = (const void *)d3d_group.entries;
        WgpuNtFill surface = first->target;
        /* Pipeline ownership covers the whole allocation. The scissor in
         * the first packet may cover only a viewport such as a mirror. */
        surface.left = surface.top = 0;
        surface.right = surface.width;
        surface.bottom = surface.height;
        result = wgpu_d3d_submit(pd, WGPU_DISPLAY_QUEUE_MATERIAL_IOCTL, &surface,
                                 &d3d_group, d3d_group.packet.size);
#ifdef WGPU_NT_PIPELINE_DAMAGE
        /* Keep the original full-surface preflush and identity. Only the
         * invalidation after successful GPU retirement uses raster bounds. */
        if (d3d_pipeline_device == pd) {
            if (result != DD_OK || !damage) {
                d3d_pipeline_damage = d3d_pipeline_target;
            } else if (!d3d_pipeline_damage_valid) {
                d3d_pipeline_damage = *damage;
            } else {
                wgpu_d3d_merge_damage(&d3d_pipeline_damage, damage);
            }
            d3d_pipeline_damage_valid = TRUE;
        }
#endif
    } else {
#else
    (void)queue;
#endif
#ifdef WGPU_NT_LAZY_GROUP_DAMAGE
        /* Async groups still use the full-target preflush and postflush. */
        damage = wgpu_d3d_sync_group_damage(&group_damage);
#endif
        if (d3d_group.packet.count == 1)
            result = wgpu_d3d_submit(pd, WGPU_DISPLAY_MATERIAL_IOCTL, damage,
                d3d_group.entries, d3d_group.packet.size - sizeof(d3d_group.packet));
        else
            result = wgpu_d3d_submit(pd, WGPU_DISPLAY_MATERIAL_GROUP_IOCTL, damage,
                &d3d_group, d3d_group.packet.size);
#ifdef WGPU_NT_PIPELINE
    }
#endif
    d3d_group.packet.count = 0;
    d3d_group.packet.size = sizeof(d3d_group.packet);
    d3d_group_has_damage = FALSE;
    if (result != DD_OK && d3d_group_error_offset)
        *d3d_group_error_offset = d3d_group_first;
#ifdef WGPU_NT_SCENE_GROUPS
    d3d_group_device = NULL;
    d3d_group_context = NULL;
#endif
    return result;
}

static HRESULT wgpu_d3d_group_flush(PDEV *pd)
{
#ifdef WGPU_NT_CLEAN_TEXTURES
    /* CPU and surface lifecycle barriers enter here or through the drain. */
    d3d_clean_textures_valid = 0;
#endif
#ifdef WGPU_NT_PIPELINE
    HRESULT result = wgpu_d3d_group_send(pd, d3d_pipeline_device == pd);
    HRESULT retired = wgpu_d3d_retire_pipeline();
    return result == DD_OK ? retired : result;
#else
    return wgpu_d3d_group_send(pd, FALSE);
#endif
}

#ifdef WGPU_NT_SCENE_GROUPS
#ifdef WGPU_NT_PROFILE_TARGETS
static WgpuD3dTargetStats d3d_target_stats;

static ULONG wgpu_d3d_read_targets(void *output, ULONG bytes)
{
    if (!d3d_lock) return 0;
    EngAcquireSemaphore(d3d_lock);
    memcpy(output, &d3d_target_stats, bytes);
    EngReleaseSemaphore(d3d_lock);
    return 1;
}

static void wgpu_d3d_profile_target(const PDEV *pd, const WgpuD3dContext *context,
                                     const WgpuNtFill *target)
{
    const DWORD caps = context->target->ddsCaps.dwCaps;
    const ULONGLONG primary_bytes = (ULONGLONG)pd->stride * pd->h;
    const ULONGLONG end = (ULONGLONG)target->offset +
        (ULONGLONG)target->pitch * target->height;
    if (target->offset < pd->start + primary_bytes && end > pd->start)
        ++d3d_target_stats.scanout;
    else if (target->offset < primary_bytes)
        ++d3d_target_stats.gdi_primary;
    else if (caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_OVERLAY))
        ++d3d_target_stats.flagged;
    else {
        ++d3d_target_stats.missing_backbuffer;
        if ((caps & (DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY)) ==
            (DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY))
            ++d3d_target_stats.hidden_offscreen;
    }
}
#endif

/* Only carry work between callbacks for an explicit scene on a hidden
 * draw buffer. The primary can be a hidden flip buffer when GDI
 * synchronizes its direct access.
 */
static BOOL wgpu_d3d_hidden_target(PDEV *pd, const WgpuD3dContext *context,
                                  const WgpuNtFill *target)
{
    const DWORD caps = context->target->ddsCaps.dwCaps;
#ifdef WGPU_NT_OFFSCREEN_PIPELINE
    const ULONGLONG primary_bytes = (ULONGLONG)pd->stride * pd->h;
    const ULONGLONG end = (ULONGLONG)target->offset +
        (ULONGLONG)target->pitch * target->height;
    const ULONGLONG scanout_end = (ULONGLONG)pd->start + primary_bytes;
    /* The GDI primary stays at offset zero even when DirectDraw flips.
     * Other VRAM surfaces can only be exposed through the DDraw barriers. */
    return (caps & DDSCAPS_VIDEOMEMORY) &&
        (caps & (DDSCAPS_BACKBUFFER | DDSCAPS_OFFSCREENPLAIN)) &&
        !(caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_OVERLAY)) &&
        target->offset >= primary_bytes &&
        (target->offset >= scanout_end || end <= pd->start);
#else
    const ULONGLONG end = (ULONGLONG)target->offset +
        (ULONGLONG)(target->height - 1) * target->pitch + target->width * pd->Bpp;
    const ULONGLONG scanout_end = (ULONGLONG)pd->start +
        (ULONGLONG)(pd->h - 1) * pd->stride + pd->w * pd->Bpp;
#ifdef WGPU_NT_OPENGL_ICD
    const ULONGLONG primary_bytes = (ULONGLONG)pd->stride * pd->h;
    const DWORD offscreen = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
    /* Lock, Blt and surface destruction all drain before exposing this
     * allocation. GDI can address the primary independently.
     */
    if ((caps & offscreen) == offscreen &&
        !(caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_OVERLAY)) &&
        target->offset >= primary_bytes &&
        (target->offset >= pd->start + primary_bytes || end <= pd->start))
        return TRUE;
#endif
#ifdef WGPU_NT_GDI_PIPELINE
    /* In a flip chain the old primary can be the hidden draw buffer. GDI
     * synchronizes before accessing the linear bitmap, and DirectDraw
     * drains before CPU access or displaying this buffer. */
    if (wgpu_gdi_hook_ready && pd->hsurf && !target->offset && end <= pd->start &&
        (caps & (DDSCAPS_FLIP | DDSCAPS_BACKBUFFER | DDSCAPS_FRONTBUFFER)) &&
        !(caps & DDSCAPS_OVERLAY)) return TRUE;
#endif
    return (caps & DDSCAPS_BACKBUFFER) &&
        !(caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_OVERLAY)) &&
        (target->offset >= scanout_end || end <= pd->start);
#endif
}

/* DirectDraw calls this before touching or publishing a surface. The caller
 * enters without d3d_lock; the DP2 parser already owns it. */
static HRESULT wgpu_d3d_drain(void)
{
    HRESULT result = DD_OK;
    if (d3d_lock) {
        EngAcquireSemaphore(d3d_lock);
#ifdef WGPU_NT_CLEAN_TEXTURES
        d3d_clean_textures_valid = 0;
#endif
        if (d3d_group_device) result = wgpu_d3d_group_flush(d3d_group_device);
#ifdef WGPU_NT_PIPELINE
        else result = wgpu_d3d_retire_pipeline();
#endif
        EngReleaseSemaphore(d3d_lock);
    }
    return result;
}
#endif

#ifdef WGPU_NT_CLEAN_TEXTURES
/* Retain published texture rows between draws on a hidden target. Every
 * CPU access barrier empties the cache, and a colliding key only evicts. */
static void wgpu_d3d_flush_texture(PDEV *pd, const WgpuD3dContext *context,
                                    const WgpuNtMaterialDraw *draw)
{
    WgpuNtFill range;
    ULONG start, row_bytes, end, bit;
    unsigned slot;
    WgpuD3dCleanTexture *entry;
    const ULONGLONG primary_bytes = (ULONGLONG)pd->h * pd->stride;
    if (!wgpu_nt_material_texture_range(&draw->texture, pd->vram, 0x1000ul, &range)) return;
    start = range.offset;
    row_bytes = range.width * (range.bpp / 8);
    end = start + (range.height - 1) * range.pitch + row_bytes;
    slot = ((start >> 6) ^ (start >> 12)) & 31;
    bit = 1ul << slot;
    entry = &d3d_clean_textures[slot];
    if (d3d_clean_textures_device != pd) {
        d3d_clean_textures_valid = 0;
        d3d_clean_textures_device = pd;
    }
    if (start >= primary_bytes &&
        (start >= pd->start + primary_bytes || end <= pd->start) &&
        context->scene_open && wgpu_d3d_hidden_target(pd, context, &draw->target)) {
        if ((d3d_clean_textures_valid & bit) && entry->offset == start &&
            entry->pitch == range.pitch && entry->row_bytes == row_bytes &&
            entry->height == range.height) return;
        *entry = (WgpuD3dCleanTexture){start, range.pitch, row_bytes, range.height};
        d3d_clean_textures_valid |= bit;
    }
    wgpu_flush_fill(pd, &range);
}
#endif

static BOOL wgpu_d3d_same_material(const WgpuNtMaterialDraw *a, const WgpuNtMaterialDraw *b)
{
    const BYTE *left = (const BYTE *)a, *right = (const BYTE *)b;
    ULONG i;
    for (i = 0; i < WGPU_NT_MATERIAL_HEADER_BYTES; ++i) {
        if (i == offsetof(WgpuNtMaterialDraw, vertex_count))
            i = offsetof(WgpuNtMaterialDraw, format);
        if (left[i] != right[i]) return FALSE;
    }
    return TRUE;
}

static HRESULT wgpu_d3d_group_draw(PDEV *pd, const WgpuD3dContext *context,
    const WgpuNtFill *damage,
    const WgpuNtMaterialDraw *draw)
{
    const ULONG vertex_bytes = draw->vertex_count * sizeof(draw->vertices[0]);
    ULONG bytes = wgpu_nt_material_bytes(draw->vertex_count);
    WgpuNtMaterialDraw *last = NULL;
    HRESULT result;
#ifndef WGPU_NT_PIPELINE
    (void)context;
#endif
    if (d3d_group.packet.count) {
        last = (void *)((BYTE *)&d3d_group + d3d_group_last);
        if (last->vertex_count + draw->vertex_count > WGPU_NT_MATERIAL_MAX_VERTICES ||
            vertex_bytes > sizeof(d3d_group) - d3d_group.packet.size ||
            !wgpu_d3d_same_material(last, draw))
            last = NULL;
    }
    if (!last && (d3d_group.packet.count == WGPU_NT_MATERIAL_GROUP_MAX ||
        bytes > sizeof(d3d_group) - d3d_group.packet.size)) {
#ifdef WGPU_NT_PIPELINE
        result = wgpu_d3d_group_send(pd, context->scene_open &&
                                     wgpu_d3d_hidden_target(pd, context, &draw->target));
#else
        result = wgpu_d3d_group_flush(pd);
#endif
        if (result != DD_OK) return result;
    }
    if (!d3d_group.packet.count) d3d_group_first = d3d_group_command;
    if (damage) {
        if (!d3d_group_has_damage) {
            d3d_group_damage = *damage;
            d3d_group_has_damage = TRUE;
        } else wgpu_d3d_merge_damage(&d3d_group_damage, damage);
    }
    if (last) {
        D3D_TIMING_COUNT(merged_records);
        memcpy((BYTE *)&d3d_group + d3d_group.packet.size, draw->vertices, vertex_bytes);
        last->vertex_count += draw->vertex_count;
        d3d_group.packet.size += vertex_bytes;
    } else {
        if (draw->combine != WGPU_NT_TEXTURE_DISABLED) {
            D3D_TIMER(start);
#ifdef WGPU_NT_CLEAN_TEXTURES
            wgpu_d3d_flush_texture(pd, context, draw);
#else
            wgpu_d3d_flush_material_texture(pd, &draw->texture);
#endif
            D3D_ELAPSED(texture_ticks, start);
        }
        d3d_group_last = d3d_group.packet.size;
        memcpy((BYTE *)&d3d_group + d3d_group.packet.size, draw, bytes);
        d3d_group.packet.size += bytes;
        ++d3d_group.packet.count;
    }
    return DD_OK;
}
#endif

static WgpuD3dContext *wgpu_d3d_context(ULONG_PTR id)
{
    HANDLE process = EngGetCurrentProcessId();
    unsigned i;
    for (i = 0; i < WGPU_D3D_CONTEXTS; ++i)
        if (id && d3d_contexts[i].id == id && d3d_contexts[i].process == process)
            return &d3d_contexts[i];
    return NULL;
}

static PDD_SURFACE_LOCAL wgpu_d3d_find_surface(PDD_DIRECTDRAW_LOCAL draw, DWORD handle)
{
    unsigned i;
    if (!handle) return NULL;
    for (i = 0; i < WGPU_D3D_TARGETS; ++i) {
        WgpuD3dTarget *target = &d3d_targets[i];
        if (target->draw != draw || (!target->flipping && target->handle != handle))
            continue;
        /* A flip can exchange live handles. Check the registered surface
         * association before reading its metadata, and never scan metadata
         * belonging to unrelated non-flipping surfaces. */
        if (target->surface->lpSurfMore != target->more) continue;
        if (target->more->dwSurfaceHandle == handle)
            return target->surface;
    }
    return NULL;
}

static PDD_SURFACE_LOCAL wgpu_d3d_surface(PDD_DIRECTDRAW_LOCAL draw, DWORD handle)
{
    PDD_SURFACE_LOCAL surface;
    D3D_TIMER(start);
    surface = wgpu_d3d_find_surface(draw, handle);
    D3D_ELAPSED(surface_lookup_ticks, start);
    D3D_TIMING_COUNT(surface_lookups);
    return surface;
}

static DWORD WINAPI wgpu_d3d_create(LPD3DHAL_CONTEXTCREATEDATA data)
{
    WgpuNtFill target;
    PDD_DIRECTDRAW_LOCAL draw = data->lpDDLcl;
    unsigned i;
    data->dwhContext = 0;
    data->ddrval = DDERR_UNSUPPORTED;
    EngAcquireSemaphore(d3d_lock);
    D3D_TRACE(context_creates, d3d_trace.context_creates + 1);
    if (!draw || !draw->lpGbl ||
        !wgpu_d3d_render_target(draw->lpGbl->dhpdev, data->lpDDSLcl, &target))
        goto done;
#ifdef WGPU_NT_ENABLE_DEPTH
    if (data->lpDDSZLcl) {
        WgpuD3dDepth depth;
        if (!wgpu_depth_surface(draw->lpGbl->dhpdev, data->lpDDSZLcl, &depth) ||
            depth.width < target.width || depth.height < target.height) goto done;
    }
#else
    if (data->lpDDSZLcl) goto done;
#endif
    data->ddrval = D3DHAL_OUTOFCONTEXTS;
    for (i = 0; i < WGPU_D3D_CONTEXTS; ++i) {
        WgpuD3dContext *context = &d3d_contexts[i];
        if (context->id) continue;
        memset(context, 0, sizeof(*context));
        if (++d3d_next_id == 0) ++d3d_next_id;
        context->id = d3d_next_id;
        context->process = EngGetCurrentProcessId();
        context->draw = draw;
        context->target = data->lpDDSLcl;
        context->shade = D3DSHADE_GOURAUD;
        context->cull = D3DCULL_CCW;
#ifdef WGPU_NT_ENABLE_MATERIAL
        context->stage_state[D3DTSS_COLOROP] = D3DTOP_MODULATE;
        context->stage_state[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        context->stage_state[D3DTSS_COLORARG2] = D3DTA_CURRENT;
        context->stage_state[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
        context->stage_state[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        context->stage_state[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        context->stage_state[D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
        context->stage_state[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
        context->stage_state[D3DTSS_MINFILTER] = D3DTFN_POINT;
        context->stage_state[D3DTSS_MAGFILTER] = D3DTFG_POINT;
        context->stage_state[D3DTSS_MIPFILTER] = D3DTFP_NONE;
        context->blend_src = D3DBLEND_ONE;
        context->blend_dst = D3DBLEND_ZERO;
        context->alpha_compare = D3DCMP_ALWAYS;
        context->perspective = TRUE;
#endif
        context->viewport.dwWidth = target.width;
        context->viewport.dwHeight = target.height;
#ifdef WGPU_NT_ENABLE_DEPTH
        context->depth = data->lpDDSZLcl;
        context->depth_enabled = context->depth != NULL;
        context->depth_write = TRUE;
        context->depth_compare = D3DCMP_LESSEQUAL;
#endif
        data->dwhContext = context->id;
        data->ddrval = DD_OK;
        break;
    }
done:
    D3D_TRACE(create_status, data->ddrval);
    EngReleaseSemaphore(d3d_lock);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI wgpu_d3d_destroy(LPD3DHAL_CONTEXTDESTROYDATA data)
{
    WgpuD3dContext *context;
    EngAcquireSemaphore(d3d_lock);
    context = wgpu_d3d_context(data->dwhContext);
#ifdef WGPU_NT_SCENE_GROUPS
    if (d3d_group_device && d3d_group_context == context)
        wgpu_d3d_group_flush(d3d_group_device);
#endif
#ifdef WGPU_NT_SCENE_PIPELINE
    if (d3d_pipeline_device && d3d_pipeline_context == context) {
        wgpu_d3d_retire_pipeline();
        d3d_pipeline_context = NULL;
    }
#endif
    if (context) memset(context, 0, sizeof(*context));
    EngReleaseSemaphore(d3d_lock);
    data->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI wgpu_d3d_destroy_all(LPD3DHAL_CONTEXTDESTROYALLDATA data)
{
    unsigned i;
    EngAcquireSemaphore(d3d_lock);
#ifdef WGPU_NT_SCENE_GROUPS
    if (d3d_group_device && d3d_group_context->process == (HANDLE)(ULONG_PTR)data->dwPID)
        wgpu_d3d_group_flush(d3d_group_device);
#endif
#ifdef WGPU_NT_SCENE_PIPELINE
    if (d3d_pipeline_context && d3d_pipeline_context->process == (HANDLE)(ULONG_PTR)data->dwPID) {
        wgpu_d3d_retire_pipeline();
        d3d_pipeline_context = NULL;
    }
#endif
    for (i = 0; i < WGPU_D3D_CONTEXTS; ++i)
        if (d3d_contexts[i].process == (HANDLE)(ULONG_PTR)data->dwPID)
            memset(&d3d_contexts[i], 0, sizeof(d3d_contexts[i]));
    EngReleaseSemaphore(d3d_lock);
    data->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Called under d3d_lock for either kind of DirectDraw surface release. */
static void wgpu_d3d_forget_surface(PDD_SURFACE_LOCAL surface)
{
    unsigned i;
    for (i = 0; i < WGPU_D3D_TARGETS; ++i)
        if (d3d_targets[i].surface == surface)
            memset(&d3d_targets[i], 0, sizeof(d3d_targets[i]));
    for (i = 0; i < WGPU_D3D_CONTEXTS; ++i) {
        if (d3d_contexts[i].target == surface) d3d_contexts[i].target = NULL;
#ifdef WGPU_NT_ENABLE_DEPTH
        if (d3d_contexts[i].depth == surface) d3d_contexts[i].depth = NULL;
#endif
    }
}

static BOOL wgpu_d3d_register_surface(PDD_DIRECTDRAW_LOCAL draw, PDD_SURFACE_LOCAL surface)
{
    unsigned i, available = WGPU_D3D_TARGETS;
    WgpuD3dTarget *target;
    if (!surface) return TRUE;
    if (!surface->lpGbl || !surface->lpSurfMore ||
        !surface->lpSurfMore->dwSurfaceHandle || (!surface->lpGbl->fpVidMem &&
        !(surface->ddsCaps.dwCaps & (DDSCAPS_VIDEOMEMORY | DDSCAPS_PRIMARYSURFACE)))) {
        wgpu_d3d_forget_surface(surface);
        return TRUE;
    }
    for (i = 0; i < WGPU_D3D_TARGETS; ++i) {
        if (d3d_targets[i].draw == draw && d3d_targets[i].surface == surface) {
            available = i;
            break;
        }
        if (!d3d_targets[i].draw && available == WGPU_D3D_TARGETS) available = i;
    }
    if (!(surface->ddsCaps.dwCaps & (WGPU_D3D_RENDER_CAPS
#ifdef WGPU_NT_ENABLE_MATERIAL
          | DDSCAPS_TEXTURE
#endif
#ifdef WGPU_NT_ENABLE_DEPTH
          | DDSCAPS_ZBUFFER
#endif
          ))) return TRUE;
    if (available == WGPU_D3D_TARGETS) return FALSE;
    target = &d3d_targets[available];
    if (!target->draw && (surface->ddsCaps.dwCaps & DDSCAPS_TEXTURE))
        D3D_TRACE(texture_registered, d3d_trace.texture_registered + 1);
    target->draw = draw;
    target->surface = surface;
    target->more = surface->lpSurfMore;
    target->handle = target->more->dwSurfaceHandle;
    target->flipping = (surface->ddsCaps.dwCaps &
        (DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER)) != 0;
    return TRUE;
}

static DWORD WINAPI wgpu_d3d_create_surface(PDD_CREATESURFACEEXDATA data)
{
    PDD_SURFACE_LOCAL chain[64];
    unsigned index, count = 1;
    data->ddRVal = DD_OK;
    if (!data->lpDDSLcl || !data->lpDDLcl) return DDHAL_DRIVER_HANDLED;
    chain[0] = data->lpDDSLcl;
    EngAcquireSemaphore(d3d_lock);
#ifdef WGPU_NT_SCENE_GROUPS
    if (d3d_group_device) {
        data->ddRVal = wgpu_d3d_group_flush(d3d_group_device);
        if (data->ddRVal != DD_OK) {
            EngReleaseSemaphore(d3d_lock);
            return DDHAL_DRIVER_HANDLED;
        }
    }
#endif
#ifdef WGPU_NT_SCENE_PIPELINE
    if (d3d_pipeline_device) {
        data->ddRVal = wgpu_d3d_retire_pipeline();
        if (data->ddRVal != DD_OK) {
            EngReleaseSemaphore(d3d_lock);
            return DDHAL_DRIVER_HANDLED;
        }
    }
#endif
    for (index = 0; index < count; ++index) {
        PDD_SURFACE_LOCAL surface = chain[index];
        PDD_ATTACHLIST link;
        unsigned links = 0;
        if (!wgpu_d3d_register_surface(data->lpDDLcl, surface)) {
            data->ddRVal = DDERR_OUTOFVIDEOMEMORY;
            break;
        }
        if (!surface->lpGbl || !surface->lpSurfMore) continue;
        for (link = surface->lpAttachList; link && links++ < 64; link = link->lpLink) {
            unsigned seen;
            if (!link->lpAttached) continue;
            for (seen = 0; seen < count && chain[seen] != link->lpAttached; ++seen) {}
            if (seen < count) continue;
            if (count == sizeof(chain) / sizeof(chain[0])) {
                data->ddRVal = DDERR_OUTOFVIDEOMEMORY;
                break;
            }
            chain[count++] = link->lpAttached;
        }
        if (data->ddRVal != DD_OK) break;
    }
    EngReleaseSemaphore(d3d_lock);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI wgpu_d3d_destroy_local(PDD_DESTROYDDLOCALDATA data)
{
    unsigned i;
    EngAcquireSemaphore(d3d_lock);
#ifdef WGPU_NT_SCENE_GROUPS
    if (d3d_group_device && d3d_group_context->draw == data->pDDLcl)
        wgpu_d3d_group_flush(d3d_group_device);
#endif
#ifdef WGPU_NT_SCENE_PIPELINE
    if (d3d_pipeline_context && d3d_pipeline_context->draw == data->pDDLcl) {
        wgpu_d3d_retire_pipeline();
        d3d_pipeline_context = NULL;
    }
#endif
    for (i = 0; i < WGPU_D3D_TARGETS; ++i)
        if (d3d_targets[i].draw == data->pDDLcl)
            memset(&d3d_targets[i], 0, sizeof(d3d_targets[i]));
    for (i = 0; i < WGPU_D3D_CONTEXTS; ++i)
        if (d3d_contexts[i].draw == data->pDDLcl)
            memset(&d3d_contexts[i], 0, sizeof(d3d_contexts[i]));
    EngReleaseSemaphore(d3d_lock);
    data->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI wgpu_d3d_get_state(PDD_GETDRIVERSTATEDATA data)
{
    data->ddRVal = DDERR_UNSUPPORTED;
    return DDHAL_DRIVER_HANDLED;
}

#ifdef WGPU_NT_ENABLE_MATERIAL
#include "d3d_material.inc"
#endif

static DWORD WINAPI wgpu_d3d_validate_stages(LPD3DHAL_VALIDATETEXTURESTAGESTATEDATA data)
{
    WgpuD3dContext *context;
    unsigned i;
    EngAcquireSemaphore(d3d_lock);
    context = wgpu_d3d_context(data->dwhContext);
    data->ddrval = context ? DD_OK : DDERR_INVALIDPARAMS;
    if (context) {
#ifdef WGPU_NT_ENABLE_MATERIAL
        if (context->textures[0] && !wgpu_d3d_texture_combine(context))
            data->ddrval = DDERR_UNSUPPORTED;
        i = 1;
#else
        i = 0;
#endif
        for (; i < sizeof(context->textures) / sizeof(context->textures[0]); ++i)
            if (context->textures[i]) data->ddrval = DDERR_UNSUPPORTED;
    }
    data->dwNumPasses = data->ddrval == DD_OK ? 1 : 0;
    EngReleaseSemaphore(d3d_lock);
    return DDHAL_DRIVER_HANDLED;
}

/* Use a private snapshot. A user mapping can still be written after it is
 * secured, including by a second thread while a command is decoded.
 */
#ifdef WGPU_NT_FAST_STREAM
/* Commands and vertices coexist under d3d_lock. Large streams keep using
 * EngAllocMem, and both paths secure the user range before copying. */
static DWORD d3d_short_commands[8192 / sizeof(DWORD)];
static DWORD d3d_short_vertices[32768 / sizeof(DWORD)];
#define WGPU_COMMAND_SNAPSHOT (BYTE *)d3d_short_commands, sizeof(d3d_short_commands)
#define WGPU_VERTEX_SNAPSHOT (BYTE *)d3d_short_vertices, sizeof(d3d_short_vertices)
#else
#define WGPU_COMMAND_SNAPSHOT NULL, 0
#define WGPU_VERTEX_SNAPSHOT NULL, 0
#endif

static BYTE *wgpu_d3d_snapshot(ULONG_PTR base, DWORD offset, DWORD bytes,
                               BYTE *scratch, DWORD scratch_bytes)
{
    HANDLE secured;
    BYTE *snapshot;
    if (!bytes || bytes > WGPU_D3D_STREAM_LIMIT || base < 0x10000 ||
        base >= WGPU_D3D_USER_LIMIT || offset >= WGPU_D3D_USER_LIMIT - base ||
        bytes > WGPU_D3D_USER_LIMIT - (base + offset)) return NULL;
    secured = EngSecureMem((void *)(base + offset), bytes);
    if (!secured) return NULL;
    snapshot = bytes <= scratch_bytes ? scratch : EngAllocMem(0, bytes, 0x64337677);
    if (snapshot) memcpy(snapshot, (const void *)(base + offset), bytes);
    EngUnsecureMem(secured);
    return snapshot;
}

static BYTE *wgpu_d3d_surface_data(PDD_SURFACE_LOCAL surface, DWORD offset, DWORD bytes,
                                   BYTE *scratch, DWORD scratch_bytes)
{
    PDD_SURFACE_GLOBAL global;
    if (!surface || !surface->lpGbl ||
        !(surface->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) return NULL;
    global = surface->lpGbl;
    if (offset > global->dwLinearSize || bytes > global->dwLinearSize - offset)
        return NULL;
    return wgpu_d3d_snapshot(global->fpVidMem, offset, bytes, scratch, scratch_bytes);
}

typedef struct WgpuD3dVertices {
    BYTE *buffer;
    DWORD stride, count;
} WgpuD3dVertices;

static DWORD wgpu_d3d_vertex_stride(LPD3DHAL_DRAWPRIMITIVES2DATA data)
{
    const DWORD fvf = data->dwVertexType;
    const DWORD base_fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    DWORD expected = 5 * sizeof(DWORD);
    if ((fvf & ~(D3DFVF_SPECULAR | D3DFVF_TEX1)) != base_fvf) return 0;
    if (fvf & D3DFVF_SPECULAR) expected += sizeof(DWORD);
    if (fvf & D3DFVF_TEX1) expected += 2 * sizeof(DWORD);
    return !data->dwVertexSize || data->dwVertexSize == expected ? expected : 0;
}

static BOOL wgpu_d3d_read_vertices(LPD3DHAL_DRAWPRIMITIVES2DATA data, WgpuD3dVertices *vertices)
{
    const BOOL user = (data->dwFlags & D3DHALDP2_USERMEMVERTICES) != 0;
    PDD_SURFACE_LOCAL surface = user ? NULL : data->lpDDVertex;
    DWORD expected = wgpu_d3d_vertex_stride(data), bytes;
    vertices->count = data->dwVertexLength;
    vertices->stride = expected;
    if (!expected || vertices->count > WGPU_D3D_STREAM_LIMIT / expected) return FALSE;
    if (surface && surface->lpGbl) {
        DWORD capacity = surface->lpGbl->dwLinearSize;
        D3D_TRACE(vertex_caps, surface->ddsCaps.dwCaps);
        D3D_TRACE(vertex_capacity, capacity);
        /* XP can omit the vertex count for a bounded system allocation. */
        if (!vertices->count && capacity > data->dwVertexOffset &&
            (surface->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
            DWORD available = capacity - data->dwVertexOffset;
            if (available > WGPU_D3D_STREAM_LIMIT) available = WGPU_D3D_STREAM_LIMIT;
            vertices->count = available / expected;
        }
    }
    D3D_TRACE(available_vertices, vertices->count);
    bytes = vertices->count * expected;
    if (!bytes) return FALSE;
    vertices->buffer = user ?
        wgpu_d3d_snapshot((ULONG_PTR)data->lpVertices, data->dwVertexOffset, bytes, WGPU_VERTEX_SNAPSHOT) :
        wgpu_d3d_surface_data(surface, data->dwVertexOffset, bytes, WGPU_VERTEX_SNAPSHOT);
    return vertices->buffer != NULL;
}

/* Accept rendering states with an implemented effect. */
static BOOL wgpu_d3d_render_state(WgpuD3dContext *context, DWORD state, DWORD value)
{
    /* Pattern words are inert because stippling itself is unsupported. */
    if (state >= D3DRENDERSTATE_STIPPLEPATTERN00 &&
        state <= D3DRENDERSTATE_STIPPLEPATTERN31)
        return TRUE;
    switch (state) {
#ifdef WGPU_NT_ENABLE_DEPTH
    case D3DRENDERSTATE_ZENABLE:
        if (value != D3DZB_FALSE && value != D3DZB_TRUE) return FALSE;
        context->depth_enabled = value == D3DZB_TRUE;
        return TRUE;
    case D3DRENDERSTATE_ZWRITEENABLE:
        if (value > 1) return FALSE;
        context->depth_write = value != 0;
        return TRUE;
    case D3DRENDERSTATE_ZFUNC:
        if (value < D3DCMP_NEVER || value > D3DCMP_ALWAYS) return FALSE;
        context->depth_compare = value;
        return TRUE;
#endif
#ifdef WGPU_NT_ENABLE_MATERIAL
    case D3DRENDERSTATE_TEXTUREPERSPECTIVE:
        if (value > 1) return FALSE;
        context->perspective = value != 0;
        return TRUE;
    case D3DRENDERSTATE_ALPHABLENDENABLE:
    case D3DRENDERSTATE_ALPHATESTENABLE:
    case D3DRENDERSTATE_SPECULARENABLE: {
        DWORD flag = state == D3DRENDERSTATE_ALPHABLENDENABLE ? WGPU_NT_BLEND_ENABLE :
            state == D3DRENDERSTATE_ALPHATESTENABLE ? WGPU_NT_ALPHA_TEST : WGPU_NT_SPECULAR_ENABLE;
        if (value > 1) return FALSE;
        if (value) context->fragment_flags |= flag;
        else context->fragment_flags &= ~flag;
        return TRUE;
    }
    case D3DRENDERSTATE_SRCBLEND:
    case D3DRENDERSTATE_DESTBLEND:
        if (value < D3DBLEND_ZERO || value > (state == D3DRENDERSTATE_SRCBLEND ?
            WGPU_NT_BLEND_FACTOR_MAX : WGPU_NT_BLEND_DEST_MAX)) return FALSE;
        if (state == D3DRENDERSTATE_SRCBLEND) context->blend_src = value;
        else context->blend_dst = value;
        return TRUE;
    case D3DRENDERSTATE_ALPHAFUNC:
        if (value < D3DCMP_NEVER || value > WGPU_NT_COMPARE_MAX) return FALSE;
        context->alpha_compare = value;
        return TRUE;
    case D3DRENDERSTATE_ALPHAREF:
        context->alpha_ref = value & 255;
        return TRUE;
    case D3DRENDERSTATE_TEXTUREHANDLE:
        context->textures[0] = value;
        return TRUE;
    case D3DRENDERSTATE_WRAP0:
        /* Only the U and V flags address the two coordinates in this HAL.
         * The legacy runtime can leave other bits set while clearing them. */
        context->wrap_coordinates = value & (D3DWRAP_U | D3DWRAP_V);
        return TRUE;
    case D3DRENDERSTATE_WRAPU:
    case D3DRENDERSTATE_WRAPV: {
        DWORD flag = state == D3DRENDERSTATE_WRAPU ? D3DWRAP_U : D3DWRAP_V;
        if (value > 1) return FALSE;
        context->wrap_coordinates = value ? context->wrap_coordinates | flag :
            context->wrap_coordinates & ~flag;
        return TRUE;
    }
    case D3DRENDERSTATE_TEXTUREMAPBLEND:
        if (value < D3DTBLEND_DECAL || value > D3DTBLEND_ADD) return FALSE;
        context->legacy_blend = value;
        return TRUE;
    case D3DRENDERSTATE_TEXTUREMIN:
        if (value != D3DFILTER_NEAREST && value != D3DFILTER_LINEAR) return FALSE;
        context->stage_state[D3DTSS_MINFILTER] = value;
        return TRUE;
    case D3DRENDERSTATE_TEXTUREMAG:
        if (value != D3DFILTER_NEAREST && value != D3DFILTER_LINEAR) return FALSE;
        context->stage_state[D3DTSS_MAGFILTER] = value;
        return TRUE;
    case D3DRENDERSTATE_TEXTUREADDRESS:
        context->stage_state[D3DTSS_ADDRESSU] = value;
        context->stage_state[D3DTSS_ADDRESSV] = value;
        return TRUE;
    case D3DRENDERSTATE_TEXTUREADDRESSU:
    case D3DRENDERSTATE_TEXTUREADDRESSV:
        context->stage_state[state == D3DRENDERSTATE_TEXTUREADDRESSU ?
                             D3DTSS_ADDRESSU : D3DTSS_ADDRESSV] = value;
        return TRUE;
#endif
    case D3DRENDERSTATE_SCENECAPTURE:
        if (value > 1) return FALSE;
#ifdef WGPU_NT_SCENE_GROUPS
        context->scene_open = value != 0;
        D3D_TIMING_ADD(scene_begin, value != 0);
        D3D_TIMING_ADD(scene_end, value == 0);
#endif
        return TRUE;
    case D3DRENDERSTATE_CULLMODE:
        if (value < D3DCULL_NONE || value > D3DCULL_CCW) return FALSE;
        context->cull = value;
        return TRUE;
    case D3DRENDERSTATE_SHADEMODE:
        if (value != D3DSHADE_GOURAUD && value != D3DSHADE_FLAT) return FALSE;
        context->shade = value;
        return TRUE;
    case D3DRENDERSTATE_FILLMODE:
        return value == D3DFILL_SOLID;
    case D3DRENDERSTATE_ANISOTROPY:
        return value == 1; /* The sampler is isotropic. */
    case D3DRENDERSTATE_ROP2:
        return value == R2_COPYPEN;
    case D3DRENDERSTATE_PLANEMASK:
        return value == 0xffffffff;
    case D3DRENDERSTATE_LINEPATTERN:
    case D3DRENDERSTATE_LASTPIXEL:
        return TRUE; /* This HAL rasterizes triangles. */
#ifndef WGPU_NT_ENABLE_DEPTH
    case D3DRENDERSTATE_ZWRITEENABLE:
    case D3DRENDERSTATE_ZFUNC:
#endif
    case D3DRENDERSTATE_LIGHTING:
    case D3DRENDERSTATE_CLIPPING:
    case D3DRENDERSTATE_EXTENTS:
#ifndef WGPU_NT_ENABLE_MATERIAL
    case D3DRENDERSTATE_SRCBLEND:
    case D3DRENDERSTATE_DESTBLEND:
    case D3DRENDERSTATE_ALPHAFUNC:
    case D3DRENDERSTATE_ALPHAREF:
#endif
    case D3DRENDERSTATE_DITHERENABLE:
    case D3DRENDERSTATE_AMBIENT:
    case D3DRENDERSTATE_COLORVERTEX:
    case D3DRENDERSTATE_LOCALVIEWER:
    case D3DRENDERSTATE_NORMALIZENORMALS:
    case D3DRENDERSTATE_DIFFUSEMATERIALSOURCE:
    case D3DRENDERSTATE_SPECULARMATERIALSOURCE:
    case D3DRENDERSTATE_AMBIENTMATERIALSOURCE:
    case D3DRENDERSTATE_EMISSIVEMATERIALSOURCE:
    case D3DRENDERSTATE_FOGCOLOR:
    case D3DRENDERSTATE_FOGTABLEMODE:
    case D3DRENDERSTATE_FOGSTART:
    case D3DRENDERSTATE_FOGEND:
    case D3DRENDERSTATE_FOGDENSITY:
    case D3DRENDERSTATE_FOGVERTEXMODE:
    case D3DRENDERSTATE_STENCILFAIL:
    case D3DRENDERSTATE_STENCILZFAIL:
    case D3DRENDERSTATE_STENCILPASS:
    case D3DRENDERSTATE_STENCILFUNC:
    case D3DRENDERSTATE_STENCILREF:
    case D3DRENDERSTATE_STENCILMASK:
    case D3DRENDERSTATE_STENCILWRITEMASK:
    case D3DRENDERSTATE_BORDERCOLOR:
    case D3DRENDERSTATE_MIPMAPLODBIAS:
    case D3DRENDERSTATE_TEXTUREFACTOR:
    case D3DRENDERSTATE_FLUSHBATCH:
        /* TL vertices have already been lit and transformed. Depth/blend
         * factors have no effect with the corresponding operations off. */
        return TRUE;
#ifndef WGPU_NT_ENABLE_MATERIAL
    case D3DRENDERSTATE_TEXTUREHANDLE:
    case D3DRENDERSTATE_ALPHABLENDENABLE:
    case D3DRENDERSTATE_ALPHATESTENABLE:
    case D3DRENDERSTATE_SPECULARENABLE:
    case D3DRENDERSTATE_WRAPU:
    case D3DRENDERSTATE_WRAPV:
    case D3DRENDERSTATE_WRAP0:
#endif
#ifndef WGPU_NT_ENABLE_DEPTH
    case D3DRENDERSTATE_ZENABLE:
#endif
    case D3DRENDERSTATE_FOGENABLE:
    case D3DRENDERSTATE_STENCILENABLE:
    case D3DRENDERSTATE_COLORKEYENABLE:
    case D3DRENDERSTATE_COLORKEYBLENDENABLE:
    case D3DRENDERSTATE_ANTIALIAS:
    case D3DRENDERSTATE_SUBPIXEL:
    case D3DRENDERSTATE_SUBPIXELX:
    case D3DRENDERSTATE_EDGEANTIALIAS:
    case D3DRENDERSTATE_ZVISIBLE:
    case D3DRENDERSTATE_ZBIAS:
    case D3DRENDERSTATE_RANGEFOGENABLE:
    case D3DRENDERSTATE_MONOENABLE:
    case D3DRENDERSTATE_STIPPLEENABLE:
    case D3DRENDERSTATE_STIPPLEDALPHA:
    case D3DRENDERSTATE_WRAP1:
    case D3DRENDERSTATE_WRAP2:
    case D3DRENDERSTATE_WRAP3:
    case D3DRENDERSTATE_WRAP4:
    case D3DRENDERSTATE_WRAP5:
    case D3DRENDERSTATE_WRAP6:
    case D3DRENDERSTATE_WRAP7:
    case D3DRENDERSTATE_CLIPPLANEENABLE:
        return value == 0;
    default:
        return FALSE;
    }
}

static DWORD wgpu_d3d_vertex_index(const BYTE *indices, DWORD record_bytes,
                                  D3DPRIMITIVETYPE primitive, DWORD index)
{
    WORD vertex_index;
    DWORD triangle = index / 3;
    DWORD corner = index % 3;
    if (primitive == D3DPT_TRIANGLESTRIP)
        index = triangle + (corner == 2 ? 2 : (corner ^ (triangle & 1)));
    else if (primitive == D3DPT_TRIANGLEFAN)
        /* Start at the outside edge so flat shading uses its first vertex. */
        index = corner == 2 ? 0 : triangle + corner + 1;
    if (!indices) return index;
    if (primitive == D3DPT_TRIANGLELIST)
        indices += triangle * record_bytes + corner * sizeof(vertex_index);
    else
        indices += index * sizeof(vertex_index);
    memcpy(&vertex_index, indices, sizeof(vertex_index));
    return vertex_index;
}

/* Convert a validated screen coordinate (at most 4096 in magnitude) to
 * signed 16.16 without using the x86 floating-point state in a DDI call. */
static LONG wgpu_d3d_fixed_coordinate(DWORD bits)
{
    int shift = (int)((bits >> 23) & 255) - 134;
    DWORD mantissa = (bits & 0x7fffff) | 0x800000;
    if (shift <= -24) return 0;
    LONG coordinate = shift < 0 ? mantissa >> -shift : mantissa << shift;
    return bits & 0x80000000u ? -coordinate : coordinate;
}

static BOOL wgpu_d3d_triangle_culled(DWORD mode, const BYTE *triangle, DWORD stride)
{
    /* Both packet vertex layouts start with two validated screen coordinates.
     * Screen Y grows downward, so positive signed area is clockwise. */
    LONG x[3], y[3];
    if (mode == D3DCULL_NONE) return FALSE;
    for (DWORD corner = 0; corner < 3; ++corner) {
        DWORD coords[2];
        __builtin_memcpy(coords, triangle + corner * stride, sizeof(coords));
        x[corner] = wgpu_d3d_fixed_coordinate(coords[0]);
        y[corner] = wgpu_d3d_fixed_coordinate(coords[1]);
    }
    long long area = (long long)(x[1] - x[0]) * (y[2] - y[0]) -
                     (long long)(y[1] - y[0]) * (x[2] - x[0]);
    return (mode == D3DCULL_CW && area > 0) || (mode == D3DCULL_CCW && area < 0);
}

static DWORD wgpu_d3d_cull(DWORD mode, void *vertices, DWORD count, DWORD stride)
{
    BYTE *buffer = vertices;
    DWORD kept = 0;
    if (mode == D3DCULL_NONE) return count;
    for (DWORD first = 0; first < count; first += 3) {
        BYTE *triangle = buffer + first * stride;
        if (wgpu_d3d_triangle_culled(mode, triangle, stride)) continue;
        if (first != kept) memcpy(buffer + kept * stride, triangle, 3 * stride);
        kept += 3;
    }
    return kept;
}

/* The rasterizer writes only inside the scissor and the submitted geometry.
 * Round the screen bounds outward and include a pixel on either side for
 * subpixel rasterization. CLFLUSH itself widens each row to cache lines. */
static const WgpuNtFill *wgpu_d3d_draw_damage(WgpuNtFill *damage,
    const WgpuNtFill *scissor, const void *vertices, DWORD count, DWORD stride)
{
    const BYTE *buffer = vertices;
    LONG min_x = 4096, max_x = -4096, min_y = 4096, max_y = -4096;
    for (DWORD i = 0; i < count; ++i) {
        DWORD xy[2];
        memcpy(xy, buffer + i * stride, sizeof(xy));
        LONG x = wgpu_d3d_fixed_coordinate(xy[0]) >> 16;
        LONG y = wgpu_d3d_fixed_coordinate(xy[1]) >> 16;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    --min_x;
    --min_y;
    max_x += 2;
    max_y += 2;
    if (min_x >= (LONG)scissor->right || min_y >= (LONG)scissor->bottom ||
        max_x <= (LONG)scissor->left || max_y <= (LONG)scissor->top) return NULL;
    *damage = *scissor;
    if (min_x > (LONG)damage->left) damage->left = min_x;
    if (min_y > (LONG)damage->top) damage->top = min_y;
    if (max_x < (LONG)damage->right) damage->right = max_x;
    if (max_y < (LONG)damage->bottom) damage->bottom = max_y;
    return damage;
}

#ifdef WGPU_NT_ENABLE_MATERIAL
/* All material and depth-clear callers hold d3d_lock. Only the live
 * vertices are copied into the queue or returned through the transport. */
static WgpuNtMaterialDraw d3d_material_scratch;

static HRESULT wgpu_d3d_draw_material(PDEV *pd, const WgpuD3dContext *context,
    const WgpuNtFill *target, const BYTE *vertices, DWORD stride, DWORD fvf,
    DWORD triangles, const BYTE *indices, DWORD record_bytes, D3DPRIMITIVETYPE primitive,
    BOOL textured)
{
    DWORD start = 0;
    HRESULT result = DDERR_UNSUPPORTED;
    WgpuNtMaterialDraw *draw = &d3d_material_scratch;
    if (textured && !(fvf & D3DFVF_TEX1)) return DDERR_UNSUPPORTED;
    memset(draw, 0, WGPU_NT_MATERIAL_HEADER_BYTES);
    if (!wgpu_d3d_material_setup(pd, context, target, draw, textured)) return result;
    while (triangles) {
        DWORD kept = 0, count = triangles;
        if (count > WGPU_NT_MATERIAL_MAX_VERTICES / 3)
            count = WGPU_NT_MATERIAL_MAX_VERTICES / 3;
        draw->vertex_count = count * 3;
        if (!wgpu_nt_material_header_valid(draw, pd->vram, 0x1000ul))
            return DDERR_INVALIDPARAMS;
        for (DWORD first = 0; first < draw->vertex_count; first += 3) {
            WgpuNtTlVertex *triangle = &draw->vertices[kept];
            for (DWORD corner = 0; corner < 3; ++corner) {
                DWORD offset = wgpu_d3d_vertex_index(indices, record_bytes, primitive,
                                                     start + first + corner);
                const BYTE *vertex = vertices + offset * stride;
                WgpuNtTlVertex *output = &triangle[corner];
                __builtin_memcpy(output, vertex, 5 * sizeof(DWORD));
                output->specular = output->u = output->v = 0;
                if (textured && !context->perspective) output->rhw = 0x3f800000;
                if (fvf & D3DFVF_SPECULAR)
                    __builtin_memcpy(&output->specular, vertex + 5 * sizeof(DWORD), sizeof(DWORD));
                if (textured)
                    __builtin_memcpy(&output->u, vertex + stride - 2 * sizeof(DWORD), 2 * sizeof(DWORD));
                if (!wgpu_nt_tl_vertex_valid(output)) return DDERR_INVALIDPARAMS;
                if (context->shade == D3DSHADE_FLAT) {
                    output->diffuse = triangle[0].diffuse;
                    output->specular = triangle[0].specular;
                }
            }
            if (!wgpu_d3d_triangle_culled(context->cull, (const BYTE *)triangle, sizeof(*triangle)))
                kept += 3;
        }
        draw->vertex_count = kept;
        if (draw->vertex_count) {
#if defined(WGPU_NT_ENABLE_GROUPS) && defined(WGPU_NT_LAZY_GROUP_DAMAGE)
            result = wgpu_d3d_group_draw(pd, context, NULL, draw);
#else
            WgpuNtFill damage;
            const WgpuNtFill *flush = wgpu_d3d_draw_damage(&damage, target, draw->vertices,
                                            draw->vertex_count, sizeof(draw->vertices[0]));
#ifdef WGPU_NT_ENABLE_GROUPS
            result = wgpu_d3d_group_draw(pd, context, flush, draw);
#else
            if (textured) {
                D3D_TIMER(start);
                wgpu_d3d_flush_material_texture(pd, &draw->texture);
                D3D_ELAPSED(texture_ticks, start);
            }
            result = wgpu_d3d_submit(pd, WGPU_DISPLAY_MATERIAL_IOCTL, flush, draw,
                                     wgpu_nt_material_bytes(draw->vertex_count));
#endif
#endif
        } else result = DD_OK;
        if (result != DD_OK) break;
        start += count * 3;
        triangles -= count;
    }
    return result;
}
#endif

static HRESULT wgpu_d3d_draw_triangles(PDEV *pd, const WgpuD3dContext *context,
    const WgpuNtFill *target, const BYTE *vertices, DWORD stride,
    DWORD fvf, DWORD first, DWORD vertex_count, DWORD triangles,
    const BYTE *indices, DWORD index_record_bytes, D3DPRIMITIVETYPE primitive)
{
    WgpuNtDraw draw;
    DWORD i = 0, start = 0;
    BOOL whole_viewport = !context->viewport.dwX && !context->viewport.dwY &&
        context->viewport.dwWidth == target->width && context->viewport.dwHeight == target->height;
#ifdef WGPU_NT_ENABLE_MATERIAL
    BOOL textured = context->textures[0] &&
        (context->legacy_blend || context->stage_state[D3DTSS_COLOROP] != D3DTOP_DISABLE);
    i = 1;
#else
    (void)fvf;
#endif
    if (!triangles) return DD_OK;
    if (!vertices) return DDERR_INVALIDPARAMS;
#ifndef WGPU_NT_ENABLE_MATERIAL
    /* The fixed primitive batch clips against the whole target. */
    if (!whole_viewport) {
        D3D_TRACE(phase, 5);
        D3D_TRACE(state, context->viewport.dwWidth | (context->viewport.dwHeight << 16));
        D3D_TRACE(value, target->width | (target->height << 16));
        return DDERR_UNSUPPORTED;
    }
#endif
    for (; i < sizeof(context->textures) / sizeof(context->textures[0]); ++i) {
        if (!context->textures[i]) continue;
        D3D_TRACE(phase, 6);
        D3D_TRACE(state, i);
        D3D_TRACE(value, context->textures[i]);
        return DDERR_UNSUPPORTED;
    }
    if (first > vertex_count || (!indices &&
        (primitive == D3DPT_TRIANGLELIST ? triangles * 3 : triangles + 2) > vertex_count - first))
        return DDERR_INVALIDPARAMS;
    if (indices)
        for (i = 0; i < triangles * 3; ++i)
            if (wgpu_d3d_vertex_index(indices, index_record_bytes, primitive, i) >= vertex_count - first)
                return DDERR_INVALIDPARAMS;
#ifdef WGPU_D3D_DIAGNOSTICS
    if (!indices) {
        const BYTE *vertex = vertices + first * stride;
        D3D_TRACE(unindexed_calls, d3d_trace.unindexed_calls + 1);
        D3D_TRACE(unindexed_first, first);
        D3D_TRACE(unindexed_stride, stride);
        D3D_TRACE(unindexed_offset, d3d_trace.vertex_offset);
        D3D_TRACE(unindexed_vertex_count, vertex_count);
        memcpy(&d3d_trace.unindexed_x, vertex, sizeof(DWORD));
        memcpy(&d3d_trace.unindexed_y, vertex + sizeof(DWORD), sizeof(DWORD));
        memcpy(&d3d_trace.unindexed_argb, vertex + 4 * sizeof(DWORD), sizeof(DWORD));
    }
#endif
#ifdef WGPU_NT_ENABLE_MATERIAL
    if (textured || context->fragment_flags || !whole_viewport
#ifdef WGPU_NT_ENABLE_DEPTH
        || context->depth_enabled
#endif
        ) {
        WgpuNtFill clipped = *target;
        const D3DHAL_DP2VIEWPORTINFO *viewport = &context->viewport;
        if (!viewport->dwWidth || !viewport->dwHeight ||
            viewport->dwX >= target->width || viewport->dwY >= target->height ||
            viewport->dwWidth > target->width - viewport->dwX ||
            viewport->dwHeight > target->height - viewport->dwY)
            return DDERR_INVALIDPARAMS;
        clipped.left = viewport->dwX;
        clipped.top = viewport->dwY;
        clipped.right = clipped.left + viewport->dwWidth;
        clipped.bottom = clipped.top + viewport->dwHeight;
        return wgpu_d3d_draw_material(pd, context, &clipped, vertices + first * stride,
                                      stride, fvf, triangles, indices, index_record_bytes, primitive,
                                      textured);
    }
#endif

    memset(&draw, 0, sizeof(draw));
    draw.target = *target;
    draw.size = sizeof(draw);
    while (triangles) {
        HRESULT result;
        DWORD count = triangles;
        if (count > WGPU_NT_DRAW_MAX_VERTICES / 3) count = WGPU_NT_DRAW_MAX_VERTICES / 3;
        draw.vertex_count = count * 3;
        for (i = 0; i < draw.vertex_count; ++i) {
            DWORD offset = wgpu_d3d_vertex_index(indices, index_record_bytes, primitive, start + i);
            const BYTE *vertex = vertices + (first + offset) * stride;
            memcpy(&draw.vertices[i].x, vertex, 2 * sizeof(DWORD));
            memcpy(&draw.vertices[i].argb, vertex + 4 * sizeof(DWORD), sizeof(DWORD));
            if (context->shade == D3DSHADE_FLAT)
                draw.vertices[i].argb = draw.vertices[i - i % 3].argb;
        }
        if (!wgpu_nt_draw_valid(&draw, pd->vram, 0x1000ul))
            return DDERR_INVALIDPARAMS;
        draw.vertex_count = wgpu_d3d_cull(context->cull, draw.vertices,
                                         draw.vertex_count, sizeof(draw.vertices[0]));
        if (draw.vertex_count) {
            WgpuNtFill damage;
            const WgpuNtFill *flush = wgpu_d3d_draw_damage(&damage, target, draw.vertices,
                                            draw.vertex_count, sizeof(draw.vertices[0]));
#ifdef WGPU_NT_ENABLE_GROUPS
            result = wgpu_d3d_group_flush(pd);
            if (result != DD_OK) return result;
#endif
            result = wgpu_d3d_submit(pd, WGPU_DISPLAY_DRAW_IOCTL, flush, &draw, sizeof(draw));
            if (result != DD_OK) return result;
        }
        start += count * 3;
        triangles -= count;
    }
    return DD_OK;
}

#ifdef WGPU_NT_ENABLE_DEPTH
static DWORD wgpu_d3d_coordinate(DWORD value)
{
    DWORD exponent;
    if (!value) return 0;
    exponent = 31 - __builtin_clz(value);
    return ((127 + exponent) << 23) | ((value << (23 - exponent)) & 0x7fffff);
}

static HRESULT wgpu_d3d_clear_depth(PDEV *pd, const WgpuD3dContext *context,
                                     const WgpuNtFill *rectangle, DWORD depth_bits,
                                     BOOL write_color)
{
    static const BYTE corners[] = {0, 1, 2, 1, 3, 2};
    WgpuNtMaterialDraw *draw = &d3d_material_scratch;
#ifdef WGPU_NT_SKIP_DEPTH_CLEAR_COLOR_FLUSH
    /* Tiled depth has its own cache barriers. */
    const WgpuNtFill *color_cache = write_color ? &draw->target : NULL;
#else
    const WgpuNtFill *color_cache = &draw->target;
#endif
    HRESULT result = DDERR_INVALIDPARAMS;
    DWORD i, x[2], y[2], argb = 0;
    memset(draw, 0, WGPU_NT_MATERIAL_HEADER_BYTES + sizeof(corners) * sizeof(draw->vertices[0]));
    draw->size = sizeof(*draw);
    draw->vertex_count = sizeof(corners);
    draw->target = *rectangle;
    draw->target.color = 0;
    if (!wgpu_depth_material(pd, context->depth, draw,
        WGPU_NT_DEPTH_TEST | WGPU_NT_DEPTH_WRITE | (write_color ? 0 : WGPU_NT_DEPTH_ONLY),
        D3DCMP_ALWAYS)) goto done;
    if (write_color) {
        /* Expand the already-quantized RGB565 components to stable UNORM
         * inputs. The render target quantizes them back to the same bits. */
        const DWORD r = (rectangle->color >> 11) & 31;
        const DWORD g = (rectangle->color >> 5) & 63;
        const DWORD b = rectangle->color & 31;
        argb = (((r << 3) | (r >> 2)) << 16) |
               (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
    }
    x[0] = wgpu_d3d_coordinate(rectangle->left);
    x[1] = wgpu_d3d_coordinate(rectangle->right);
    y[0] = wgpu_d3d_coordinate(rectangle->top);
    y[1] = wgpu_d3d_coordinate(rectangle->bottom);
    for (i = 0; i < draw->vertex_count; ++i) {
        draw->vertices[i].x = x[corners[i] & 1];
        draw->vertices[i].y = y[corners[i] >> 1];
        draw->vertices[i].z = depth_bits;
        draw->vertices[i].rhw = 0x3f800000;
        draw->vertices[i].diffuse = argb;
    }
    if (wgpu_nt_material_valid(draw, pd->vram, 0x1000ul))
        result = wgpu_d3d_submit(pd, WGPU_DISPLAY_MATERIAL_IOCTL,
                                 color_cache, draw, wgpu_nt_material_bytes(draw->vertex_count));
done:
    return result;
}
#endif

static HRESULT wgpu_d3d_clear_rect(PDEV *pd, const WgpuD3dContext *context,
    WgpuNtFill *fill, DWORD flags, DWORD depth_bits)
{
    if (!wgpu_surface_fits(pd, fill)) return DDERR_INVALIDPARAMS;
#ifdef WGPU_NT_PROFILE_BARRIERS
    if (flags == (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER)) ++d3d_barriers.combined_clears;
    else if (flags == D3DCLEAR_ZBUFFER) ++d3d_barriers.depth_clears;
    else if (flags == D3DCLEAR_TARGET) ++d3d_barriers.color_clears;
#endif
#ifdef WGPU_NT_ENABLE_DEPTH
#ifdef WGPU_NT_COMBINED_CLEAR_565
    if (flags == (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER) && fill->bpp == 16)
        return wgpu_d3d_clear_depth(pd, context, fill, depth_bits, TRUE);
#endif
    if (flags & D3DCLEAR_ZBUFFER) {
        HRESULT result = wgpu_d3d_clear_depth(pd, context, fill, depth_bits, FALSE);
        if (result != DD_OK) return result;
    }
#else
    (void)context;
    (void)depth_bits;
#endif
    if (flags & D3DCLEAR_TARGET)
        return wgpu_d3d_submit(pd, WGPU_DISPLAY_FILL_IOCTL, fill, fill, sizeof(*fill));
    return DD_OK;
}

static HRESULT wgpu_d3d_clear(PDEV *pd, const WgpuD3dContext *context,
                              WgpuNtFill fill, const BYTE *payload, DWORD count)
{
    DWORD flags, color, i, depth_bits = 0;
    memcpy(&flags, payload, sizeof(flags));
    memcpy(&color, payload + sizeof(flags), sizeof(color));
    D3D_TRACE(clear_commands, d3d_trace.clear_commands + 1);
    D3D_TRACE(clear_rects, count);
    D3D_TRACE(clear_flags, flags);
#ifdef WGPU_NT_ENABLE_DEPTH
    if (!flags || (flags & ~(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER))) return DDERR_UNSUPPORTED;
    memcpy(&depth_bits, payload + offsetof(D3DHAL_DP2CLEAR, dvFillDepth), sizeof(depth_bits));
#else
    if (flags != D3DCLEAR_TARGET) return DDERR_UNSUPPORTED;
#endif
    fill.color = fill.bpp == 16 ?
        ((color >> 8) & 0xf800) | ((color >> 5) & 0x07e0) | ((color >> 3) & 0x1f) :
        color & 0xffffff;
    if (!count) {
        const D3DHAL_DP2VIEWPORTINFO *viewport = &context->viewport;
        if (viewport->dwX >= fill.width || viewport->dwY >= fill.height ||
            viewport->dwWidth > fill.width - viewport->dwX ||
            viewport->dwHeight > fill.height - viewport->dwY) return DDERR_INVALIDPARAMS;
        fill.left = viewport->dwX;
        fill.top = viewport->dwY;
        fill.right = fill.left + viewport->dwWidth;
        fill.bottom = fill.top + viewport->dwHeight;
        return wgpu_d3d_clear_rect(pd, context, &fill, flags, depth_bits);
    }
    payload += offsetof(D3DHAL_DP2CLEAR, Rects);
    for (i = 0; i < count; ++i) {
        RECT rect;
        HRESULT result;
        memcpy(&rect, payload + i * sizeof(rect), sizeof(rect));
        fill.left = rect.left;
        fill.top = rect.top;
        fill.right = rect.right;
        fill.bottom = rect.bottom;
        result = wgpu_d3d_clear_rect(pd, context, &fill, flags, depth_bits);
        if (result != DD_OK) return result;
    }
    return DD_OK;
}

static HRESULT wgpu_d3d_commands(PDEV *pd, WgpuD3dContext *context,
    WgpuNtFill *target, const BYTE *commands, WgpuD3dVertices *vertices,
    LPD3DHAL_DRAWPRIMITIVES2DATA data)
{
    DWORD offset = 0;
    while (offset < data->dwCommandLength) {
        D3DHAL_DP2COMMAND command;
        DWORD bytes, i, count, inline_offset = 0, inline_stride = 0;
        const BYTE *payload;
        HRESULT result = DD_OK;
        BOOL geometry = FALSE;
        data->dwErrorOffset = offset;
        if (sizeof(command) > data->dwCommandLength - offset) return DDERR_INVALIDPARAMS;
        memcpy(&command, commands + offset, sizeof(command));
        D3D_TRACE(opcode, command.bCommand);
        offset += sizeof(command);
        payload = commands + offset;
        count = command.wPrimitiveCount;
        switch (command.bCommand) {
        case D3DDP2OP_RENDERSTATE:
            bytes = count * sizeof(D3DHAL_DP2RENDERSTATE);
            break;
        case D3DDP2OP_TEXTURESTAGESTATE:
            bytes = count * sizeof(D3DHAL_DP2TEXTURESTAGESTATE);
            break;
        case D3DDP2OP_VIEWPORTINFO:
            bytes = count * sizeof(D3DHAL_DP2VIEWPORTINFO);
            break;
        case D3DDP2OP_WINFO:
            bytes = count * sizeof(D3DHAL_DP2WINFO);
            break;
        case D3DDP2OP_SETRENDERTARGET:
            bytes = count * sizeof(D3DHAL_DP2SETRENDERTARGET);
            break;
#ifdef WGPU_NT_ENABLE_MATERIAL
        case D3DDP2OP_TEXBLT:
            bytes = count * sizeof(D3DHAL_DP2TEXBLT);
            break;
#endif
        case D3DDP2OP_TRIANGLELIST:
            bytes = sizeof(D3DHAL_DP2TRIANGLELIST);
            geometry = TRUE;
            break;
        case D3DDP2OP_TRIANGLESTRIP:
            bytes = sizeof(D3DHAL_DP2TRIANGLESTRIP);
            geometry = TRUE;
            break;
        case D3DDP2OP_TRIANGLEFAN:
            bytes = sizeof(D3DHAL_DP2TRIANGLEFAN);
            geometry = TRUE;
            break;
        case D3DDP2OP_TRIANGLEFAN_IMM:
            inline_stride = wgpu_d3d_vertex_stride(data);
            if (!inline_stride) return DDERR_INVALIDPARAMS;
            /* Edge flags precede DWORD-aligned vertices in the command
             * buffer. The HAL supports solid fill only. */
            inline_offset = sizeof(D3DHAL_DP2TRIANGLEFAN_IMM);
            inline_offset += (0u - (data->dwCommandOffset + offset + inline_offset)) & 3u;
            bytes = inline_offset + (count + 2) * inline_stride;
            break;
        case D3DDP2OP_INDEXEDTRIANGLESTRIP:
        case D3DDP2OP_INDEXEDTRIANGLEFAN:
            bytes = (count + 2) * sizeof(WORD);
            geometry = TRUE;
            break;
        case D3DDP2OP_INDEXEDTRIANGLELIST:
            bytes = count * sizeof(D3DHAL_DP2INDEXEDTRIANGLELIST);
            geometry = TRUE;
            break;
        case D3DDP2OP_INDEXEDTRIANGLELIST2:
            bytes = sizeof(D3DHAL_DP2STARTVERTEX) + count * sizeof(D3DHAL_DP2INDEXEDTRIANGLELIST2);
            geometry = TRUE;
            break;
        case D3DDP2OP_CLEAR:
            bytes = offsetof(D3DHAL_DP2CLEAR, Rects) + count * sizeof(RECT);
            break;
        default:
            return D3DERR_COMMAND_UNPARSED;
        }
        if (bytes > data->dwCommandLength - offset) return DDERR_INVALIDPARAMS;
        if (geometry && count && !vertices->buffer && !wgpu_d3d_read_vertices(data, vertices)) {
            D3D_TRACE(phase, 7);
            return DDERR_INVALIDPARAMS;
        }
#ifdef WGPU_NT_ENABLE_GROUPS
#ifdef WGPU_NT_PROFILE_BARRIERS
        BOOL pending_bind = command.bCommand == D3DDP2OP_SETRENDERTARGET &&
            (d3d_group.packet.count || d3d_pipeline_device);
#endif
        d3d_group_command = data->dwErrorOffset;
        if (command.bCommand == D3DDP2OP_SETRENDERTARGET ||
            command.bCommand == D3DDP2OP_TEXBLT || command.bCommand == D3DDP2OP_CLEAR) {
            result = wgpu_d3d_group_flush(pd);
            if (result != DD_OK) return result;
        }
#endif
        switch (command.bCommand) {
        case D3DDP2OP_RENDERSTATE:
            for (i = 0; i < count; ++i) {
                D3DHAL_DP2RENDERSTATE state;
                memcpy(&state, payload + i * sizeof(state), sizeof(state));
                D3D_TRACE(state, state.RenderState);
                D3D_TRACE(value, state.dwState);
#ifdef WGPU_NT_SCENE_GROUPS
                if (state.RenderState == D3DRENDERSTATE_SCENECAPTURE) {
                    result = wgpu_d3d_group_flush(pd);
                    if (result != DD_OK) return result;
                }
#endif
                if (!wgpu_d3d_render_state(context, state.RenderState, state.dwState)) {
#ifdef WGPU_D3D_DIAGNOSTICS
                    if (!d3d_trace.init_state_count) {
                        DWORD remaining = count - i;
                        if (remaining > WGPU_D3D_TRACE_STATES) remaining = WGPU_D3D_TRACE_STATES;
                        d3d_trace.init_state_count = remaining;
                        memcpy(d3d_trace.init_states, payload + i * sizeof(state),
                               remaining * sizeof(state));
                    }
#endif
                    return DDERR_UNSUPPORTED;
                }
            }
            break;
        case D3DDP2OP_TEXTURESTAGESTATE:
            for (i = 0; i < count; ++i) {
                D3DHAL_DP2TEXTURESTAGESTATE state;
                memcpy(&state, payload + i * sizeof(state), sizeof(state));
                D3D_TRACE(state, state.TSState);
                D3D_TRACE(value, state.dwValue);
                if (state.wStage >= sizeof(context->textures) / sizeof(context->textures[0]))
                    return DDERR_INVALIDPARAMS;
                if (state.TSState == D3DTSS_TEXTUREMAP)
                    context->textures[state.wStage] = state.dwValue;
#ifdef WGPU_NT_ENABLE_MATERIAL
                else if (state.wStage == 0 && state.TSState <
                         sizeof(context->stage_state) / sizeof(context->stage_state[0])) {
                    context->stage_state[state.TSState] = state.dwValue;
                    if (state.TSState >= D3DTSS_COLOROP && state.TSState <= D3DTSS_ALPHAARG2)
                        context->legacy_blend = 0;
                }
#endif
                /* Texture sampling states have no effect while no stage
                 * has a texture. Draws with a bound texture are refused. */
            }
            break;
        case D3DDP2OP_VIEWPORTINFO:
            if (count) memcpy(&context->viewport, payload + bytes - sizeof(context->viewport),
                              sizeof(context->viewport));
            break;
        case D3DDP2OP_SETRENDERTARGET:
            for (i = 0; i < count; ++i) {
                D3DHAL_DP2SETRENDERTARGET request;
                PDD_SURFACE_LOCAL surface;
                WgpuNtFill next_target;
                memcpy(&request, payload + i * sizeof(request), sizeof(request));
                D3D_TRACE(phase, 12);
                D3D_TRACE(state, request.hRenderTarget);
                D3D_TRACE(value, request.hZBuffer);
#ifndef WGPU_NT_ENABLE_DEPTH
                if (request.hZBuffer) return DDERR_UNSUPPORTED;
#endif
                surface = wgpu_d3d_surface(context->draw, request.hRenderTarget);
                if (!surface) return DDERR_INVALIDPARAMS;
                D3D_TRACE(phase, 13);
                if (!wgpu_d3d_render_target(pd, surface, &next_target))
                    return DDERR_INVALIDPARAMS;
#ifdef WGPU_NT_ENABLE_DEPTH
                if (request.hZBuffer) {
                    WgpuD3dDepth depth;
                    PDD_SURFACE_LOCAL local = wgpu_d3d_surface(context->draw, request.hZBuffer);
                    D3D_TRACE(phase, 14);
                    if (!local) return DDERR_INVALIDPARAMS;
                    D3D_TRACE(phase, 15);
                    if (!wgpu_depth_surface(pd, local, &depth) ||
                        depth.width < next_target.width || depth.height < next_target.height)
                        return DDERR_INVALIDPARAMS;
#ifdef WGPU_NT_PROFILE_BARRIERS
                    if (pending_bind && !i && surface == context->target && local == context->depth)
                        ++d3d_barriers.pending_same_surface_binds;
#endif
                    context->depth = local;
                } else {
#ifdef WGPU_NT_PROFILE_BARRIERS
                    if (pending_bind && !i && surface == context->target && !context->depth)
                        ++d3d_barriers.pending_same_surface_binds;
#endif
                    context->depth = NULL;
                }
#endif
#ifdef WGPU_NT_PROFILE_BARRIERS
                ++d3d_barriers.target_binds;
                d3d_barriers.pending_binds += pending_bind && !i;
#endif
                *target = next_target;
                context->target = surface;
            }
            break;
#ifdef WGPU_NT_ENABLE_MATERIAL
        case D3DDP2OP_TEXBLT:
            for (i = 0; i < count; ++i) {
                D3DHAL_DP2TEXBLT texture_blt;
                memcpy(&texture_blt, payload + i * sizeof(texture_blt), sizeof(texture_blt));
                result = wgpu_d3d_texture_blt(pd, context, &texture_blt);
                if (result != DD_OK) break;
            }
            break;
#endif
        case D3DDP2OP_TRIANGLELIST:
        case D3DDP2OP_TRIANGLESTRIP:
        case D3DDP2OP_TRIANGLEFAN: {
            WORD first;
            D3DPRIMITIVETYPE primitive = command.bCommand == D3DDP2OP_TRIANGLELIST ?
                D3DPT_TRIANGLELIST : command.bCommand == D3DDP2OP_TRIANGLESTRIP ?
                D3DPT_TRIANGLESTRIP : D3DPT_TRIANGLEFAN;
            memcpy(&first, payload, sizeof(first));
            result = wgpu_d3d_draw_triangles(pd, context, target, vertices->buffer, vertices->stride,
                data->dwVertexType, first, vertices->count, count, NULL, 0, primitive);
            break;
        }
        case D3DDP2OP_TRIANGLEFAN_IMM:
            result = wgpu_d3d_draw_triangles(pd, context, target, payload + inline_offset,
                inline_stride, data->dwVertexType, 0, count + 2, count,
                NULL, 0, D3DPT_TRIANGLEFAN);
            break;
        case D3DDP2OP_INDEXEDTRIANGLESTRIP:
        case D3DDP2OP_INDEXEDTRIANGLEFAN:
            result = wgpu_d3d_draw_triangles(pd, context, target, vertices->buffer, vertices->stride,
                data->dwVertexType, 0, vertices->count, count, payload, sizeof(WORD),
                command.bCommand == D3DDP2OP_INDEXEDTRIANGLESTRIP ?
                D3DPT_TRIANGLESTRIP : D3DPT_TRIANGLEFAN);
            break;
        case D3DDP2OP_INDEXEDTRIANGLELIST:
            result = wgpu_d3d_draw_triangles(pd, context, target, vertices->buffer, vertices->stride,
                data->dwVertexType, 0, vertices->count, count, payload,
                sizeof(D3DHAL_DP2INDEXEDTRIANGLELIST), D3DPT_TRIANGLELIST);
            break;
        case D3DDP2OP_INDEXEDTRIANGLELIST2: {
            D3DHAL_DP2STARTVERTEX first;
            memcpy(&first, payload, sizeof(first));
            result = wgpu_d3d_draw_triangles(pd, context, target, vertices->buffer, vertices->stride,
                data->dwVertexType, first.wVStart, vertices->count, count, payload + sizeof(first),
                sizeof(D3DHAL_DP2INDEXEDTRIANGLELIST2), D3DPT_TRIANGLELIST);
            break;
        }
        case D3DDP2OP_CLEAR: {
#ifdef WGPU_NT_PROFILE_BARRIERS
            const unsigned long long start = wgpu_d3d_ticks();
#endif
            result = wgpu_d3d_clear(pd, context, *target, payload, count);
#ifdef WGPU_NT_PROFILE_BARRIERS
            d3d_barriers.clear_ticks += wgpu_d3d_ticks() - start;
#endif
            D3D_TRACE(clear_status, result);
            break;
        }
        }
        if (result != DD_OK) return result;
        offset += bytes;
    }
    data->dwErrorOffset = 0;
    return DD_OK;
}

static DWORD WINAPI wgpu_d3d_draw(LPD3DHAL_DRAWPRIMITIVES2DATA data)
{
    HRESULT result = DDERR_INVALIDPARAMS;
    BYTE *commands = NULL;
    WgpuD3dVertices vertices = {0};
    WgpuD3dContext *context = NULL;
    PDEV *pd = NULL;
    WgpuNtFill target;
#ifdef WGPU_NT_SCENE_PIPELINE
    ULONG submit_threshold = WGPU_NT_MATERIAL_GROUP_MAX / 2;
#endif
#ifdef WGPU_NT_SCENE_GROUPS
    BOOL can_defer = FALSE;
#endif
    data->dwErrorOffset = 0;
    EngAcquireSemaphore(d3d_lock);
#if defined(WGPU_NT_PIPELINE) && !defined(WGPU_NT_SCENE_PIPELINE)
    if (d3d_pipeline_device && wgpu_d3d_retire_pipeline() != DD_OK) {
        EngReleaseSemaphore(d3d_lock);
        data->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
#endif
#ifdef WGPU_NT_ENABLE_GROUPS
    if (!d3d_group.packet.count) d3d_group.packet.size = sizeof(d3d_group.packet);
    d3d_group_error_offset = &data->dwErrorOffset;
#endif
    D3D_TIMER(draw_start);
    D3D_TIMING_COUNT(draw_callbacks);
    D3D_TRACE(dp2_calls, d3d_trace.dp2_calls + 1);
    D3D_TRACE(phase, 1);
    D3D_TRACE(opcode, 0);
    D3D_TRACE(state, 0);
    D3D_TRACE(value, 0);
    D3D_TRACE(command_offset, data->dwCommandOffset);
    D3D_TRACE(command_bytes, data->dwCommandLength);
    D3D_TRACE(vertex_type, data->dwVertexType);
    D3D_TRACE(vertex_count, data->dwVertexLength);
    D3D_TRACE(vertex_stride, data->dwVertexSize);
    D3D_TRACE(vertex_caps, 0);
    D3D_TRACE(vertex_capacity, 0);
    D3D_TRACE(vertex_offset, data->dwVertexOffset);
    D3D_TRACE(available_vertices, 0);
    D3D_TRACE(flags, data->dwFlags);
    /* The initial profile uses DrawPrimitive with user-memory vertices
     * or a system-memory surface. Execute buffer state is a separate DDI.
     */
    if (data->dwFlags & D3DHALDP2_EXECUTEBUFFER) {
        result = DDERR_UNSUPPORTED;
        goto done;
    }
    context = wgpu_d3d_context(data->dwhContext);
    if (!context || !context->draw->lpGbl || !context->target) goto done;
    pd = context->draw->lpGbl->dhpdev;
    if (!wgpu_d3d_render_target(pd, context->target, &target)) goto done;
#ifdef WGPU_NT_SCENE_PIPELINE
    if (d3d_pipeline_device && (d3d_pipeline_context != context || !context->scene_open ||
        !wgpu_d3d_pipeline_target_matches(pd, &target) ||
        !wgpu_d3d_hidden_target(pd, context, &target))) {
        result = wgpu_d3d_group_flush(d3d_group_device ? d3d_group_device : d3d_pipeline_device);
        if (result != DD_OK) goto done;
    }
#endif
#ifdef WGPU_NT_SCENE_GROUPS
    if (d3d_group_device && (d3d_group_context != context || d3d_group_device != pd)) {
        result = wgpu_d3d_group_flush(d3d_group_device);
        if (result != DD_OK) goto done;
    }
#endif
    D3D_TRACE(phase, 2);
    if (data->dwCommandLength) {
        D3D_TRACE(phase, 3);
        D3D_TRACE(command_caps, data->lpDDCommands ? data->lpDDCommands->ddsCaps.dwCaps : 0);
        D3D_TRACE(command_capacity, data->lpDDCommands && data->lpDDCommands->lpGbl ?
                  data->lpDDCommands->lpGbl->dwLinearSize : 0);
        commands = wgpu_d3d_surface_data(data->lpDDCommands, data->dwCommandOffset,
                                         data->dwCommandLength, WGPU_COMMAND_SNAPSHOT);
        if (!commands) goto done;
        D3D_TRACE(phase, 4);
        result = wgpu_d3d_commands(pd, context, &target, commands, &vertices, data);
    } else {
        result = DD_OK;
    }
done:
#ifdef WGPU_NT_ENABLE_GROUPS
#ifdef WGPU_NT_PIPELINE
#ifdef WGPU_NT_SCENE_PIPELINE
    /* Start enough work to keep the GPU busy while the next DP2 call
     * builds. Short runs continue to merge without a transport request. */
#ifdef WGPU_NT_ADAPTIVE_GROUPS
    submit_threshold = d3d_pipeline_device ?
        WGPU_NT_MATERIAL_GROUP_MAX * 3 / 4 : WGPU_NT_MATERIAL_GROUP_MAX / 4;
#endif
    if (result == DD_OK && pd && context->scene_open &&
        d3d_group.packet.count >= submit_threshold &&
        wgpu_d3d_hidden_target(pd, context, &target)) {
        result = wgpu_d3d_group_send(pd, TRUE);
    }
    if (d3d_pipeline_device && result == DD_OK && pd && context->scene_open &&
        wgpu_d3d_pipeline_target_matches(pd, &target) &&
        wgpu_d3d_hidden_target(pd, context, &target)) {
        d3d_pipeline_context = context;
    } else
#else
    /* A DP2 call may return its vertex buffers and expose the target.
     * Keep the first physical candidate's in-flight lifetime within DP2. */
#endif
    if (d3d_pipeline_device) {
        PDEV *owner = d3d_group_device ? d3d_group_device : d3d_pipeline_device;
        HRESULT queued = wgpu_d3d_group_flush(owner);
        if (queued != DD_OK) result = queued;
    }
#endif
#ifdef WGPU_NT_SCENE_GROUPS
    if (result == DD_OK && pd && context->scene_open && d3d_group.packet.count) {
        can_defer = wgpu_d3d_hidden_target(pd, context, &target);
        D3D_TIMING_ADD(visible_group_returns, !can_defer);
#ifdef WGPU_NT_PROFILE_TARGETS
        if (!can_defer) wgpu_d3d_profile_target(pd, context, &target);
#ifdef WGPU_NT_GDI_PIPELINE
        else if (!target.offset) ++d3d_target_stats.deferred_gdi_primary;
#endif
#endif
    }
    if (can_defer) {
        D3D_TIMING_COUNT(scene_group_holds);
        d3d_group_device = pd;
        d3d_group_context = context;
        d3d_group_first = 0; /* The original command buffer has been returned. */
    } else if ((pd || d3d_group_device)
#ifdef WGPU_NT_SCENE_PIPELINE
        && (d3d_group.packet.count || !d3d_pipeline_context)
#endif
        ) {
        D3D_TIMING_ADD(end_groups, d3d_group.packet.count != 0);
        HRESULT queued = wgpu_d3d_group_flush(d3d_group_device ? d3d_group_device : pd);
        if (queued != DD_OK) result = queued;
    }
#else
    if (pd) {
        D3D_TIMING_ADD(end_groups, d3d_group.packet.count != 0);
        HRESULT queued = wgpu_d3d_group_flush(pd);
        if (queued != DD_OK) result = queued;
    }
#endif
    d3d_group_error_offset = NULL;
#endif
    D3D_TRACE(dp2_status, result);
    if (result != DD_OK) {
        D3D_TRACE(rejected_batches, d3d_trace.rejected_batches + 1);
        D3D_TRACE(last_rejected_status, result);
        D3D_TRACE(last_rejected_opcode, d3d_trace.opcode);
        D3D_TRACE(rejected_phase, d3d_trace.phase);
        D3D_TRACE(rejected_fvf, data->dwVertexType);
        D3D_TRACE(rejected_vertices, data->dwVertexLength);
        D3D_TRACE(rejected_cmd_bytes, data->dwCommandLength);
        D3D_TRACE(rejected_state, d3d_trace.state);
        D3D_TRACE(rejected_value, d3d_trace.value);
    }
    if (commands
#ifdef WGPU_NT_FAST_STREAM
        && commands != (BYTE *)d3d_short_commands
#endif
        ) EngFreeMem(commands);
    if (vertices.buffer
#ifdef WGPU_NT_FAST_STREAM
        && vertices.buffer != (BYTE *)d3d_short_vertices
#endif
        ) EngFreeMem(vertices.buffer);
    D3D_ELAPSED(draw_ticks, draw_start);
    EngReleaseSemaphore(d3d_lock);
    data->ddrval = result;
    return DDHAL_DRIVER_HANDLED;
}

static const GUID d3d_callbacks3 = {
    0xddf41230, 0xec0a, 0x11d0, { 0xa9, 0xb6, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e }
};
static const GUID d3d_misc2 = {
    0x406b2f00, 0x3e5a, 0x11d1, { 0xb6, 0x40, 0x00, 0xaa, 0x00, 0xa1, 0xf9, 0x6a }
};
static const GUID d3d_extended_caps = {
    0x7de41f80, 0x9d93, 0x11d0, { 0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29 }
};
static const GUID d3d_parse_unknown_guid = {
    0x2e04ffa0, 0x98e4, 0x11d1, { 0x8c, 0xe1, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 }
};

static BOOL wgpu_d3d_equal_guid(const GUID *a, const GUID *b)
{
    unsigned i;
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3)
        return FALSE;
    for (i = 0; i < sizeof(a->Data4); ++i)
        if (a->Data4[i] != b->Data4[i]) return FALSE;
    return TRUE;
}
static D3DHAL_CALLBACKS d3d_callbacks;
static D3DHAL_GLOBALDRIVERDATA d3d_caps = {
    .dwSize = sizeof(D3DHAL_GLOBALDRIVERDATA),
    .hwCaps = {
        .dwSize = sizeof(D3DDEVICEDESC_V1),
        .dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRICAPS |
                   D3DDD_DEVICERENDERBITDEPTH
#ifdef WGPU_NT_ENABLE_DEPTH
                   | D3DDD_DEVICEZBUFFERBITDEPTH
#endif
                   ,
        .dcmColorModel = D3DCOLOR_RGB,
        .dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                     D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_DRAWPRIMITIVES2 |
                     D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWRASTERIZATION
#ifdef WGPU_NT_ENABLE_MATERIAL
                     | D3DDEVCAPS_TEXTUREVIDEOMEMORY
#endif
                     ,
        .dtcTransformCaps = { .dwSize = sizeof(D3DTRANSFORMCAPS) },
        .dlcLightingCaps = { .dwSize = sizeof(D3DLIGHTINGCAPS) },
        .dpcLineCaps = { .dwSize = sizeof(D3DPRIMCAPS) },
        .dpcTriCaps = {
            .dwSize = sizeof(D3DPRIMCAPS),
            .dwMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW,
            .dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB
#ifdef WGPU_NT_ENABLE_MATERIAL
                | D3DPSHADECAPS_SPECULARFLATRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB
#endif
                ,
#ifdef WGPU_NT_ENABLE_DEPTH
            .dwZCmpCaps = 0xff,
#endif
#ifdef WGPU_NT_ENABLE_MATERIAL
            .dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA,
            .dwAlphaCmpCaps = 0xff,
            .dwSrcBlendCaps = 0x7ff,
            .dwDestBlendCaps = 0x3ff,
            .dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR |
                D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR
#ifdef WGPU_NT_ENABLE_MIPS
                | D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR
#endif
                ,
            .dwTextureBlendCaps = D3DPTBLENDCAPS_COPY | D3DPTBLENDCAPS_DECAL |
                D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_MODULATEALPHA |
                D3DPTBLENDCAPS_DECALALPHA | D3DPTBLENDCAPS_ADD,
            .dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_CLAMP,
#endif
        },
        .dwDeviceRenderBitDepth = DDBD_16 | DDBD_32,
#ifdef WGPU_NT_ENABLE_DEPTH
        .dwDeviceZBufferBitDepth = DDBD_16,
#endif
        .dwMaxVertexCount = 0xffff
    },
#ifdef WGPU_NT_ENABLE_MATERIAL
    .dwNumTextureFormats = sizeof(wgpu_texture_formats) / sizeof(wgpu_texture_formats[0]),
    .lpTextureFormats = wgpu_texture_formats,
#endif
};

int d3dnt_init(void)
{
    d3d_lock = EngCreateSemaphore();
    if (!d3d_lock) return 0;
    d3d_callbacks.dwSize = sizeof(d3d_callbacks);
    d3d_callbacks.ContextCreate = wgpu_d3d_create;
    d3d_callbacks.ContextDestroy = wgpu_d3d_destroy;
    d3d_callbacks.ContextDestroyAll = wgpu_d3d_destroy_all;
    return 1;
}

void d3dnt_mode(void *fb, ULONG vram, ULONG bpp, ULONG r, ULONG g, ULONG b, int on)
{
    d3d_mode_supported = on && fb && vram && wgpu_d3d_format(bpp, r, g, b);
}

void d3dnt_halinfo(DD_HALINFO *info)
{
    if (!d3d_mode_supported) return;
    info->ddCaps.dwCaps |= DDCAPS_3D;
#ifdef WGPU_NT_ENABLE_DEPTH
    info->ddCaps.dwCaps |= DDCAPS_BLTDEPTHFILL;
    info->ddCaps.ddsCaps.dwCaps |= DDSCAPS_ZBUFFER;
    info->ddCaps.dwZBufferBitDepths = DDBD_16;
    info->vmiData.dwZBufferAlign = 64;
#endif
#ifdef WGPU_NT_ENABLE_MATERIAL
    info->ddCaps.ddsCaps.dwCaps |= DDSCAPS_TEXTURE | DDSCAPS_MIPMAP;
    info->vmiData.dwTextureAlign = 64;
#ifdef WGPU_NT_ENABLE_MIPS
    info->ddCaps.ddsCaps.dwCaps |= DDSCAPS_COMPLEX;
#endif
#endif
    info->lpD3DGlobalDriverData = &d3d_caps;
    info->lpD3DHALCallbacks = &d3d_callbacks;
}

static int wgpu_d3d_return_info(PDD_GETDRIVERINFODATA data, const void *info, ULONG bytes)
{
    ULONG copy = data->dwExpectedSize < bytes ? data->dwExpectedSize : bytes;
    if (copy) memcpy(data->lpvData, info, copy);
    data->dwActualSize = bytes;
    data->ddRVal = DD_OK;
    return 1;
}

#ifdef WGPU_NT_SCENE_PIPELINE
static DWORD WINAPI wgpu_d3d_flip_to_gdi(PDD_FLIPTOGDISURFACEDATA data)
{
    data->ddRVal = wgpu_d3d_drain();
    return DDHAL_DRIVER_HANDLED;
}

static DWORD WINAPI wgpu_d3d_exclusive_mode(PDD_SETEXCLUSIVEMODEDATA data)
{
    data->ddRVal = wgpu_d3d_drain();
    return DDHAL_DRIVER_HANDLED;
}
#endif

int d3dnt_info(PDD_GETDRIVERINFODATA data)
{
#ifdef WGPU_NT_ENABLE_DEPTH
    static const GUID depth_formats_guid = {
        0x93869880, 0x36cf, 0x11d1, {0x9b, 0x1b, 0x00, 0xaa, 0x00, 0xbb, 0xb8, 0xae}
    };
    /* GUID_ZPixelFormats returns a DWORD count before the format array. */
    static const struct {
        DWORD count;
        DDPIXELFORMAT formats[1];
    } depth_formats = {
        .count = 1,
        .formats = {{
            .dwSize = sizeof(DDPIXELFORMAT), .dwFlags = DDPF_ZBUFFER,
            .dwZBufferBitDepth = 16, .dwZBitMask = 0xffff
        }}
    };
#endif
    static const D3DHAL_CALLBACKS3 callbacks = {
        .dwSize = sizeof(D3DHAL_CALLBACKS3),
        .dwFlags = D3DHAL3_CB32_DRAWPRIMITIVES2 | D3DHAL3_CB32_VALIDATETEXTURESTAGESTATE,
        .ValidateTextureStageState = wgpu_d3d_validate_stages,
        .DrawPrimitives2 = wgpu_d3d_draw
    };
    static const DD_MISCELLANEOUS2CALLBACKS misc = {
        .dwSize = sizeof(DD_MISCELLANEOUS2CALLBACKS),
        .dwFlags = DDHAL_MISC2CB32_CREATESURFACEEX | DDHAL_MISC2CB32_GETDRIVERSTATE |
                   DDHAL_MISC2CB32_DESTROYDDLOCAL,
        .CreateSurfaceEx = wgpu_d3d_create_surface,
        .GetDriverState = wgpu_d3d_get_state,
        .DestroyDDLocal = wgpu_d3d_destroy_local
    };
#ifdef WGPU_NT_SCENE_PIPELINE
    static const GUID nt_callbacks_guid = {
        0x6fe9ecde, 0xdf89, 0x11d1, {0x9d, 0xb0, 0x00, 0x60, 0x08, 0x27, 0x71, 0xba}
    };
    static const DD_NTCALLBACKS nt_callbacks = {
        .dwSize = sizeof(DD_NTCALLBACKS),
        .dwFlags = DDHAL_NTCB32_FLIPTOGDISURFACE | DDHAL_NTCB32_SETEXCLUSIVEMODE,
        .SetExclusiveMode = wgpu_d3d_exclusive_mode,
        .FlipToGDISurface = wgpu_d3d_flip_to_gdi
    };
#endif
    static const D3DHAL_D3DEXTENDEDCAPS extended = {
        .dwSize = sizeof(D3DHAL_D3DEXTENDEDCAPS),
#ifdef WGPU_NT_ENABLE_MATERIAL
        .dwMinTextureWidth = 1, .dwMinTextureHeight = 1,
        .dwMaxTextureWidth = 2048, .dwMaxTextureHeight = 2048,
        .dwTextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 |
            D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_ADD,
        .wMaxTextureBlendStages = 1, .wMaxSimultaneousTextures = 1,
#endif
    };
    if (!d3d_mode_supported) return 0;
    if (wgpu_d3d_equal_guid(&data->guidInfo, &d3d_parse_unknown_guid)) {
        /* XP requires that the HAL acknowledge this notification.
         * This profile handles its declared tokens locally and reports
         * the offset of an unparsed token to the runtime. */
        if (!data->lpvData) return 0;
        data->ddRVal = DD_OK;
        return 1;
    }
    if (data->dwExpectedSize && !data->lpvData) return 0;
#ifdef WGPU_NT_ENABLE_MIPS
    {
        static const GUID private_caps = {
            0xfad16a23, 0x7b66, 0x11d2, {0x83, 0xd7, 0x00, 0xc0, 0x4f, 0x7c, 0xe5, 0x8c}
        };
        static const DD_NTPRIVATEDRIVERCAPS caps = {
            sizeof(caps), DDHAL_PRIVATECAP_ATOMICSURFACECREATION
        };
        if (wgpu_d3d_equal_guid(&data->guidInfo, &private_caps))
            return wgpu_d3d_return_info(data, &caps, sizeof(caps));
    }
#endif
#ifdef WGPU_NT_SCENE_PIPELINE
    if (wgpu_d3d_equal_guid(&data->guidInfo, &nt_callbacks_guid))
        return wgpu_d3d_return_info(data, &nt_callbacks, sizeof(nt_callbacks));
#endif
#ifdef WGPU_NT_ENABLE_DEPTH
    if (wgpu_d3d_equal_guid(&data->guidInfo, &depth_formats_guid))
        return wgpu_d3d_return_info(data, &depth_formats, sizeof(depth_formats));
#endif
    if (wgpu_d3d_equal_guid(&data->guidInfo, &d3d_callbacks3))
        return wgpu_d3d_return_info(data, &callbacks, sizeof(callbacks));
    if (wgpu_d3d_equal_guid(&data->guidInfo, &d3d_misc2))
        return wgpu_d3d_return_info(data, &misc, sizeof(misc));
    if (wgpu_d3d_equal_guid(&data->guidInfo, &d3d_extended_caps))
        return wgpu_d3d_return_info(data, &extended, sizeof(extended));
    return 0;
}

void d3dnt_destroy(PDD_SURFACE_LOCAL surface)
{
    if (!d3d_lock || !surface) return;
    EngAcquireSemaphore(d3d_lock);
    wgpu_d3d_forget_surface(surface);
    EngReleaseSemaphore(d3d_lock);
}

int d3dnt_can_create(DDSURFACEDESC *desc)
{
#ifdef WGPU_NT_ENABLE_DEPTH
    if (desc->ddsCaps.dwCaps & DDSCAPS_ZBUFFER)
        return d3d_mode_supported && (desc->dwFlags & DDSD_PIXELFORMAT) &&
            wgpu_depth_format(&desc->ddpfPixelFormat);
#endif
#ifdef WGPU_NT_ENABLE_MATERIAL
    return d3d_mode_supported && (desc->ddsCaps.dwCaps & DDSCAPS_TEXTURE) &&
#ifndef WGPU_NT_ENABLE_MIPS
        wgpu_d3d_single_texture_level(desc) &&
#endif
        desc->dwWidth && desc->dwWidth <= 2048 &&
        desc->dwHeight && desc->dwHeight <= 2048 &&
        (desc->dwFlags & DDSD_PIXELFORMAT) && wgpu_d3d_texture_format(&desc->ddpfPixelFormat);
#else
    (void)desc;
    return 0;
#endif
}
int d3dnt_dxt(PDD_SURFACE_GLOBAL surface, DWORD fourcc, LONG stride)
{
    (void)surface; (void)fourcc; (void)stride;
    return 0;
}
