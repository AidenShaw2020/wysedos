/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Voodoo Graphics, 2, Banshee, 3 emulation.
 *
 * Authors: Sarah Walker, <https://pcem-emulator.co.uk/>
 *          leilei
 *
 *          Copyright 2008-2020 Sarah Walker.
 */
#include "../voodoo_port.h"
#include <86box/vid_voodoo_common.h>
rgba8_t rgb332[0x100];
rgba8_t ai44[0x100];
rgba8_t rgb565[0x10000];
rgba8_t argb1555[0x10000];
rgba8_t bgr565[0x10000];
rgba8_t abgr1555[0x10000];
rgba8_t rgba1555[0x10000];
rgba8_t bgra1555[0x10000];
rgba8_t argb4444[0x10000];
rgba8_t ai88[0x10000];
void voodoo_port_tables_init(void)
{
    int c;
    for (c = 0; c < 0x100; c++) {
        rgb332[c].r = c & 0xe0;
        rgb332[c].g = (c << 3) & 0xe0;
        rgb332[c].b = (c << 6) & 0xc0;
        rgb332[c].r = rgb332[c].r | (rgb332[c].r >> 3) | (rgb332[c].r >> 6);
        rgb332[c].g = rgb332[c].g | (rgb332[c].g >> 3) | (rgb332[c].g >> 6);
        rgb332[c].b = rgb332[c].b | (rgb332[c].b >> 2);
        rgb332[c].b = rgb332[c].b | (rgb332[c].b >> 4);
        rgb332[c].a = 0xff;

        ai44[c].a = (c & 0xf0) | ((c & 0xf0) >> 4);
        ai44[c].r = (c & 0x0f) | ((c & 0x0f) << 4);
        ai44[c].g = ai44[c].b = ai44[c].r;
    }

    for (c = 0; c < 0x10000; c++) {
        rgb565[c].r = (c >> 8) & 0xf8;
        rgb565[c].g = (c >> 3) & 0xfc;
        rgb565[c].b = (c << 3) & 0xf8;
        rgb565[c].r |= (rgb565[c].r >> 5);
        rgb565[c].g |= (rgb565[c].g >> 6);
        rgb565[c].b |= (rgb565[c].b >> 5);
        rgb565[c].a = 0xff;

        argb1555[c].r = (c >> 7) & 0xf8;
        argb1555[c].g = (c >> 2) & 0xf8;
        argb1555[c].b = (c << 3) & 0xf8;
        argb1555[c].r |= (argb1555[c].r >> 5);
        argb1555[c].g |= (argb1555[c].g >> 5);
        argb1555[c].b |= (argb1555[c].b >> 5);
        argb1555[c].a = (c & 0x8000) ? 0xff : 0;

        bgr565[c].b = (c >> 8) & 0xf8;
        bgr565[c].g = (c >> 3) & 0xfc;
        bgr565[c].r = (c << 3) & 0xf8;
        bgr565[c].b |= (bgr565[c].b >> 5);
        bgr565[c].g |= (bgr565[c].g >> 6);
        bgr565[c].r |= (bgr565[c].r >> 5);
        bgr565[c].a = 0xff;

        abgr1555[c].b = (c >> 7) & 0xf8;
        abgr1555[c].g = (c >> 2) & 0xf8;
        abgr1555[c].r = (c << 3) & 0xf8;
        abgr1555[c].b |= (abgr1555[c].b >> 5);
        abgr1555[c].g |= (abgr1555[c].g >> 5);
        abgr1555[c].r |= (abgr1555[c].r >> 5);
        abgr1555[c].a = (c & 0x8000) ? 0xff : 0;

        rgba1555[c].r = (c >> 8) & 0xf8;
        rgba1555[c].g = (c >> 3) & 0xf8;
        rgba1555[c].b = (c << 2) & 0xf8;
        rgba1555[c].r |= (rgba1555[c].r >> 5);
        rgba1555[c].g |= (rgba1555[c].g >> 5);
        rgba1555[c].b |= (rgba1555[c].b >> 5);
        rgba1555[c].a = (c & 1) ? 0xff : 0;

        bgra1555[c].b = (c >> 8) & 0xf8;
        bgra1555[c].g = (c >> 3) & 0xf8;
        bgra1555[c].r = (c << 2) & 0xf8;
        bgra1555[c].b |= (bgra1555[c].b >> 5);
        bgra1555[c].g |= (bgra1555[c].g >> 5);
        bgra1555[c].r |= (bgra1555[c].r >> 5);
        bgra1555[c].a = (c & 1) ? 0xff : 0;

        argb4444[c].a = (c >> 8) & 0xf0;
        argb4444[c].r = (c >> 4) & 0xf0;
        argb4444[c].g = c & 0xf0;
        argb4444[c].b = (c << 4) & 0xf0;
        argb4444[c].a |= (argb4444[c].a >> 4);
        argb4444[c].r |= (argb4444[c].r >> 4);
        argb4444[c].g |= (argb4444[c].g >> 4);
        argb4444[c].b |= (argb4444[c].b >> 4);

        ai88[c].a = (c >> 8);
        ai88[c].r = c & 0xff;
        ai88[c].g = c & 0xff;
        ai88[c].b = c & 0xff;
    }
}
