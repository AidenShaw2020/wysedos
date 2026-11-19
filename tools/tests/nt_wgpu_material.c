/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/nt/wgpu_material.h"
#include "velocity9x/intel_gen8_3d.h"

static WgpuNtU32 aperture[WGPU_NT_TRI_MIN / sizeof(WgpuNtU32)];
static WgpuNtMaterialDraw draw;

#ifdef WGPU_NT_BATCH_TEMPLATES
static void check_relocated_template(void)
{
    struct { WgpuNtMaterialGroup group; WgpuNtMaterialDraw draw; } packet = {0};
    enum { BYTES = WGPU_NT_MATERIAL_ALT_GROUP_FIRST + WGPU_NT_MATERIAL_GROUP_STRIDE };
    static WgpuNtU32 cached_aperture[BYTES / 4], reference_aperture[BYTES / 4];
    WgpuNtMaterialCache cached = {0}, reference = {0};
    WgpuNtMaterialPrepared actual, expected;
    const WgpuNtU32 gfx = 0x10000000;
    packet.draw = draw;
    packet.group.count = 1;
    packet.group.size = sizeof(packet.group) + wgpu_nt_material_bytes(draw.vertex_count);
    cached.templates = calloc(1, wgpu_nt_template_cache_size());
    assert(cached.templates);
    assert(wgpu_nt_prepare_material_group(cached_aperture, BYTES, gfx, 0,
        &packet.group, packet.group.size, 0x200000, 0x20000000, &cached, &actual));
    /* Exercise a template hit with a different slot and number of vertices. */
    memcpy(packet.draw.vertices + draw.vertex_count, draw.vertices,
           draw.vertex_count * sizeof(draw.vertices[0]));
    packet.draw.vertex_count = 2 * draw.vertex_count;
    packet.group.size = sizeof(packet.group) + wgpu_nt_material_bytes(packet.draw.vertex_count);
    assert(wgpu_nt_prepare_material_group(cached_aperture, BYTES, gfx, 1,
        &packet.group, packet.group.size, 0x200000, 0x20000000, &cached, &actual));
    assert(wgpu_nt_prepare_material_group(reference_aperture, BYTES, gfx, 1,
        &packet.group, packet.group.size, 0x200000, 0x20000000, &reference, &expected));
    assert(actual.offset == expected.offset && actual.bytes == expected.bytes);
    assert(cached.template_stats.lookups == 2 && cached.template_stats.hits == 1);
    assert(!memcmp(cached_aperture + actual.offset / 4, reference_aperture + expected.offset / 4,
                   actual.bytes));
    free(cached.templates);
}
#endif

static void check_material_shader_cache(void)
{
    const WgpuNtU32 gfx = 0x10000000;
    static WgpuNtU32 arena[WGPU_NT_MATERIAL_SHADER_END / sizeof(WgpuNtU32)];
    const WgpuNtU32 index = 2 * WGPU_NT_TEXTURE_MODULATE;
    const WgpuNtU32 offset = WGPU_NT_MATERIAL_SHADER_FIRST +
        index * WGPU_NT_MATERIAL_SHADER_BYTES;
    WgpuNtU32 expected[WGPU_NT_MATERIAL_SHADER_BYTES / sizeof(WgpuNtU32)];
    WgpuNtMaterialCache cache = {0};
    memset(aperture, 0xa5, sizeof(aperture));
    memset(arena, 0xa5, sizeof(arena));
    assert(wgpu_nt_build_material(aperture, sizeof(aperture), gfx, 4, 5, 1,
        &draw, 0x200000, 0x20000000, &cache));
    assert(!memcmp(aperture + (WGPU_NT_TRI_BATCH + 0x8000) / 4, draw.vertices,
                   draw.vertex_count * sizeof(draw.vertices[0])));
    assert(cache.shaders == 0); /* Small apertures still use their private kernel slot. */
    memcpy(expected, aperture + WGPU_NT_TRI_KERNEL / 4, sizeof(expected));
    assert(wgpu_nt_build_material(arena, sizeof(arena), gfx, 4, 5, 1,
        &draw, 0x200000, 0x20000000, &cache));
    assert(cache.shaders == (1ul << index));
    assert(!memcmp(expected, arena + offset / 4, sizeof(expected)));
#ifdef WGPU_NT_BATCH_TEMPLATES
    check_relocated_template();
#endif
    assert(arena[WGPU_NT_TRI_KERNEL / 4] == 0xa5a5a5a5u);
    draw.fragment_flags = WGPU_NT_SPECULAR_ENABLE;
    assert(wgpu_nt_build_material(arena, sizeof(arena), gfx, 5, 6, 1,
        &draw, 0x200000, 0x20000000, &cache));
    assert(cache.shaders == (3ul << index));
    assert(memcmp(expected, arena + (offset + WGPU_NT_MATERIAL_SHADER_BYTES) / 4,
                  sizeof(expected)));
    draw.fragment_flags = 0;
    assert(wgpu_nt_build_material(arena, sizeof(arena), gfx, 6, 7, 1,
        &draw, 0x200000, 0x20000000, &cache));
    assert(cache.shaders == (3ul << index));
    assert(!memcmp(expected, arena + offset / 4, sizeof(expected)));
}

static void check_material_group(void)
{
    const WgpuNtU32 gfx = 0x10000000;
    const WgpuNtU32 first = WGPU_NT_MATERIAL_GROUP_FIRST;
    const WgpuNtU32 stride = WGPU_NT_MATERIAL_GROUP_STRIDE;
    const WgpuNtU32 entry_bytes = wgpu_nt_material_bytes(draw.vertex_count);
    const WgpuNtU32 alternate = WGPU_NT_MATERIAL_ALT_GROUP_FIRST;
    static WgpuNtU32 arena[(WGPU_NT_MATERIAL_ALT_GROUP_FIRST +
        (WGPU_NT_MATERIAL_ARENAS - 1) * WGPU_NT_MATERIAL_GROUP_MAX *
            WGPU_NT_MATERIAL_GROUP_STRIDE) / sizeof(WgpuNtU32)];
    static WgpuNtU32 original[WGPU_NT_MATERIAL_ALT_GROUP_FIRST / sizeof(WgpuNtU32)];
    WgpuNtMaterialPrepared prepared;
    struct {
        WgpuNtMaterialGroup header;
        WgpuNtU32 payload[WGPU_NT_MATERIAL_GROUP_MAX * 264 / sizeof(WgpuNtU32)];
    } packet;
    WgpuNtU32 i, j;
    assert(entry_bytes == 264);
    packet.header.size = sizeof(packet);
    packet.header.count = WGPU_NT_MATERIAL_GROUP_MAX;
    for (i = 0; i < packet.header.count; ++i)
        memcpy((unsigned char *)packet.payload + i * entry_bytes, &draw, entry_bytes);
    assert(wgpu_nt_material_group_valid(&packet.header, sizeof(packet)));
    assert(!wgpu_nt_material_group_valid(&packet.header, sizeof(packet) - 1));
    ++packet.header.count;
    assert(!wgpu_nt_material_group_valid(&packet.header, sizeof(packet)));
    --packet.header.count;
    memset(arena, 0xa5, sizeof(arena));
    assert(!wgpu_nt_build_material_group(arena, first + stride * packet.header.count - 1,
        gfx, 4, 5, 1, &packet.header, sizeof(packet), 0x200000, 0x20000000, NULL));
    assert(arena[WGPU_NT_DESCRIPTOR / 4] == 0xa5a5a5a5u);
    assert(wgpu_nt_build_material_group(arena, sizeof(arena), gfx, 4, 5, 1,
        &packet.header, sizeof(packet), 0x200000, 0x20000000, NULL));
    assert(arena[0] == 4 && arena[WGPU_NT_DESCRIPTOR / 4] == first);
    assert(arena[WGPU_NT_DESCRIPTOR / 4 + 2] == 5);
    for (i = 0; i < packet.header.count; ++i) {
        const WgpuNtU32 *commands = arena + (first + i * stride) / 4;
        const WgpuNtU32 tail = i + 1 < packet.header.count ? 0x18800001u : 0x05000000u;
        for (j = 0; j < 510 && commands[j] != tail; ++j) { }
        assert(j < 510);
        if (i + 1 < packet.header.count)
            assert(commands[j + 1] == gfx + first + (i + 1) * stride && commands[j + 2] == 0);
        assert(commands[WGPU_NT_TRI_BATCH_BYTES / 4] != 0xa5a5a5a5u);
    }
    assert(arena[(first + stride * packet.header.count) / 4] == 0xa5a5a5a5u);
    memcpy(original, arena, sizeof(original));
    assert(!wgpu_nt_prepare_material_group(arena, alternate + packet.header.count * stride - 1, gfx, 1,
        &packet.header, sizeof(packet), 0x200000, 0x20000000, NULL, &prepared));
    assert(!wgpu_nt_prepare_material_group(arena, sizeof(arena), gfx, WGPU_NT_MATERIAL_ARENAS,
        &packet.header, sizeof(packet), 0x200000, 0x20000000, NULL, &prepared));
    assert(wgpu_nt_prepare_material_group(arena, sizeof(arena), gfx, 1,
        &packet.header, sizeof(packet), 0x200000, 0x20000000, NULL, &prepared));
    assert(prepared.offset == alternate && prepared.bytes == arena[WGPU_NT_DESCRIPTOR / 4 + 1]);
    assert(!memcmp(original, arena, sizeof(original)));
    for (i = 0; i < packet.header.count; ++i) {
        const WgpuNtU32 *commands = arena + (alternate + i * stride) / 4;
        const WgpuNtU32 tail = i + 1 < packet.header.count ? 0x18800001u : 0x05000000u;
        for (j = 0; j < 510 && commands[j] != tail; ++j) { }
        assert(j < 510);
        if (i + 1 < packet.header.count)
            assert(commands[j + 1] == gfx + alternate + (i + 1) * stride);
    }
    wgpu_nt_publish_material_group(arena, &prepared, 5, 6, 1);
    assert(arena[0] == 5 && arena[WGPU_NT_DESCRIPTOR / 4] == alternate);
    assert(arena[WGPU_NT_DESCRIPTOR / 4 + 1] == prepared.bytes);
    assert(arena[WGPU_NT_DESCRIPTOR / 4 + 2] == 6);
    assert(arena[WGPU_NT_DESCRIPTOR / 4 + 3] == 1);
#ifdef WGPU_NT_QUEUED_GROUPS
    for (i = 2; i < WGPU_NT_MATERIAL_ARENAS; ++i) {
        WgpuNtU32 offset = alternate + (i - 1) * packet.header.count * stride;
        WgpuNtU32 prior_tail = arena[offset / 4 - 1];
        assert(wgpu_nt_prepare_material_group(arena, sizeof(arena), gfx, i,
            &packet.header, sizeof(packet), 0x200000, 0x20000000, NULL, &prepared));
        assert(prepared.offset == offset && arena[offset / 4 - 1] == prior_tail);
        wgpu_nt_queue_material_descriptor(arena, &prepared, 6 + i, 1);
        assert(arena[0] == 5 && arena[WGPU_NT_DESCRIPTOR / 4] == offset);
        assert(arena[WGPU_NT_DESCRIPTOR / 4 + 2] == 6 + i);
    }
    assert(!wgpu_nt_fence_reached(0, 0xffffffffu));
    assert(!wgpu_nt_fence_reached(4, 5));
    assert(wgpu_nt_fence_reached(5, 5));
    assert(wgpu_nt_fence_reached(7, 5));
    assert(wgpu_nt_fence_reached(1, 0xffffffffu));
    assert(!wgpu_nt_fence_reached(0xffffffffu, 1));
#endif
    memcpy(arena, original, sizeof(original));
    /* A malformed later entry must never publish a new descriptor. */
    packet.payload[(entry_bytes + WGPU_NT_MATERIAL_HEADER_BYTES) / 4] = 0x7fc00000u;
    prepared.offset = 0;
    prepared.bytes = 0;
    assert(!wgpu_nt_prepare_material_group(arena, sizeof(arena), gfx, 1,
        &packet.header, sizeof(packet), 0x200000, 0x20000000, NULL, &prepared));
    assert(!prepared.offset && !prepared.bytes);
    assert(!memcmp(original, arena, sizeof(original)));
    assert(!wgpu_nt_build_material_group(arena, sizeof(arena), gfx, 5, 6, 1,
        &packet.header, sizeof(packet), 0x200000, 0x20000000, NULL));
    assert(arena[0] == 4 && arena[WGPU_NT_DESCRIPTOR / 4 + 2] == 5);
}

static void check_depth_layout(void)
{
    enum { PITCH = 256, ROWS = 32, WORDS = PITCH * ROWS / 2 };
    v9x_u16 linear[WORDS], tiled[WORDS];
    unsigned x, y;
    for (y = 0; y < WORDS; ++y) linear[y] = (v9x_u16)y;
    assert(v9x_intel_gen8_depth_band(tiled, linear, PITCH, 0) == V9X_STATUS_OK);
    for (y = 0; y < ROWS; ++y) {
        for (x = 0; x < PITCH / 2; ++x) {
            WgpuNtU32 index = wgpu_nt_depth_word(PITCH, x, y);
            assert(index < WORDS && tiled[index] == linear[y * PITCH / 2 + x]);
            assert(wgpu_nt_depth_word(PITCH, x, y + ROWS) == index + WORDS);
        }
    }
}

static void reject(void)
{
    size_t i;
    memset(aperture, 0xa5, sizeof(aperture));
    assert(!wgpu_nt_build_material(aperture, sizeof(aperture), 0x10000000, 4, 5, 1,
                                   &draw, 0x200000, 0x20000000, NULL));
    for (i = 0; i < sizeof(aperture) / sizeof(aperture[0]); ++i)
        assert(aperture[i] == 0xa5a5a5a5u);
}

static void check_mip_material(void)
{
    const WgpuNtU32 gfx = 0x10000000, framebuffer = 0x20000000;
    WgpuNtMaterialDraw mip = draw;
    WgpuNtMipLayout layout;
    struct v9x_hsw_miptree reference;
    v9x_u32 expected[16];
    WgpuNtU32 i, levels;

    for (levels = 2; levels <= 9; ++levels) {
        assert(wgpu_nt_mip_layout(32, 256, levels, 2, &layout));
        assert(v9x_intel_gen8_miptree(32, 256, (v9x_u8)levels, 2, &reference)
               == V9X_STATUS_OK);
        assert(layout.pitch == reference.pitch && layout.rows == reference.rows);
        for (i = 0; i < levels; ++i) assert(layout.offset[i] == reference.level_offset[i]);
    }
    assert(wgpu_nt_mip_layout(32, 32, 6, 2, &layout));
    mip.texture.pitch = layout.pitch;
    mip.texture.reserved = (6u << 8) | WGPU_NT_MIP_LINEAR;
    assert(wgpu_nt_build_material(aperture, sizeof(aperture), gfx, 4, 5, 1,
                                  &mip, 0x200000, framebuffer, NULL));
    assert(v9x_intel_gen8_surface_2d(expected, V9X_GEN8_FMT_B5G6R5_UNORM,
        32, 32, layout.pitch, framebuffer + mip.texture.offset, 6, 0) == V9X_STATUS_OK);
    assert(!memcmp(aperture + (WGPU_NT_TRI_BATCH + 0x4080) / 4, expected, sizeof(expected)));
    assert(v9x_intel_gen8_sampler(expected, 0, 0, V9X_GEN8_MIP_LINEAR,
        V9X_GEN8_TC_WRAP, 6, 0) == V9X_STATUS_OK);
    assert(!memcmp(aperture + (WGPU_NT_TRI_BATCH + 0x4200) / 4, expected, 4 * sizeof(*expected)));
    assert(!wgpu_nt_material_valid(&mip, mip.texture.offset + layout.pitch * layout.rows - 1,
                                   framebuffer));
    mip.texture.reserved = (7u << 8) | WGPU_NT_MIP_NEAREST;
    assert(!wgpu_nt_material_valid(&mip, 0x200000, framebuffer));
}

int main(void)
{
    WgpuNtU32 format, combine, bytes;
    WgpuNtU32 *descriptor = aperture + WGPU_NT_DESCRIPTOR / 4;
    check_depth_layout();
    draw.target = (WgpuNtFill){
        .size = sizeof(WgpuNtFill), .abi_version = WGPU_NT_ABI_VERSION,
        .pitch = 512, .width = 128, .height = 128, .bpp = 32, .right = 128, .bottom = 128
    };
    draw.texture = (WgpuNtFill){
        .size = sizeof(WgpuNtFill), .abi_version = WGPU_NT_ABI_VERSION,
        .offset = 0x10000, .pitch = 128, .width = 32, .height = 32,
        .bpp = 16, .right = 32, .bottom = 32
    };
    draw.size = sizeof(draw);
    draw.vertex_count = 3;
    draw.vertices[0] = (WgpuNtTlVertex){
        .x = 0x41800000, .y = 0x41800000, .z = 0x3f000000,
        .rhw = 0x3f800000, .diffuse = 0xffffffff
    };
    draw.vertices[1] = draw.vertices[0];
    draw.vertices[1].x = 0x42000000;
    draw.vertices[1].u = 0x3f800000;
    draw.vertices[2] = draw.vertices[0];
    draw.vertices[2].y = 0x42000000;
    draw.vertices[2].v = 0x3f800000;
    bytes = wgpu_nt_material_bytes(draw.vertex_count);
    assert(bytes == 264);
    assert(wgpu_nt_material_packet_valid(&draw, bytes));
    assert(wgpu_nt_material_packet_valid(&draw, sizeof(draw)));
    assert(!wgpu_nt_material_packet_valid(NULL, sizeof(draw)));
    assert(!wgpu_nt_material_packet_valid(&draw, WGPU_NT_MATERIAL_HEADER_BYTES - 1));
    assert(!wgpu_nt_material_packet_valid(&draw, bytes - 1));
    assert(!wgpu_nt_material_packet_valid(&draw, bytes + 1));
    draw.format = WGPU_NT_RGB565;
    draw.combine = WGPU_NT_TEXTURE_MODULATE;
    check_mip_material();
    check_material_group();
    check_material_shader_cache();
    draw.vertex_count = WGPU_NT_MATERIAL_MAX_VERTICES + 3;
    assert(!wgpu_nt_material_packet_valid(&draw, sizeof(draw)));
    draw.vertex_count = 3;
    for (format = WGPU_NT_RGB565; format <= WGPU_NT_ARGB8888; ++format) {
        draw.format = format;
        draw.texture.bpp = format >= WGPU_NT_XRGB8888 ? 32 : 16;
        for (combine = WGPU_NT_TEXTURE_REPLACE; combine <= WGPU_NT_TEXTURE_DECAL_ALPHA; ++combine) {
            draw.combine = combine;
            memset(aperture, 0xa5, sizeof(aperture));
            bytes = wgpu_nt_build_material(aperture, sizeof(aperture), 0x10000000, 4, 5, 1,
                                          &draw, 0x200000, 0x20000000, NULL);
            assert(bytes > 0 && bytes <= WGPU_NT_TRI_BATCH_BYTES);
            assert(aperture[0] == 4 && descriptor[0] == WGPU_NT_TRI_BATCH);
            assert(descriptor[1] == bytes && descriptor[2] == 5 && descriptor[3] == 1);
            assert(aperture[(WGPU_NT_TRI_BATCH + bytes) / 4] == 0xa5a5a5a5u);
        }
    }
    draw.texture.offset = 0xffc0; /* Last target cache line overlaps the texture. */
    reject();
    draw.texture.offset = 0x10000;
    draw.vertices[2].u = 0x7fc00000;
    reject();
    draw.vertices[2].u = 0;
    draw.vertex_count = WGPU_NT_MATERIAL_MAX_VERTICES + 3;
    reject();
    draw.vertex_count = 3;
    draw.texture.pitch = 65;
    reject();
    draw.target.width = draw.target.right = 64;
    draw.texture.pitch = draw.target.pitch;
    draw.texture.offset = 256;
    assert(wgpu_nt_build_material(aperture, sizeof(aperture), 0x10000000, 4, 5, 1,
                                  &draw, 0x200000, 0x20000000, NULL));
    draw.texture.offset = 192;
    reject();
    draw.texture.offset = 448;
    reject();
    draw.texture.offset = 256;
    draw.fragment_flags = WGPU_NT_BLEND_ENABLE | WGPU_NT_ALPHA_TEST | WGPU_NT_SPECULAR_ENABLE;
    draw.blend_src = 5; /* SRCALPHA */
    draw.blend_dst = 6; /* INVSRCALPHA */
    draw.alpha_compare = 5; /* GREATER */
    draw.alpha_ref = 127;
    assert(wgpu_nt_build_material(aperture, sizeof(aperture), 0x10000000, 4, 5, 1,
                                  &draw, 0x200000, 0x20000000, NULL));
    draw.combine = WGPU_NT_TEXTURE_DISABLED;
    draw.format = 0;
    assert(wgpu_nt_build_material(aperture, sizeof(aperture), 0x10000000, 4, 5, 1,
                                  &draw, 0x200000, 0x20000000, NULL));
    draw.alpha_compare = 0;
    reject();
    draw.fragment_flags = draw.alpha_ref = 0;
    draw.depth_offset = 0x20000;
    draw.depth_pitch = 128;
    draw.depth_flags = WGPU_NT_DEPTH_TEST | WGPU_NT_DEPTH_WRITE | WGPU_NT_DEPTH_ONLY;
    draw.depth_compare = WGPU_NT_COMPARE_MAX;
    assert(wgpu_nt_build_material(aperture, sizeof(aperture), 0x10000000, 4, 5, 1,
                                  &draw, 0x200000, 0x20000000, NULL));
    draw.depth_offset = 0x20040;
    reject();
    draw.depth_offset = 0x1ff000;
    reject();
    draw.depth_offset = 0x1000;
    reject();
    puts("PASS: Gen8 material batches, fences, allocation bounds and invalid packet isolation");
    return 0;
}
