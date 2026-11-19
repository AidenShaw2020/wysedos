/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Single-texture state translation for the NT Gen8 material packet. */
/* Direct3D 5 can mark even a single image as a mipmap surface. */
static BOOL wgpu_d3d_single_texture_level(const DDSURFACEDESC *desc)
{
    return !(desc->ddsCaps.dwCaps & DDSCAPS_MIPMAP) ||
           ((desc->dwFlags & DDSD_MIPMAPCOUNT) && desc->dwMipMapCount == 1);
}
static ULONG wgpu_d3d_texture_format(const DDPIXELFORMAT *pixel)
{
    ULONG alpha;
    if ((pixel->dwFlags & ~(DDPF_RGB | DDPF_ALPHAPIXELS)) ||
        !(pixel->dwFlags & DDPF_RGB)) return 0;
    alpha = (pixel->dwFlags & DDPF_ALPHAPIXELS) ? pixel->dwRGBAlphaBitMask : 0;
    if (pixel->dwRGBBitCount == 16) {
        if (pixel->dwRBitMask == 0xf800 && pixel->dwGBitMask == 0x07e0 &&
            pixel->dwBBitMask == 0x001f && !alpha) return WGPU_NT_RGB565;
        if (pixel->dwRBitMask == 0x7c00 && pixel->dwGBitMask == 0x03e0 &&
            pixel->dwBBitMask == 0x001f && alpha == 0x8000) return WGPU_NT_ARGB1555;
        if (pixel->dwRBitMask == 0x0f00 && pixel->dwGBitMask == 0x00f0 &&
            pixel->dwBBitMask == 0x000f && alpha == 0xf000) return WGPU_NT_ARGB4444;
    }
    if (pixel->dwRGBBitCount == 32 && pixel->dwRBitMask == 0xff0000 &&
        pixel->dwGBitMask == 0xff00 && pixel->dwBBitMask == 0xff) {
        if (!alpha) return WGPU_NT_XRGB8888;
        if (alpha == 0xff000000) return WGPU_NT_ARGB8888;
    }
    return 0;
}

#ifdef WGPU_NT_ENABLE_MIPS
#include "d3d_mip.inc"
#endif

static BOOL wgpu_d3d_diffuse_arg(DWORD arg)
{
    return arg == D3DTA_CURRENT || arg == D3DTA_DIFFUSE;
}

static BOOL wgpu_d3d_tex_and_diffuse(DWORD a, DWORD b)
{
    return (a == D3DTA_TEXTURE && wgpu_d3d_diffuse_arg(b)) ||
           (b == D3DTA_TEXTURE && wgpu_d3d_diffuse_arg(a));
}

static ULONG wgpu_d3d_texture_combine(const WgpuD3dContext *context)
{
    const DWORD *s = context->stage_state;
    DWORD color_a = s[D3DTSS_COLORARG1], color_b = s[D3DTSS_COLORARG2];
    DWORD alpha_a = s[D3DTSS_ALPHAARG1], alpha_b = s[D3DTSS_ALPHAARG2];
    DWORD alpha_op = s[D3DTSS_ALPHAOP];
    DWORD alpha_arg = alpha_op == D3DTOP_SELECTARG1 ? alpha_a :
                      alpha_op == D3DTOP_SELECTARG2 ? alpha_b : 0xffffffffu;
    BOOL alpha_input = wgpu_d3d_diffuse_arg(alpha_arg);
    BOOL alpha_texture = alpha_arg == D3DTA_TEXTURE;
    BOOL alpha_mod = alpha_op == D3DTOP_MODULATE && wgpu_d3d_tex_and_diffuse(alpha_a, alpha_b);
    if (s[D3DTSS_ADDRESSU] != s[D3DTSS_ADDRESSV] ||
        (s[D3DTSS_ADDRESSU] != D3DTADDRESS_WRAP && s[D3DTSS_ADDRESSU] != D3DTADDRESS_CLAMP) ||
        (s[D3DTSS_MINFILTER] != D3DTFN_POINT && s[D3DTSS_MINFILTER] != D3DTFN_LINEAR) ||
        (s[D3DTSS_MAGFILTER] != D3DTFG_POINT && s[D3DTSS_MAGFILTER] != D3DTFG_LINEAR) ||
#ifdef WGPU_NT_ENABLE_MIPS
        (s[D3DTSS_MIPFILTER] != D3DTFP_NONE && s[D3DTSS_MIPFILTER] != D3DTFP_POINT &&
         s[D3DTSS_MIPFILTER] != D3DTFP_LINEAR) || s[D3DTSS_MAXMIPLEVEL] ||
        s[D3DTSS_MIPMAPLODBIAS] ||
#else
        s[D3DTSS_MIPFILTER] != D3DTFP_NONE ||
#endif
        s[D3DTSS_TEXCOORDINDEX] ||
        s[D3DTSS_TEXTURETRANSFORMFLAGS]) return 0;
    switch (context->legacy_blend) {
    case D3DTBLEND_COPY:
    case D3DTBLEND_DECAL:
        return WGPU_NT_TEXTURE_REPLACE;
    case D3DTBLEND_MODULATE:
        return WGPU_NT_TEXTURE_MODULATE_ALPHA;
    case D3DTBLEND_MODULATEALPHA:
        return WGPU_NT_TEXTURE_MODULATE;
    case D3DTBLEND_DECALALPHA:
        return WGPU_NT_TEXTURE_DECAL_ALPHA;
    case D3DTBLEND_ADD:
        return WGPU_NT_TEXTURE_ADD_DIFFUSE_ALPHA;
    case 0:
        break;
    default:
        return 0;
    }
    switch (s[D3DTSS_COLOROP]) {
    case D3DTOP_SELECTARG1:
    case D3DTOP_SELECTARG2:
        if ((s[D3DTSS_COLOROP] == D3DTOP_SELECTARG1 ? color_a : color_b) != D3DTA_TEXTURE)
            return 0;
        return alpha_texture ? WGPU_NT_TEXTURE_REPLACE : alpha_input ? WGPU_NT_TEXTURE_DECAL : 0;
    case D3DTOP_MODULATE:
        if (!wgpu_d3d_tex_and_diffuse(color_a, color_b)) return 0;
        if (alpha_texture) return WGPU_NT_TEXTURE_MODULATE_ALPHA;
        if (alpha_input) return WGPU_NT_TEXTURE_MODULATE_DIFFUSE_ALPHA;
        return alpha_mod ? WGPU_NT_TEXTURE_MODULATE : 0;
    case D3DTOP_ADD:
        if (!wgpu_d3d_tex_and_diffuse(color_a, color_b)) return 0;
        return alpha_input ? WGPU_NT_TEXTURE_ADD_DIFFUSE_ALPHA : alpha_mod ? WGPU_NT_TEXTURE_ADD : 0;
    default:
        return 0;
    }
}

static BOOL wgpu_d3d_material_setup(PDEV *pd, const WgpuD3dContext *context,
                                    const WgpuNtFill *target, WgpuNtMaterialDraw *draw,
                                    BOOL textured)
{
    PDD_SURFACE_LOCAL local;
    DSURF texture;
    RECTL rect;
    ULONG format, combine;
    draw->target = *target;
    draw->size = sizeof(*draw);
    draw->fragment_flags = context->fragment_flags;
    draw->blend_src = context->blend_src;
    draw->blend_dst = context->blend_dst;
    draw->alpha_compare = context->alpha_compare;
    draw->alpha_ref = context->alpha_ref;
#ifdef WGPU_NT_ENABLE_DEPTH
    if (context->depth_enabled && !wgpu_depth_material(pd, context->depth, draw,
        WGPU_NT_DEPTH_TEST | (context->depth_write ? WGPU_NT_DEPTH_WRITE : 0),
        context->depth_compare)) return FALSE;
#endif
    if (!textured) return TRUE;
    if (context->wrap_coordinates) {
        D3D_TRACE(phase, 16);
        D3D_TRACE(state, D3DRENDERSTATE_WRAP0);
        D3D_TRACE(value, context->wrap_coordinates);
        return FALSE;
    }
    local = wgpu_d3d_surface(context->draw, context->textures[0]);
    if (!local || !local->lpGbl || !(local->ddsCaps.dwCaps & DDSCAPS_TEXTURE) ||
        !dsurf(pd, local, &texture) || texture.fourcc || texture.pitch <= 0) return FALSE;
    format = wgpu_d3d_texture_format(&local->lpGbl->ddpfSurface);
    combine = wgpu_d3d_texture_combine(context);
    if (!format || !combine) return FALSE;
    rect.left = rect.top = 0;
    rect.right = texture.w;
    rect.bottom = texture.h;
    wgpu_surface_rect(pd, &texture, &rect, &draw->texture);
    draw->texture.bpp = texture.Bpp * 8;
    if (!wgpu_surface_fits(pd, &draw->texture)) return FALSE;
    draw->format = format;
    draw->combine = combine;
    draw->min_linear = context->stage_state[D3DTSS_MINFILTER] == D3DTFN_LINEAR;
    draw->mag_linear = context->stage_state[D3DTSS_MAGFILTER] == D3DTFG_LINEAR;
    draw->clamp = context->stage_state[D3DTSS_ADDRESSU] == D3DTADDRESS_CLAMP;
#ifdef WGPU_NT_ENABLE_MIPS
    if (context->stage_state[D3DTSS_MIPFILTER] != D3DTFP_NONE) {
        ULONG levels = wgpu_mip_levels(pd, local->lpGbl);
        if (levels)
            draw->texture.reserved = (levels << 8) |
                (context->stage_state[D3DTSS_MIPFILTER] == D3DTFP_POINT
                    ? WGPU_NT_MIP_NEAREST : WGPU_NT_MIP_LINEAR);
        else if ((local->ddsCaps.dwCaps & DDSCAPS_MIPMAP) &&
                 local->lpSurfMore && local->lpSurfMore->dwMipMapCount > 1)
            return FALSE;
    }
#endif
    return TRUE;
}

static HRESULT wgpu_d3d_texture_blt(PDEV *pd, const WgpuD3dContext *context,
                                   const D3DHAL_DP2TEXBLT *blt)
{
    PDD_SURFACE_LOCAL src = wgpu_d3d_surface(context->draw, blt->dwDDSrcSurface);
    PDD_SURFACE_LOCAL dst;
    PDD_SURFACE_GLOBAL sg;
    DSURF dest, source;
    RECTL rect;
    WgpuNtFill dst_bounds;
    ULONG format, width, height, pitch, bytes, y, start, extent;
    ULONG_PTR origin;
    BYTE *row;
    HANDLE secured = NULL;
    BOOL system, backward;
    D3D_TRACE(state, blt->dwDDDestSurface);
    D3D_TRACE(value, blt->dwDDSrcSurface);
    D3D_TRACE(phase, 8);
    if (!src || !src->lpGbl || !(src->ddsCaps.dwCaps & DDSCAPS_TEXTURE))
        return DDERR_INVALIDPARAMS;
    /* A zero destination requests residency of an existing texture. */
    if (!blt->dwDDDestSurface)
        return dsurf(pd, src, &source) && !source.fourcc &&
            wgpu_d3d_texture_format(&src->lpGbl->ddpfSurface) ? DD_OK : DDERR_UNSUPPORTED;
    D3D_TRACE(phase, 9);
    dst = wgpu_d3d_surface(context->draw, blt->dwDDDestSurface);
    if (!dst || !dst->lpGbl || !(dst->ddsCaps.dwCaps & DDSCAPS_TEXTURE) ||
        !dsurf(pd, dst, &dest) || !wgpu_flush_supported() ||
        dest.pitch <= 0 || blt->pDest.x < 0 || blt->pDest.y < 0 ||
        blt->rSrc.left < 0 || blt->rSrc.top < 0 ||
        blt->rSrc.right <= blt->rSrc.left || blt->rSrc.bottom <= blt->rSrc.top)
        return DDERR_INVALIDPARAMS;
    D3D_TRACE(phase, 10);
    sg = src->lpGbl;
    format = wgpu_d3d_texture_format(&sg->ddpfSurface);
    if (!format || format != wgpu_d3d_texture_format(&dst->lpGbl->ddpfSurface) ||
        dest.Bpp != (format >= WGPU_NT_XRGB8888 ? 4u : 2u))
        return DDERR_INVALIDPIXELFORMAT;
    if (sg->wWidth > 2048 || sg->wHeight > 2048 ||
        (ULONG)blt->rSrc.right > sg->wWidth || (ULONG)blt->rSrc.bottom > sg->wHeight ||
        (ULONG)blt->pDest.x >= dest.w || (ULONG)blt->pDest.y >= dest.h ||
        sg->lPitch <= 0 || sg->lPitch > 16384 ||
        (ULONG)sg->lPitch < sg->wWidth * dest.Bpp)
        return DDERR_INVALIDPARAMS;
    width = blt->rSrc.right - blt->rSrc.left;
    height = blt->rSrc.bottom - blt->rSrc.top;
    if (width > dest.w - blt->pDest.x || height > dest.h - blt->pDest.y)
        return DDERR_INVALIDPARAMS;
    rect.left = blt->pDest.x;
    rect.top = blt->pDest.y;
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    wgpu_surface_rect(pd, &dest, &rect, &dst_bounds);
    dst_bounds.bpp = dest.Bpp * 8;
    if (!wgpu_surface_fits(pd, &dst_bounds)) return DDERR_INVALIDPARAMS;
    D3D_TRACE(phase, 11);
    pitch = sg->lPitch;
    bytes = width * dest.Bpp;
    start = blt->rSrc.top * pitch + blt->rSrc.left * dest.Bpp;
    extent = (height - 1) * pitch + bytes;
    system = (src->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) != 0;
    if (system) {
        origin = sg->fpVidMem;
        if (origin < 0x10000 || origin >= WGPU_D3D_USER_LIMIT ||
            start >= WGPU_D3D_USER_LIMIT - origin ||
            extent > WGPU_D3D_USER_LIMIT - (origin + start)) return DDERR_INVALIDPARAMS;
        secured = EngSecureMem((void *)(origin + start), extent);
        if (!secured) return DDERR_INVALIDPARAMS;
    } else {
        WgpuNtFill src_bounds;
        if (!dsurf(pd, src, &source) || source.Bpp != dest.Bpp) return DDERR_INVALIDPARAMS;
        wgpu_surface_rect(pd, &source, &blt->rSrc, &src_bounds);
        src_bounds.bpp = source.Bpp * 8;
        if (!wgpu_surface_fits(pd, &src_bounds)) return DDERR_INVALIDPARAMS;
        origin = (ULONG_PTR)source.base;
    }
    row = EngAllocMem(0, bytes, 'bTgw');
    if (!row) {
        if (secured) EngUnsecureMem(secured);
        return DDERR_OUTOFMEMORY;
    }
    backward = !system && origin == (ULONG_PTR)dest.base && rect.top > blt->rSrc.top;
    for (y = 0; y < height; ++y) {
        ULONG index = backward ? height - 1 - y : y;
        memcpy(row, (const BYTE *)origin + start + index * pitch, bytes);
        memcpy(dest.base + (rect.top + index) * dest.pitch + rect.left * dest.Bpp, row, bytes);
    }
    wgpu_flush_fill(pd, &dst_bounds);
    EngFreeMem(row);
    if (secured) EngUnsecureMem(secured);
    return DD_OK;
}

#define WGPU_TEXTURE_FORMAT(bits, r, g, b, flags, a) { \
    .dwSize = sizeof(DDSURFACEDESC), .dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT, \
    .ddsCaps = { .dwCaps = DDSCAPS_TEXTURE }, \
    .ddpfPixelFormat = { .dwSize = sizeof(DDPIXELFORMAT), .dwFlags = DDPF_RGB | (flags), \
       .dwRGBBitCount = (bits), .dwRBitMask = (r), .dwGBitMask = (g), .dwBBitMask = (b), \
       .dwRGBAlphaBitMask = (a) } }
static DDSURFACEDESC wgpu_texture_formats[] = {
    WGPU_TEXTURE_FORMAT(16, 0xf800, 0x07e0, 0x001f, 0, 0),
    WGPU_TEXTURE_FORMAT(16, 0x7c00, 0x03e0, 0x001f, DDPF_ALPHAPIXELS, 0x8000),
    WGPU_TEXTURE_FORMAT(16, 0x0f00, 0x00f0, 0x000f, DDPF_ALPHAPIXELS, 0xf000),
    WGPU_TEXTURE_FORMAT(32, 0xff0000, 0xff00, 0xff, 0, 0),
    WGPU_TEXTURE_FORMAT(32, 0xff0000, 0xff00, 0xff, DDPF_ALPHAPIXELS, 0xff000000)
};
#undef WGPU_TEXTURE_FORMAT
