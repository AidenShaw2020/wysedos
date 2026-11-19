/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wgpu_nt.h"

static int near_channel(unsigned actual, int numerator, unsigned maximum)
{
    int wanted = (numerator * (int)maximum + 224) / 448;
    int difference = (int)actual - wanted;
    return difference >= -2 && difference <= 2;
}

static int valid_image(const WgpuNtTriangle *image)
{
    return image && image->size==sizeof(*image) &&
        image->abi_version==WGPU_NT_ABI_VERSION &&
        image->width==WGPU_NT_TRI_WIDTH && image->height==WGPU_NT_TRI_HEIGHT &&
        image->pitch==WGPU_NT_TRI_WIDTH*2 && image->fence && image->flags<=1;
}

int wgpu_nt_check_draw_test(const WgpuNtTriangle *image)
{
    unsigned red=0, green=0, background=0;
    int x,y;
    if (!valid_image(image)) return 0;
    for (y=0; y<(int)image->height; ++y) {
        for (x=0; x<(int)image->width; ++x) {
            /* Right triangles at (8,16)..(48,56) and (64,64)..(112,104).
             * Use a two-pixel margin at edges to avoid the fill rule. */
            int r=(x>=10 && y>=18 && x+y<=61);
            int g=(x>=66 && y>=66 && (x-64)*5+(y-64)*6<=225);
            int outside_r=(x<7 || y<15 || x+y>65);
            int outside_g=(x<63 || y<63 || (x-64)*5+(y-64)*6>250);
            unsigned pixel=image->pixels[y*image->width+x];
            if (r) { if (pixel!=0xf800) return 0; ++red; }
            else if (g) { if (pixel!=0x07e0) return 0; ++green; }
            else if (outside_r && outside_g) {
                if (pixel!=WGPU_NT_TRI_BACKGROUND) return 0;
                ++background;
            }
        }
    }
    return red>500 && green>600 && background>13000;
}

int wgpu_nt_check_triangle(const WgpuNtTriangle *image)
{
    unsigned x, y, inside = 0, outside = 0;
    int red, green, blue;
    unsigned pixel;
    if (!valid_image(image)) return 0;
    for (y = 0; y < WGPU_NT_TRI_HEIGHT; ++y) {
        for (x = 0; x < WGPU_NT_TRI_WIDTH; ++x) {
            /* Barycentric weights at pixel centres for fixed vertices
             * (64,8) red, (120,120) green, (8,120) blue; sum is 448. */
            red = 478 - 4 * (int)y;
            green = 4 * (int)x + 2 * (int)y - 269;
            blue = -4 * (int)x + 2 * (int)y + 239;
            pixel = image->pixels[y * WGPU_NT_TRI_WIDTH + x];
            if (red >= 8 && green >= 8 && blue >= 8) {
                if (!near_channel(pixel >> 11, red, 31) ||
                    !near_channel((pixel >> 5) & 63, green, 63) ||
                    !near_channel(pixel & 31, blue, 31)) {
                    return 0;
                }
                ++inside;
            } else if (red < -8 || green < -8 || blue < -8) {
                if (pixel != 0x1082) {
                    return 0;
                }
                ++outside;
            }
        }
    }
    return inside > 5000 && outside > 8000;
}
