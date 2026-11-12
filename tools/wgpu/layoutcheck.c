/* The IGT state structures as wgputest.c fills them, dumped byte by byte, so
 * that Open Watcom's bit-field layout can be compared with clang's. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "igt/gen8_render.h"

static void dump(const char *name, const void *p, unsigned n)
{
    const uint8_t *b = p;
    printf("%s %u:", name, n);
    for (unsigned i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
}

int main(void)
{
    struct gen8_surface_state ss;
    memset(&ss, 0, sizeof ss);
    ss.ss0.surface_type = SURFACE_2D;
    ss.ss0.surface_format = SURFACEFORMAT_B5G6R5_UNORM;
    ss.ss0.render_cache_read_write = 1;
    ss.ss0.vertical_alignment = 1;
    ss.ss0.horizontal_alignment = 1;
    ss.ss1.memory_object_control = CHV_MOCS_WB | CHV_MOCS_L3;
    ss.ss8.base_addr = 0x5401a000u;
    ss.ss2.height = 479;
    ss.ss2.width = 639;
    ss.ss3.pitch = 1279;
    ss.ss7.shader_chanel_select_r = 4;
    ss.ss7.shader_chanel_select_g = 5;
    ss.ss7.shader_chanel_select_b = 6;
    ss.ss7.shader_chanel_select_a = 7;
    dump("surface", &ss, sizeof ss);
    struct gen8_blend_state bs;
    memset(&bs, 0, sizeof bs);
    for (int i = 0; i < 16; i++) {
        bs.bs[i].dest_blend_factor = GEN6_BLENDFACTOR_ZERO;
        bs.bs[i].source_blend_factor = GEN6_BLENDFACTOR_ONE;
        bs.bs[i].color_blend_func = GEN6_BLENDFUNCTION_ADD;
        bs.bs[i].pre_blend_color_clamp = 1;
    }
    dump("blend", &bs, sizeof bs);
    struct gen4_cc_viewport cc;
    memset(&cc, 0, sizeof cc);
    cc.min_depth = -1.e35f;
    cc.max_depth = 1.e35f;
    dump("cc_viewport", &cc, sizeof cc);
    struct gen7_sf_clip_viewport vp;
    memset(&vp, 0, sizeof vp);
    vp.guardband.xmin = 0; vp.guardband.xmax = 1.0f;
    vp.guardband.ymin = 0; vp.guardband.ymax = 1.0f;
    dump("sf_clip", &vp, sizeof vp);
    printf("sizes cc_state %u scissor %u\n", (unsigned)sizeof(struct gen6_color_calc_state),
           (unsigned)sizeof(struct gen6_scissor_rect));
    return 0;
}
