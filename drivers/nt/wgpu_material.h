/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WGPU_NT_MATERIAL_H
#define WGPU_NT_MATERIAL_H
#include <stddef.h>
#include "wgpu_fill.h"

#define WGPU_NT_IOCTL_MATERIAL 0x0022e01cul
#define WGPU_NT_IOCTL_MATERIAL_GROUP 0x0022e024ul
#define WGPU_NT_IOCTL_QUEUE_MATERIAL 0x0022e028ul
#define WGPU_NT_IOCTL_DRAIN_MATERIAL 0x0022e02cul
#define WGPU_NT_MATERIAL_MAX_VERTICES 768u
#if defined(WGPU_NT_GROUP_32)
#define WGPU_NT_MATERIAL_GROUP_MAX 32u
#elif defined(WGPU_NT_WIDE_GROUPS)
#define WGPU_NT_MATERIAL_GROUP_MAX 16u
#else
#define WGPU_NT_MATERIAL_GROUP_MAX 8u
#endif
#define WGPU_NT_MATERIAL_GROUP_FIRST 0x28000ul
#define WGPU_NT_MATERIAL_GROUP_STRIDE (WGPU_NT_TRI_BATCH_BYTES + 4096ul)
#define WGPU_NT_MATERIAL_SHADER_FIRST (WGPU_NT_MATERIAL_GROUP_FIRST + \
    WGPU_NT_MATERIAL_GROUP_MAX * WGPU_NT_MATERIAL_GROUP_STRIDE)
#define WGPU_NT_MATERIAL_SHADER_BYTES 4096ul

/* Each entry is a compact WgpuNtMaterialDraw. No guest pointers or offsets
 * into the GPU command arena are supplied by the caller. */
typedef struct WgpuNtMaterialGroup {
    WgpuNtU32 size, count;
} WgpuNtMaterialGroup;

enum WgpuNtTextureFormat {
    WGPU_NT_RGB565 = 1,
    WGPU_NT_ARGB1555,
    WGPU_NT_ARGB4444,
    WGPU_NT_XRGB8888,
    WGPU_NT_ARGB8888
};

enum WgpuNtTextureCombine {
    WGPU_NT_TEXTURE_DISABLED,
    WGPU_NT_TEXTURE_REPLACE,
    WGPU_NT_TEXTURE_MODULATE,
    WGPU_NT_TEXTURE_MODULATE_ALPHA,
    WGPU_NT_TEXTURE_DECAL,
    WGPU_NT_TEXTURE_ADD,
    WGPU_NT_TEXTURE_MODULATE_DIFFUSE_ALPHA,
    WGPU_NT_TEXTURE_ADD_DIFFUSE_ALPHA,
    WGPU_NT_TEXTURE_DECAL_ALPHA
};

/* One immutable aperture page per combine/specular combination. */
#define WGPU_NT_MATERIAL_SHADER_COUNT (2ul * (WGPU_NT_TEXTURE_DECAL_ALPHA + 1ul))
#define WGPU_NT_MATERIAL_SHADER_END (WGPU_NT_MATERIAL_SHADER_FIRST + \
    WGPU_NT_MATERIAL_SHADER_COUNT * WGPU_NT_MATERIAL_SHADER_BYTES)
#define WGPU_NT_MATERIAL_ALT_GROUP_FIRST WGPU_NT_MATERIAL_SHADER_END
#ifdef WGPU_NT_QUEUED_GROUPS
#define WGPU_NT_MATERIAL_ARENAS 4u
#else
#define WGPU_NT_MATERIAL_ARENAS 2u
#endif

typedef struct WgpuNtMaterialPrepared {
    WgpuNtU32 offset, bytes;
} WgpuNtMaterialPrepared;

typedef struct WgpuNtMaterialCache {
    WgpuNtU32 shaders;
#ifdef WGPU_NT_BATCH_TEMPLATES
    struct WgpuNtBatchTemplates *templates;
    WgpuNtTemplateStats template_stats;
#endif
} WgpuNtMaterialCache;

#ifdef WGPU_NT_BATCH_TEMPLATES
size_t wgpu_nt_template_cache_size(void);
#endif

/* The factor and compare field values follow Direct3D's legacy enums. */
#define WGPU_NT_BLEND_ENABLE 1u
#define WGPU_NT_ALPHA_TEST 2u
#define WGPU_NT_SPECULAR_ENABLE 4u
#define WGPU_NT_BLEND_FACTOR_MAX 11u
#define WGPU_NT_BLEND_DEST_MAX 10u
#define WGPU_NT_COMPARE_MAX 8u
#define WGPU_NT_DEPTH_TEST 1u
#define WGPU_NT_DEPTH_WRITE 2u
#define WGPU_NT_DEPTH_ONLY 4u
#define WGPU_NT_MIP_NEAREST 1u
#define WGPU_NT_MIP_LINEAR 3u
#define WGPU_NT_MIP_LEVELS_MAX 12u

typedef struct WgpuNtMipLayout {
    WgpuNtU32 pitch, rows, offset[WGPU_NT_MIP_LEVELS_MAX];
} WgpuNtMipLayout;

/* Keep this linear HALIGN_4/VALIGN_4 layout in sync with the Velocity Gen8
 * miptree. The display allocator and the WDM validator share it. */
static __inline int wgpu_nt_mip_layout(WgpuNtU32 width, WgpuNtU32 height,
    WgpuNtU32 levels, WgpuNtU32 texel, WgpuNtMipLayout *layout)
{
    WgpuNtU32 w = width, h = height, x = 0, y = 0, i;
    WgpuNtU32 longest = width > height ? width : height;
    WgpuNtU32 packed_width;
    if (!width || width > 2048 || !height || height > 2048 ||
        levels < 2 || levels > WGPU_NT_MIP_LEVELS_MAX ||
        !(longest >> (levels - 1)) || (texel != 2 && texel != 4)) return 0;
    packed_width = ((width > 1 ? width / 2 : 1) + 3) & ~3u;
    if (levels > 2) packed_width += ((width > 3 ? width / 4 : 1) + 3) & ~3u;
    if (packed_width < width) packed_width = width;
    layout->pitch = (packed_width * texel + 63) & ~63u;
    layout->rows = 0;
    for (i = 0; i < levels; ++i) {
        WgpuNtU32 rows = (h + 3) & ~3u;
        layout->offset[i] = y * layout->pitch + x * texel;
        if (layout->rows < y + rows) layout->rows = y + rows;
        if (i == 1) x += (w + 3) & ~3u;
        else y += rows;
        if (w > 1) w /= 2;
        if (h > 1) h /= 2;
    }
    return 1;
}

typedef struct WgpuNtTlVertex {
    /* IEEE 754 bit patterns for position, reciprocal W and UV. */
    WgpuNtU32 x, y, z, rhw, diffuse, specular, u, v;
} WgpuNtTlVertex;

typedef struct WgpuNtMaterialDraw {
    WgpuNtFill target;
    /* In this texture descriptor only, reserved encodes (levels << 8) |
     * mip filter. Zero preserves the single-level material wire layout. */
    WgpuNtFill texture;
    WgpuNtU32 size, vertex_count, format, combine;
    WgpuNtU32 min_linear, mag_linear, clamp, fragment_flags;
    WgpuNtU32 blend_src, blend_dst, alpha_compare, alpha_ref;
    WgpuNtU32 depth_offset, depth_pitch, depth_flags, depth_compare;
    WgpuNtTlVertex vertices[WGPU_NT_MATERIAL_MAX_VERTICES];
} WgpuNtMaterialDraw;

typedef char WgpuNtTlVertexSize[sizeof(WgpuNtTlVertex) == 32 ? 1 : -1];
typedef char WgpuNtMaterialSize[sizeof(WgpuNtMaterialDraw) == 24744 ? 1 : -1];
#define WGPU_NT_MATERIAL_HEADER_BYTES offsetof(WgpuNtMaterialDraw, vertices)

/* The size field identifies the layout. The transport may omit vertices
 * beyond vertex_count, and accepts the full-size packet from older clients. */
static __inline WgpuNtU32 wgpu_nt_material_bytes(WgpuNtU32 vertex_count)
{
    return WGPU_NT_MATERIAL_HEADER_BYTES + vertex_count * sizeof(WgpuNtTlVertex);
}

static __inline int wgpu_nt_material_packet_valid(const WgpuNtMaterialDraw *draw,
                                                 WgpuNtU32 bytes)
{
    if (!draw || bytes < WGPU_NT_MATERIAL_HEADER_BYTES) return 0;
    return draw->vertex_count <= WGPU_NT_MATERIAL_MAX_VERTICES &&
        (bytes == sizeof(*draw) || bytes == wgpu_nt_material_bytes(draw->vertex_count));
}

static __inline int wgpu_nt_material_group_valid(const WgpuNtMaterialGroup *group,
                                                WgpuNtU32 bytes)
{
    const unsigned char *entry;
    WgpuNtU32 i;
    if (!group || bytes < sizeof(*group) || group->size != bytes ||
        !group->count || group->count > WGPU_NT_MATERIAL_GROUP_MAX) return 0;
    entry = (const unsigned char *)(group + 1);
    bytes -= sizeof(*group);
    for (i = 0; i < group->count; ++i) {
        const WgpuNtMaterialDraw *draw = (const void *)entry;
        WgpuNtU32 entry_bytes;
        if (bytes < WGPU_NT_MATERIAL_HEADER_BYTES || draw->size != sizeof(*draw) ||
            !draw->vertex_count || draw->vertex_count > WGPU_NT_MATERIAL_MAX_VERTICES)
            return 0;
        entry_bytes = wgpu_nt_material_bytes(draw->vertex_count);
        if (entry_bytes > bytes) return 0;
        bytes -= entry_bytes;
        entry += entry_bytes;
    }
    return bytes == 0;
}

/* Word index in a Y-tiled Z16 buffer with a validated 128-byte pitch. */
static __inline WgpuNtU32 wgpu_nt_depth_word(WgpuNtU32 pitch, WgpuNtU32 x, WgpuNtU32 y)
{
    return (y / 32) * pitch * 16 + (x / 64) * 2048 +
           ((x & 56) << 5) + (y & 31) * 8 + (x & 7);
}

static __inline int wgpu_nt_material_disjoint(const WgpuNtFill *a, const WgpuNtFill *b)
{
    WgpuNtU32 ax, bx, ay, by, aw, bw;
    if ((unsigned long long)a->offset + a->pitch * a->height <= b->offset ||
        (unsigned long long)b->offset + b->pitch * b->height <= a->offset)
        return 1;
    if (a->pitch != b->pitch) return 0;
    /* A rectangular heap can place surfaces beside each other. Include the
     * final partial cache line and confirm that neither row wraps. */
    ax = a->offset % a->pitch;
    bx = b->offset % b->pitch;
    ay = a->offset / a->pitch;
    by = b->offset / b->pitch;
    aw = (a->width * (a->bpp / 8) + 63) & ~63u;
    bw = (b->width * (b->bpp / 8) + 63) & ~63u;
    if (aw > a->pitch - ax || bw > b->pitch - bx) return 0;
    return ax + aw <= bx || bx + bw <= ax || ay + a->height <= by || by + b->height <= ay;
}

/* Give cache maintenance the complete packed footprint, including levels
 * below the logical base image. Validate before addressing either view. */
static __inline int wgpu_nt_material_texture_range(const WgpuNtFill *texture,
    WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx, WgpuNtFill *range)
{
    WgpuNtU32 mode = texture->reserved & 255u;
    WgpuNtU32 levels = texture->reserved >> 8;
    WgpuNtMipLayout layout;
    unsigned long long end;
    *range = *texture;
    range->reserved = 0;
    if (!wgpu_nt_fill_valid(range, lfb_bytes, lfb_gfx)) return 0;
    if (!texture->reserved) return 1;
    if ((mode != WGPU_NT_MIP_NEAREST && mode != WGPU_NT_MIP_LINEAR) ||
        !wgpu_nt_mip_layout(texture->width, texture->height, levels,
                           texture->bpp / 8, &layout) ||
        texture->pitch != layout.pitch) return 0;
    range->width = range->right = layout.pitch / (texture->bpp / 8);
    range->height = range->bottom = layout.rows;
    end = (unsigned long long)range->offset + (unsigned long long)range->pitch * range->height;
    return end <= lfb_bytes && (unsigned long long)lfb_gfx + end <= 0x100000000ull;
}

static __inline int wgpu_nt_material_header_valid(const WgpuNtMaterialDraw *draw,
                                                 WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx)
{
    const WgpuNtFill *target, *texture;
    WgpuNtFill texture_range;
    if (!draw || draw->size != sizeof(*draw) ||
        draw->combine > WGPU_NT_TEXTURE_DECAL_ALPHA ||
        draw->min_linear > 1 || draw->mag_linear > 1 || draw->clamp > 1 ||
        (draw->fragment_flags & ~(WGPU_NT_BLEND_ENABLE | WGPU_NT_ALPHA_TEST | WGPU_NT_SPECULAR_ENABLE)) ||
        draw->blend_src > WGPU_NT_BLEND_FACTOR_MAX || draw->blend_dst > WGPU_NT_BLEND_DEST_MAX ||
        draw->alpha_compare > WGPU_NT_COMPARE_MAX || draw->alpha_ref > 255 ||
        ((draw->fragment_flags & WGPU_NT_BLEND_ENABLE) && (!draw->blend_src || !draw->blend_dst)) ||
        ((draw->fragment_flags & WGPU_NT_ALPHA_TEST) && !draw->alpha_compare) ||
        !draw->vertex_count || draw->vertex_count > WGPU_NT_MATERIAL_MAX_VERTICES ||
        draw->vertex_count % 3) return 0;
    target = &draw->target;
    texture = &draw->texture;
    if (!wgpu_nt_fill_valid(target, lfb_bytes, lfb_gfx) || target->color) return 0;
    if (draw->combine != WGPU_NT_TEXTURE_DISABLED) {
        if (draw->format < WGPU_NT_RGB565 || draw->format > WGPU_NT_ARGB8888 ||
            !wgpu_nt_material_texture_range(texture, lfb_bytes, lfb_gfx, &texture_range) ||
            texture->color || texture->left || texture->top ||
            texture->right != texture->width || texture->bottom != texture->height ||
            texture->bpp != (draw->format >= WGPU_NT_XRGB8888 ? 32u : 16u) ||
            !wgpu_nt_material_disjoint(target, &texture_range)) return 0;
    } else if (draw->format || draw->min_linear || draw->mag_linear || draw->clamp ||
               texture->reserved) {
        return 0;
    }
    if (draw->depth_flags) {
        unsigned long long end;
        WgpuNtU32 rows = (target->height + 31) & ~31u;
        if (draw->depth_flags & ~(WGPU_NT_DEPTH_TEST | WGPU_NT_DEPTH_WRITE | WGPU_NT_DEPTH_ONLY) ||
            !(draw->depth_flags & WGPU_NT_DEPTH_TEST) ||
            !draw->depth_compare || draw->depth_compare > WGPU_NT_COMPARE_MAX ||
            (draw->depth_offset & 4095) || (draw->depth_pitch & 127) ||
            draw->depth_pitch < target->width * 2 || draw->depth_pitch > 8192 ||
            ((draw->depth_flags & WGPU_NT_DEPTH_ONLY) &&
             (!(draw->depth_flags & WGPU_NT_DEPTH_WRITE) ||
              draw->combine != WGPU_NT_TEXTURE_DISABLED || draw->fragment_flags)))
            return 0;
        end = (unsigned long long)draw->depth_offset + draw->depth_pitch * rows;
        /* Y-tiled accesses cover the entire padded depth allocation. */
        if (end > lfb_bytes || (unsigned long long)lfb_gfx + end > 0x100000000ull ||
            (end > target->offset &&
             (unsigned long long)target->offset + target->pitch * target->height > draw->depth_offset) ||
            (draw->combine != WGPU_NT_TEXTURE_DISABLED && end > texture_range.offset &&
             (unsigned long long)texture_range.offset + texture_range.pitch * texture_range.height >
                 draw->depth_offset))
            return 0;
    } else if (draw->depth_offset || draw->depth_pitch || draw->depth_compare) {
        return 0;
    }
    return 1;
}

static __inline int wgpu_nt_tl_vertex_valid(const WgpuNtTlVertex *vertex)
{
    return (vertex->x & 0x7fffffffu) <= 0x45800000u &&
        (vertex->y & 0x7fffffffu) <= 0x45800000u &&
        vertex->z <= 0x3f800000u && vertex->rhw && vertex->rhw <= 0x47800000u &&
        (vertex->u & 0x7fffffffu) <= 0x45800000u &&
        (vertex->v & 0x7fffffffu) <= 0x45800000u;
}

static __inline int wgpu_nt_material_valid(const WgpuNtMaterialDraw *draw,
                                          WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx)
{
    WgpuNtU32 i;
    if (!wgpu_nt_material_header_valid(draw, lfb_bytes, lfb_gfx)) return 0;
    for (i = 0; i < draw->vertex_count; ++i)
        if (!wgpu_nt_tl_vertex_valid(&draw->vertices[i])) return 0;
    return 1;
}

WgpuNtU32 wgpu_nt_build_material(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags,
    const WgpuNtMaterialDraw *draw, WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx,
    WgpuNtMaterialCache *cache);
/* Build in the selected arena without publishing the descriptor or touching the
 * completion word. The owner must choose an arena no longer used by the GPU. */
int wgpu_nt_prepare_material_group(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, unsigned arena, const WgpuNtMaterialGroup *group,
    WgpuNtU32 input_bytes, WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx,
    WgpuNtMaterialCache *cache, WgpuNtMaterialPrepared *prepared);
void wgpu_nt_publish_material_group(volatile WgpuNtU32 *memory,
    const WgpuNtMaterialPrepared *prepared, WgpuNtU32 completed,
    WgpuNtU32 fence, WgpuNtU32 flags);
#ifdef WGPU_NT_QUEUED_GROUPS
void wgpu_nt_queue_material_descriptor(volatile WgpuNtU32 *memory,
    const WgpuNtMaterialPrepared *prepared, WgpuNtU32 fence, WgpuNtU32 flags);
#endif

WgpuNtU32 wgpu_nt_build_material_group(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags,
    const WgpuNtMaterialGroup *group, WgpuNtU32 input_bytes,
    WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx, WgpuNtMaterialCache *cache);
#endif
