/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wgpu_material.h"
#include "velocity9x/intel_gen8_3d.h"
#include "velocity9x/intel_gen8_eu.h"
#include "velocity9x/wgpu_chain.h"
#ifdef WGPU_NT_BATCH_TEMPLATES
#include "velocity9x/gen8_batch_template.h"
#endif

enum {
    BINDING = 0x4000,
    TARGET_STATE = 0x4040,
    TEXTURE_STATE = 0x4080,
    BLEND_STATE = 0x4100,
    COLOR_CALC = 0x4140,
    CC_VIEWPORT = 0x4180,
    SF_VIEWPORT = 0x41c0,
    SAMPLER = 0x4200,
    VERTICES = 0x8000
};

typedef char WgpuNtMaterialVerticesFit[
    VERTICES + WGPU_NT_MATERIAL_MAX_VERTICES * sizeof(WgpuNtTlVertex) <= WGPU_NT_TRI_BATCH_BYTES ? 1 : -1
];
typedef char WgpuNtMaterialShaderMaskFits[WGPU_NT_MATERIAL_SHADER_COUNT <= 32 ? 1 : -1];

#ifdef WGPU_NT_BULK_COPY
/* The Win9x driver uses string copies for contiguous commands and vertices
 * in write-combined memory. Keep the small state writes on the scalar path. */
static void copy_aperture_words(volatile v9x_u32 *out, const void *in, v9x_u32 count)
{
    __asm__ __volatile__("cld; rep movsl"
        : "+D"(out), "+S"(in), "+c"(count) : : "memory", "cc");
}
#endif

static void write_words(volatile v9x_u32 *out, const v9x_u32 *in, v9x_u32 count)
{
#ifdef WGPU_NT_BULK_COPY
    if (count >= 32) {
        copy_aperture_words(out, in, count);
        return;
    }
#endif
    v9x_u32 i;
    for (i = 0; i < count; ++i) out[i] = in[i];
}

#ifdef WGPU_NT_BATCH_TEMPLATES
#define WGPU_NT_TEMPLATE_SLOTS 8u
#define WGPU_NT_COMMAND_WORDS 512u
typedef struct WgpuNtBatchTemplate {
    v9x_u32 key[V9X_GEN8_BATCH_KEY_WORDS];
    v9x_u32 words[WGPU_NT_COMMAND_WORDS];
    struct v9x_gen8_batch_layout layout;
} WgpuNtBatchTemplate;

typedef struct WgpuNtBatchTemplates {
    WgpuNtBatchTemplate entries[WGPU_NT_TEMPLATE_SLOTS];
} WgpuNtBatchTemplates;

size_t wgpu_nt_template_cache_size(void)
{
    return sizeof(WgpuNtBatchTemplates);
}

static int build_material_commands(struct v9x_hsw_batch *batch, v9x_u32 *command,
                                   const struct v9x_gen8_draw *draw, WgpuNtMaterialCache *cache)
{
    WgpuNtBatchTemplate *entry;
    v9x_u32 key[V9X_GEN8_BATCH_KEY_WORDS], hash = 2166136261u;
    unsigned i;
    v9x_intel_hsw_batch_init(batch, command, WGPU_NT_COMMAND_WORDS);
    if (!cache || !cache->templates) return v9x_intel_gen8_draw(batch, draw) == V9X_STATUS_OK;
    ++cache->template_stats.lookups;
    v9x_gen8_batch_key(key, draw);
    for (i = 0; i < V9X_GEN8_BATCH_KEY_WORDS; ++i) hash = (hash ^ key[i]) * 16777619u;
    hash ^= hash >> 16;
    hash ^= hash >> 8;
    entry = &cache->templates->entries[hash & (WGPU_NT_TEMPLATE_SLOTS - 1)];
    if (entry->layout.used) {
        for (i = 0; i < V9X_GEN8_BATCH_KEY_WORDS && entry->key[i] == key[i]; ++i) {}
        if (i == V9X_GEN8_BATCH_KEY_WORDS) {
            batch->used = entry->layout.used;
            write_words(command, entry->words, batch->used);
            if (v9x_gen8_batch_relocate(command, batch->used, draw, &entry->layout)) {
                ++cache->template_stats.hits;
                return 1;
            }
            v9x_intel_hsw_batch_init(batch, command, WGPU_NT_COMMAND_WORDS);
        }
    }
    if (v9x_intel_gen8_draw(batch, draw) != V9X_STATUS_OK) return 0;
    if (v9x_gen8_batch_layout(command, batch->used, &entry->layout)) {
        write_words(entry->key, key, V9X_GEN8_BATCH_KEY_WORDS);
        write_words(entry->words, command, batch->used);
    }
    return 1;
}
#endif

static WgpuNtU32 wgpu_nt_build_material_slot(volatile WgpuNtU32 *memory,
    WgpuNtU32 gfx, WgpuNtU32 batch_offset, WgpuNtU32 kernel_offset, WgpuNtU32 next_gfx,
    const WgpuNtMaterialDraw *draw, WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx,
    WgpuNtMaterialCache *cache)
{
    static const v9x_u16 formats[] = {
        0, V9X_GEN8_FMT_B5G6R5_UNORM, V9X_GEN8_FMT_B5G5R5A1_UNORM,
        V9X_GEN8_FMT_B4G4R4A4_UNORM, V9X_GEN8_FMT_B8G8R8X8_UNORM,
        V9X_GEN8_FMT_B8G8R8A8_UNORM
    };
    static const v9x_u8 combines[] = {
        V9X_HSW_PS_DIFFUSE, V9X_HSW_PS_TEXTURE, V9X_HSW_PS_MODULATE, V9X_HSW_PS_MODULATE_TEXALPHA,
        V9X_HSW_PS_DECAL, V9X_HSW_PS_ADD,
        V9X_HSW_PS_STAGE0_OPS(V9X_HSW_PS_OP_MODULATE, V9X_HSW_PS_AOP_INPUT),
        V9X_HSW_PS_STAGE0_OPS(V9X_HSW_PS_OP_ADD, V9X_HSW_PS_AOP_INPUT),
        V9X_HSW_PS_STAGE0_OPS(V9X_HSW_PS_OP_DECALALPHA, V9X_HSW_PS_AOP_INPUT)
    };
    static const v9x_u8 blend_factors[] = {
        0, V9X_GEN8_BLEND_ZERO, V9X_GEN8_BLEND_ONE,
        V9X_GEN8_BLEND_SRC_COLOR, V9X_GEN8_BLEND_INV_SRC_COLOR,
        V9X_GEN8_BLEND_SRC_ALPHA, V9X_GEN8_BLEND_INV_SRC_ALPHA,
        /* RGB565 and XRGB8888 have implicit destination alpha 1. */
        V9X_GEN8_BLEND_ONE, V9X_GEN8_BLEND_ZERO, V9X_GEN8_BLEND_DST_COLOR,
        V9X_GEN8_BLEND_INV_DST_COLOR, V9X_GEN8_BLEND_ZERO
    };
    volatile v9x_u32 *slot;
    struct v9x_gen8_draw gen8 = {0};
    struct v9x_hsw_ps_desc shader = {0};
    struct v9x_hsw_batch batch;
    v9x_u32 code[256], command[512], state[16], viewport[2], code_words = 0;
    WgpuNtU32 shader_bit = 0;
    const WgpuNtFill *target, *texture;
    if (!wgpu_nt_material_valid(draw, lfb_bytes, lfb_gfx))
        return 0;
    target = &draw->target;
    texture = &draw->texture;
    shader.stage0 = combines[draw->combine];
    shader.specular = !!(draw->fragment_flags & WGPU_NT_SPECULAR_ENABLE);
    if (cache) {
        WgpuNtU32 index = 2 * draw->combine + shader.specular;
        shader_bit = 1ul << index;
        kernel_offset = WGPU_NT_MATERIAL_SHADER_FIRST + index * WGPU_NT_MATERIAL_SHADER_BYTES;
    }
    if ((!cache || !(cache->shaders & shader_bit)) &&
        v9x_intel_gen8_ps_kernel_build(code, 256, &shader, 0, &code_words) != V9X_STATUS_OK)
        return 0;
    slot = memory + batch_offset / 4;
    gen8.state_base = gfx + batch_offset;
    gen8.state_bytes = WGPU_NT_TRI_BATCH_BYTES;
    gen8.kernel_base = gfx + kernel_offset;
    gen8.kernel_bytes = 4096;
    gen8.binding_table = BINDING;
    gen8.binding_entries = 1;
    if (draw->combine != WGPU_NT_TEXTURE_DISABLED) {
        gen8.binding_entries = 2;
        gen8.samplers = 1;
        gen8.sampler = SAMPLER;
    }
    gen8.blend = BLEND_STATE;
    gen8.cc = COLOR_CALC;
    gen8.cc_viewport = CC_VIEWPORT;
    gen8.sf_clip_viewport = SF_VIEWPORT;
    gen8.vertices = gen8.state_base + VERTICES;
    gen8.vertex_count = draw->vertex_count;
    gen8.width = (v9x_u16)target->width;
    gen8.height = (v9x_u16)target->height;
    gen8.x0 = (v9x_u16)target->left;
    gen8.y0 = (v9x_u16)target->top;
    gen8.x1 = (v9x_u16)(target->right - 1);
    gen8.y1 = (v9x_u16)(target->bottom - 1);
    if (draw->depth_flags) {
        gen8.depth = lfb_gfx + draw->depth_offset;
        gen8.depth_pitch = draw->depth_pitch;
        gen8.depth_format = V9X_GEN8_DEPTH_D16_UNORM;
        gen8.depth_test = 1;
        gen8.depth_write = (draw->depth_flags & WGPU_NT_DEPTH_WRITE) != 0;
        gen8.depth_func = (v9x_u8)(draw->depth_compare & 7);
        if (draw->depth_flags & WGPU_NT_DEPTH_ONLY) gen8.fragment.write_disable = 15;
    }
    if (draw->fragment_flags & WGPU_NT_BLEND_ENABLE) {
        gen8.fragment.enable = 1;
        gen8.fragment.src = blend_factors[draw->blend_src];
        gen8.fragment.dst = blend_factors[draw->blend_dst];
    }
    if (draw->fragment_flags & WGPU_NT_ALPHA_TEST) {
        gen8.fragment.alpha_test = 1;
        gen8.fragment.alpha_func = (v9x_u8)(draw->alpha_compare & 7);
    }

#ifdef WGPU_NT_BATCH_TEMPLATES
    if (!build_material_commands(&batch, command, &gen8, cache)) return 0;
#else
    v9x_intel_hsw_batch_init(&batch, command, 512);
    if (v9x_intel_gen8_draw(&batch, &gen8) != V9X_STATUS_OK) return 0;
#endif
    if (next_gfx) {
        v9x_u32 end = batch.used;
        if (end && command[end - 1] == 0) --end;
        if (!end || command[end - 1] != 0x05000000ul || end - 1 + 16 > 512)
            return 0;
        v9x_wgpu_chain_tail(command + end - 1, next_gfx);
        batch.used = end - 1 + 16;
    }
    if (v9x_intel_gen8_surface_2d(state, target->bpp == 16 ? V9X_GEN8_FMT_B5G6R5_UNORM :
            V9X_GEN8_FMT_B8G8R8A8_UNORM, gen8.width, gen8.height, target->pitch,
            lfb_gfx + target->offset, 1, 1) != V9X_STATUS_OK) return 0;
    write_words(slot + TARGET_STATE / 4, state, 16);
    slot[BINDING / 4] = TARGET_STATE;
    if (gen8.samplers) {
        v9x_u8 levels = texture->reserved ? (v9x_u8)(texture->reserved >> 8) : 1;
        v9x_u8 mip = (v9x_u8)(texture->reserved & 255u);
        if (v9x_intel_gen8_surface_2d(state, formats[draw->format], (v9x_u16)texture->width,
                (v9x_u16)texture->height, texture->pitch, lfb_gfx + texture->offset,
                levels, 0) != V9X_STATUS_OK) return 0;
        write_words(slot + TEXTURE_STATE / 4, state, 16);
        slot[BINDING / 4 + 1] = TEXTURE_STATE;
        if (v9x_intel_gen8_sampler(state, (v9x_u8)draw->min_linear, (v9x_u8)draw->mag_linear,
                mip, draw->clamp ? V9X_GEN8_TC_CLAMP : V9X_GEN8_TC_WRAP,
                levels, 0) != V9X_STATUS_OK) return 0;
        write_words(slot + SAMPLER / 4, state, 4);
    }
    if (v9x_intel_gen8_blend(state, &gen8.fragment) != V9X_STATUS_OK) return 0;
    write_words(slot + BLEND_STATE / 4, state, 3);
    if (v9x_intel_gen8_cc(state, (v9x_u8)draw->alpha_ref) != V9X_STATUS_OK) return 0;
    write_words(slot + COLOR_CALC / 4, state, 6);
    v9x_intel_gen8_viewports(viewport, state);
    write_words(slot + CC_VIEWPORT / 4, viewport, 2);
    write_words(slot + SF_VIEWPORT / 4, state, 16);
#ifdef WGPU_NT_BULK_COPY
    copy_aperture_words(slot + VERTICES / 4, draw->vertices,
                       draw->vertex_count * sizeof(WgpuNtTlVertex) / sizeof(v9x_u32));
#else
    for (v9x_u32 i = 0; i < draw->vertex_count; ++i) {
        const WgpuNtTlVertex *vertex = &draw->vertices[i];
        volatile v9x_u32 *out = slot + VERTICES / 4 + i * 8;
        out[0] = vertex->x; out[1] = vertex->y;
        out[2] = vertex->z; out[3] = vertex->rhw;
        out[4] = vertex->diffuse; out[5] = vertex->specular;
        out[6] = vertex->u; out[7] = vertex->v;
    }
#endif
    if (code_words) {
        write_words(memory + kernel_offset / 4, code, code_words);
        if (cache) {
            /* The cache marker can outlive this request, even if a later
             * entry fails validation before the usual submit fence. */
            __asm__ __volatile__("sfence" ::: "memory");
            cache->shaders |= shader_bit;
        }
    }
    write_words(slot, command, batch.used);
    return VERTICES + draw->vertex_count * sizeof(WgpuNtTlVertex);
}

static void wgpu_nt_material_descriptor(volatile WgpuNtU32 *memory,
    WgpuNtU32 batch, WgpuNtU32 used, WgpuNtU32 fence, WgpuNtU32 flags)
{
    memory[WGPU_NT_DESCRIPTOR / 4] = batch;
    memory[WGPU_NT_DESCRIPTOR / 4 + 1] = used;
    memory[WGPU_NT_DESCRIPTOR / 4 + 2] = fence;
    memory[WGPU_NT_DESCRIPTOR / 4 + 3] = flags;
}

WgpuNtU32 wgpu_nt_build_material(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags,
    const WgpuNtMaterialDraw *draw, WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx,
    WgpuNtMaterialCache *cache)
{
    WgpuNtU32 used;
    if (!memory || bytes < WGPU_NT_TRI_MIN || !fence || flags > 1 ||
        (gfx & 4095u) || gfx > 0xffffffffu - bytes) return 0;
    used = wgpu_nt_build_material_slot(memory, gfx, WGPU_NT_TRI_BATCH,
        WGPU_NT_TRI_KERNEL, 0, draw, lfb_bytes, lfb_gfx,
        bytes >= WGPU_NT_MATERIAL_SHADER_END ? cache : NULL);
    if (used) {
        memory[0] = completed;
        wgpu_nt_material_descriptor(memory, WGPU_NT_TRI_BATCH, used, fence, flags);
    }
    return used;
}

int wgpu_nt_prepare_material_group(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, unsigned arena, const WgpuNtMaterialGroup *group,
    WgpuNtU32 input_bytes, WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx,
    WgpuNtMaterialCache *cache, WgpuNtMaterialPrepared *prepared)
{
    WgpuNtU32 i, first_used = 0, first, slot;
    const unsigned char *entry;
    if (!memory || !prepared || arena >= WGPU_NT_MATERIAL_ARENAS ||
        (gfx & 4095u) || gfx > 0xffffffffu - bytes ||
        !wgpu_nt_material_group_valid(group, input_bytes)) return 0;
    first = arena ? WGPU_NT_MATERIAL_ALT_GROUP_FIRST +
        (arena - 1) * WGPU_NT_MATERIAL_GROUP_MAX * WGPU_NT_MATERIAL_GROUP_STRIDE :
        WGPU_NT_MATERIAL_GROUP_FIRST;
    slot = first;
    if (bytes < slot + group->count * WGPU_NT_MATERIAL_GROUP_STRIDE) return 0;
    entry = (const unsigned char *)(group + 1);
    for (i = 0; i < group->count; ++i) {
        const WgpuNtMaterialDraw *draw = (const void *)entry;
        WgpuNtU32 next = slot + WGPU_NT_MATERIAL_GROUP_STRIDE;
        WgpuNtU32 used = wgpu_nt_build_material_slot(memory, gfx, slot,
            slot + WGPU_NT_TRI_BATCH_BYTES, i + 1 < group->count ? gfx + next : 0,
            draw, lfb_bytes, lfb_gfx,
            bytes >= WGPU_NT_MATERIAL_SHADER_END ? cache : NULL);
        if (!used) return 0;
        if (!i) first_used = used;
        slot = next;
        entry += wgpu_nt_material_bytes(draw->vertex_count);
    }
    prepared->offset = first;
    prepared->bytes = first_used;
    return 1;
}

void wgpu_nt_publish_material_group(volatile WgpuNtU32 *memory,
    const WgpuNtMaterialPrepared *prepared, WgpuNtU32 completed,
    WgpuNtU32 fence, WgpuNtU32 flags)
{
    memory[0] = completed;
    wgpu_nt_material_descriptor(memory, prepared->offset, prepared->bytes, fence, flags);
}

#ifdef WGPU_NT_QUEUED_GROUPS
void wgpu_nt_queue_material_descriptor(volatile WgpuNtU32 *memory,
    const WgpuNtMaterialPrepared *prepared, WgpuNtU32 fence, WgpuNtU32 flags)
{
    /* The running GPU owns the completion word until the queue drains. */
    wgpu_nt_material_descriptor(memory, prepared->offset, prepared->bytes, fence, flags);
}
#endif

WgpuNtU32 wgpu_nt_build_material_group(volatile WgpuNtU32 *memory, WgpuNtU32 bytes,
    WgpuNtU32 gfx, WgpuNtU32 completed, WgpuNtU32 fence, WgpuNtU32 flags,
    const WgpuNtMaterialGroup *group, WgpuNtU32 input_bytes,
    WgpuNtU32 lfb_bytes, WgpuNtU32 lfb_gfx, WgpuNtMaterialCache *cache)
{
    WgpuNtMaterialPrepared prepared;
    if (!fence || flags > 1 || !wgpu_nt_prepare_material_group(memory, bytes, gfx,
            0, group, input_bytes, lfb_bytes, lfb_gfx, cache, &prepared))
        return 0;
    wgpu_nt_publish_material_group(memory, &prepared, completed, fence, flags);
    return prepared.bytes;
}
