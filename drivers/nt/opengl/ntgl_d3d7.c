/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Experimental Velocity R3D adapter for the NT Direct3D 7 HAL. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include "velocity9x/r3d_abi.h"

void v9x_gl_log3(const char *format, v9x_u32 a, v9x_u32 b, v9x_u32 c);

/* Calls are serialized by the ICD, which releases the target before resizing. */
static IDirect3D7 *hal;
static IDirect3DDevice7 *context;
static IDirectDrawSurface7 *render_target;
static IDirectDrawSurface7 *render_depth;
static DDSURFACEDESC2 display_mode;
static v9x_u32 generation;
static unsigned draw_failures_logged;
static DWORD target_width, target_height;

typedef struct NtglCachedState {
    DWORD value;
    BOOL valid;
} NtglCachedState;

static NtglCachedState render_states[D3DRENDERSTATE_LIGHTING + 1];
static NtglCachedState texture_states[D3DTSS_MIPFILTER + 1];
static D3DVIEWPORT7 active_viewport;
static BOOL viewport_valid;
static BOOL scene_open;

static void invalidate_state(void)
{
    unsigned i;
    for (i = 0; i < sizeof render_states / sizeof render_states[0]; ++i)
        render_states[i].valid = FALSE;
    for (i = 0; i < sizeof texture_states / sizeof texture_states[0]; ++i)
        texture_states[i].valid = FALSE;
    viewport_valid = FALSE;
}

static HRESULT render_state(D3DRENDERSTATETYPE name, DWORD value)
{
    NtglCachedState *state;
    HRESULT result;
    if ((unsigned)name >= sizeof render_states / sizeof render_states[0])
        return IDirect3DDevice7_SetRenderState(context, name, value);
    state = &render_states[name];
    if (state->valid && state->value == value) return DD_OK;
    result = IDirect3DDevice7_SetRenderState(context, name, value);
    state->valid = result == DD_OK;
    state->value = value;
    return result;
}

static HRESULT texture_state(D3DTEXTURESTAGESTATETYPE name, DWORD value)
{
    NtglCachedState *state;
    HRESULT result;
    if ((unsigned)name >= sizeof texture_states / sizeof texture_states[0])
        return IDirect3DDevice7_SetTextureStageState(context, 0, name, value);
    state = &texture_states[name];
    if (state->valid && state->value == value) return DD_OK;
    result = IDirect3DDevice7_SetTextureStageState(context, 0, name, value);
    state->valid = result == DD_OK;
    state->value = value;
    return result;
}

static HRESULT set_viewport(D3DVIEWPORT7 *viewport)
{
    HRESULT result;
    if (viewport_valid && active_viewport.dwX == viewport->dwX &&
        active_viewport.dwY == viewport->dwY &&
        active_viewport.dwWidth == viewport->dwWidth &&
        active_viewport.dwHeight == viewport->dwHeight) return DD_OK;
    result = IDirect3DDevice7_SetViewport(context, viewport);
    viewport_valid = result == DD_OK;
    active_viewport = *viewport;
    return result;
}

int ntgl_close_scene(void)
{
    HRESULT result;
    if (!scene_open) return 1;
    scene_open = FALSE;
    result = IDirect3DDevice7_EndScene(context);
    if (result != DD_OK) {
        invalidate_state();
        v9x_gl_log3("NT D3D EndScene=%08lX", (DWORD)result, 0, 0);
    }
    return result == DD_OK;
}

static int open_scene(void)
{
    if (!scene_open) scene_open = IDirect3DDevice7_BeginScene(context) == DD_OK;
    return scene_open;
}

void ntgl_forget_surface(void *surface)
{
    if (context && (!surface || surface == render_target)) {
        (void)ntgl_close_scene();
        IDirect3DDevice7_Release(context);
        if (render_depth)
            IDirectDrawSurface7_DeleteAttachedSurface(render_target, 0, render_depth);
        context = 0;
        render_target = 0;
        render_depth = 0;
    }
}

static int bind_target(IDirectDrawSurface7 *surface, IDirectDrawSurface7 *depth)
{
    DDSURFACEDESC2 desc = {0};
    if (!surface) return 0;
    if (IDirectDrawSurface7_IsLost(surface) != DD_OK) {
        ntgl_forget_surface(surface);
        return 0;
    }
    if (context && render_target == surface && render_depth == depth) return 1;
    desc.dwSize = sizeof desc;
    if (IDirectDrawSurface7_GetSurfaceDesc(surface, &desc) != DD_OK) return 0;
    ntgl_forget_surface(0);
    if (depth && IDirectDrawSurface7_AddAttachedSurface(surface, depth) != DD_OK)
        return 0;
    if (IDirect3D7_CreateDevice(hal, &IID_IDirect3DHALDevice, surface, &context) != DD_OK) {
        if (depth) IDirectDrawSurface7_DeleteAttachedSurface(surface, 0, depth);
        return 0;
    }
    render_target = surface;
    render_depth = depth;
    target_width = desc.dwWidth;
    target_height = desc.dwHeight;
    invalidate_state();
    return 1;
}

static int configure_depth(const V9X_R3D_ABI_STATE *state)
{
    if (render_state(D3DRENDERSTATE_ZENABLE,
                           state->depth_enable ? D3DZB_TRUE : D3DZB_FALSE) != DD_OK)
        return 0;
    if (!state->depth_enable) return 1;
    return render_state(D3DRENDERSTATE_ZWRITEENABLE,
                           state->depth_write != 0) == DD_OK &&
           render_state(D3DRENDERSTATE_ZFUNC,
                           state->depth_func) == DD_OK;
}

static int configure_alpha(const V9X_R3D_ABI_STATE *state)
{
    if (render_state(D3DRENDERSTATE_ALPHABLENDENABLE,
                           state->blend_enable != 0) != DD_OK ||
        render_state(D3DRENDERSTATE_ALPHATESTENABLE,
                           state->alpha_test_enable != 0) != DD_OK)
        return 0;
    if (state->blend_enable &&
        (render_state(D3DRENDERSTATE_SRCBLEND,
                                state->src_blend) != DD_OK ||
         render_state(D3DRENDERSTATE_DESTBLEND,
                                state->dst_blend) != DD_OK))
        return 0;
    return !state->alpha_test_enable ||
           (render_state(D3DRENDERSTATE_ALPHAFUNC,
                                  state->alpha_func) == DD_OK &&
            render_state(D3DRENDERSTATE_ALPHAREF,
                                  state->alpha_ref) == DD_OK);
}

static int configure_texture(const V9X_R3D_ABI_TEXTURE *texture)
{
    DWORD color, alpha, alpha_arg;
    if (texture->storage == V9X_R3D_ABI_TEXTURE_NONE)
        return IDirect3DDevice7_SetTexture(context, 0, 0) == DD_OK;
    if (texture->storage != V9X_R3D_ABI_TEXTURE_HW || !texture->surface.surface ||
        texture->mip < V9X_R3D_ABI_MIP_NONE || texture->mip > V9X_R3D_ABI_MIP_LINEAR)
        return 0;
    if (texture->color_op == V9X_R3D_ABI_COLOROP_REPLACE) color = D3DTOP_SELECTARG1;
    else if (texture->color_op == V9X_R3D_ABI_COLOROP_MODULATE) color = D3DTOP_MODULATE;
    else return 0;
    alpha = D3DTOP_SELECTARG1;
    alpha_arg = D3DTA_DIFFUSE;
    if (texture->alpha_op == V9X_R3D_ABI_ALPHAOP_REPLACE) alpha_arg = D3DTA_TEXTURE;
    else if (texture->alpha_op == V9X_R3D_ABI_ALPHAOP_MODULATE) {
        alpha = D3DTOP_MODULATE;
        alpha_arg = D3DTA_TEXTURE;
    } else if (texture->alpha_op != V9X_R3D_ABI_ALPHAOP_FRAGMENT) return 0;
    if (color == D3DTOP_SELECTARG1 && alpha == D3DTOP_MODULATE) return 0;

    return IDirect3DDevice7_SetTexture(context, 0, texture->surface.surface) == DD_OK &&
           texture_state(D3DTSS_COLOROP, color) == DD_OK &&
           texture_state(D3DTSS_COLORARG1, D3DTA_TEXTURE) == DD_OK &&
           texture_state(D3DTSS_COLORARG2, D3DTA_DIFFUSE) == DD_OK &&
           texture_state(D3DTSS_ALPHAOP, alpha) == DD_OK &&
           texture_state(D3DTSS_ALPHAARG1, alpha_arg) == DD_OK &&
           texture_state(D3DTSS_ALPHAARG2, D3DTA_DIFFUSE) == DD_OK &&
           texture_state(D3DTSS_ADDRESSU, texture->address) == DD_OK &&
           texture_state(D3DTSS_ADDRESSV, texture->address) == DD_OK &&
           texture_state(D3DTSS_MINFILTER, texture->min_filter) == DD_OK &&
           texture_state(D3DTSS_MAGFILTER, texture->mag_filter) == DD_OK &&
           texture_state(D3DTSS_MIPFILTER, texture->mip) == DD_OK;
}

static v9x_u32 V9X_R3D_CALL describe(V9X_R3D_ABI_DESCRIBE *out)
{
    static const char renderer[] = "wyseDOS WGPU NT Direct3D HAL";
    unsigned i;
    if (out->struct_bytes != sizeof(*out)) return V9X_R3D_RESULT_ABI;
    out->abi_version = V9X_R3D_ABI_VERSION;
    out->generation = generation;
    out->engine = V9X_R3D_ABI_ENGINE_GEN8;
    out->target_formats = 1ul << (display_mode.ddpfPixelFormat.dwRGBBitCount == 16
                          ? V9X_R3D_ABI_FORMAT_RGB565 : V9X_R3D_ABI_FORMAT_XRGB8888);
    out->texture_formats = (1ul << V9X_R3D_ABI_FORMAT_RGB565) |
                           (1ul << V9X_R3D_ABI_FORMAT_ARGB1555) |
                           (1ul << V9X_R3D_ABI_FORMAT_ARGB4444);
    out->hw_texture_size_min = 1;
    out->hw_texture_size_max = 2048;
    out->hw_texture_shape = V9X_R3D_ABI_HWTEX_POW2;
    out->texture_size_max = out->hw_texture_size_max;
    out->batch_max = 256;
    out->texture_units = 1;
    for (i = 0; i < sizeof(renderer); ++i) out->renderer[i] = renderer[i];
    return V9X_R3D_RESULT_OK;
}

static v9x_u32 V9X_R3D_CALL draw(const V9X_R3D_ABI_DRAW *job, V9X_R3D_ABI_OUTCOME *out)
{
    D3DVIEWPORT7 viewport = {0};
    v9x_u32 result = V9X_R3D_RESULT_UNSUPPORTED;
    HRESULT render;
    out->submitted = 0;
    if (job->struct_bytes != sizeof(*job)) result = V9X_R3D_RESULT_ABI;
    else if (job->generation != generation) result = V9X_R3D_RESULT_STALE;
    else if (job->vertices && job->triangle_count && job->triangle_count <= 256 &&
             !job->texture1.storage &&
             (!job->state.depth_enable || job->depth.surface) &&
             !job->state.fog_enable && !job->raw_target.address && !job->raw_depth.address &&
             job->state.write_mask == V9X_R3D_ABI_WRITE_RGB &&
             job->state.scissor_left < job->state.scissor_right &&
             job->state.scissor_top < job->state.scissor_bottom &&
             bind_target(job->target.surface, job->depth.surface) &&
             job->state.scissor_right <= target_width && job->state.scissor_bottom <= target_height) {
        /* Vertices already use screen coordinates. The HAL clips these
         * against the viewport without rescaling them. The visible drawable
         * may be narrower than its padded backing surface. */
        viewport.dwX = job->state.scissor_left;
        viewport.dwY = job->state.scissor_top;
        viewport.dwWidth = job->state.scissor_right - viewport.dwX;
        viewport.dwHeight = job->state.scissor_bottom - viewport.dwY;
        viewport.dvMaxZ = 1.0f;
        if (set_viewport(&viewport) == DD_OK &&
            configure_depth(&job->state) &&
            configure_alpha(&job->state) &&
            configure_texture(&job->texture) &&
            render_state(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE) == DD_OK &&
            render_state(D3DRENDERSTATE_LIGHTING, FALSE) == DD_OK &&
            open_scene()) {
            render = IDirect3DDevice7_DrawPrimitive(context, D3DPT_TRIANGLELIST,
                      D3DFVF_TLVERTEX, (void *)job->vertices, 3 * job->triangle_count, 0);
            result = render == DD_OK ? V9X_R3D_RESULT_OK : V9X_R3D_RESULT_INDETERMINATE;
            if (result == V9X_R3D_RESULT_OK) {
                out->submitted = job->triangle_count;
            } else {
                (void)ntgl_close_scene();
                invalidate_state();
                if (draw_failures_logged < 8) {
                    ++draw_failures_logged;
                    v9x_gl_log3("NT D3D DrawPrimitive=%08lX triangles=%lu",
                                (DWORD)render, job->triangle_count, 0);
                }
            }
        }
        if (job->texture.storage)
            IDirect3DDevice7_SetTexture(context, 0, 0);
    }
    out->result = result;
    return result;
}

static v9x_u32 V9X_R3D_CALL clear(const V9X_R3D_ABI_CLEAR *job)
{
    D3DRECT rect;
    DWORD flags = 0;
    float depth = 1.0f;
    const V9X_R3D_ABI_RECT *requested;
    if (job->struct_bytes != sizeof(*job)) return V9X_R3D_RESULT_ABI;
    if (job->generation != generation) return V9X_R3D_RESULT_STALE;
    if (job->clear_color && job->write_mask != V9X_R3D_ABI_WRITE_RGB)
        return V9X_R3D_RESULT_UNSUPPORTED;
    if (job->clear_color) flags |= D3DCLEAR_TARGET;
    if (job->clear_depth && job->write_depth) {
        if (!job->depth.surface || job->depth_value > 0xfffffful)
            return V9X_R3D_RESULT_INVALID;
        flags |= D3DCLEAR_ZBUFFER;
        depth = (float)job->depth_value / 16777215.0f;
    }
    if (!flags) return V9X_R3D_RESULT_OK;
    if (job->rect_count != 1 || !job->rects ||
        !bind_target(job->target.surface, job->depth.surface))
        return V9X_R3D_RESULT_UNSUPPORTED;
    requested = job->rects;
    if (requested->left >= requested->right || requested->top >= requested->bottom ||
        requested->right > target_width || requested->bottom > target_height)
        return V9X_R3D_RESULT_INVALID;
    rect.x1 = requested->left;
    rect.y1 = requested->top;
    rect.x2 = requested->right;
    rect.y2 = requested->bottom;
    if (IDirect3DDevice7_Clear(context, 1, &rect, flags,
                               0xff000000ul | job->color_value, depth, 0) != DD_OK)
        return V9X_R3D_RESULT_INDETERMINATE;
    return V9X_R3D_RESULT_OK;
}

static v9x_u32 V9X_R3D_CALL flush(v9x_u32 expected)
{
    if (generation != expected) return V9X_R3D_RESULT_STALE;
    return ntgl_close_scene() ? V9X_R3D_RESULT_OK : V9X_R3D_RESULT_INDETERMINATE;
}

static v9x_u32 V9X_R3D_CALL finish(v9x_u32 expected)
{
    DDSURFACEDESC2 desc = {0};
    if (generation != expected) return V9X_R3D_RESULT_STALE;
    if (!ntgl_close_scene()) return V9X_R3D_RESULT_INDETERMINATE;
    if (!render_target) return V9X_R3D_RESULT_OK;
    desc.dwSize = sizeof desc;
    if (IDirectDrawSurface7_Lock(render_target, 0, &desc, DDLOCK_WAIT | DDLOCK_READONLY, 0) != DD_OK)
        return V9X_R3D_RESULT_INDETERMINATE;
    return IDirectDrawSurface7_Unlock(render_target, 0) == DD_OK
        ? V9X_R3D_RESULT_OK : V9X_R3D_RESULT_INDETERMINATE;
}

static const V9X_R3D_INTERFACE callbacks = {
    V9X_R3D_ABI_VERSION, sizeof(V9X_R3D_INTERFACE), describe, draw, clear, flush, finish, 0
};

const V9X_R3D_INTERFACE *ntgl_bind(IDirectDraw7 *ddraw)
{
    DDSURFACEDESC2 mode = {0};
    IDirect3D7 *new_hal;
    DWORD bits;
    if (!ddraw) return 0;
    mode.dwSize = sizeof mode;
    if (IDirectDraw7_GetDisplayMode(ddraw, &mode) != DD_OK) return 0;
    bits = mode.ddpfPixelFormat.dwRGBBitCount;
    if ((bits != 16 && bits != 32) ||
        (bits == 16 && mode.ddpfPixelFormat.dwRBitMask != 0xf800ul) ||
        IDirectDraw7_QueryInterface(ddraw, &IID_IDirect3D7, (void **)&new_hal) != DD_OK)
        return 0;
    ntgl_forget_surface(0);
    if (hal) IDirect3D7_Release(hal);
    hal = new_hal;
    display_mode = mode;
    if (++generation == 0) ++generation;
    return &callbacks;
}

int ntgl_mode_changed(IDirectDraw7 *ddraw)
{
    DDSURFACEDESC2 mode = {0};
    mode.dwSize = sizeof mode;
    if (IDirectDraw7_GetDisplayMode(ddraw, &mode) != DD_OK) return 0;
    return mode.dwWidth != display_mode.dwWidth ||
           mode.dwHeight != display_mode.dwHeight ||
           mode.ddpfPixelFormat.dwRGBBitCount != display_mode.ddpfPixelFormat.dwRGBBitCount ||
           mode.ddpfPixelFormat.dwRBitMask != display_mode.ddpfPixelFormat.dwRBitMask ||
           mode.ddpfPixelFormat.dwGBitMask != display_mode.ddpfPixelFormat.dwGBitMask ||
           mode.ddpfPixelFormat.dwBBitMask != display_mode.ddpfPixelFormat.dwBBitMask;
}
