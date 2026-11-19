/* Host tests: real fixed draw builder versus the existing DOS reference,
 * bounds/poison checks, and independent pixel-validation negative cases. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/nt/wgpu_nt.h"

extern const uint32_t *nt_triangle_reference(uint32_t gfx, uint32_t *bytes);
static WgpuNtU32 aperture[(WGPU_NT_TRI_MIN + 4096) / 4];
static WgpuNtTriangle image;

static void poison(void) { memset(aperture, 0xa5, sizeof(aperture)); }
static void unchanged(void)
{
    unsigned i;
    for (i = 0; i < sizeof(aperture) / sizeof(aperture[0]); ++i)
        assert(aperture[i] == 0xa5a5a5a5ul);
}

static void reference_pixels(void)
{
    unsigned x, y;
    int r, g, b;
    image.size = sizeof(image); image.abi_version = WGPU_NT_ABI_VERSION;
    image.width = image.height = 128; image.pitch = 256;
    image.fence = 7; image.flags = 1;
    for (y = 0; y < 128; ++y) for (x = 0; x < 128; ++x) {
        r = 478 - 4 * (int)y; g = 4 * (int)x + 2 * (int)y - 269;
        b = -4 * (int)x + 2 * (int)y + 239;
        image.pixels[y * 128 + x] = r >= 0 && g >= 0 && b >= 0 ?
            (unsigned short)(((r * 31 + 224) / 448) << 11 |
                             ((g * 63 + 224) / 448) << 5 | (b * 31 + 224) / 448) : 0x1082;
    }
}

int main(void)
{
    const uint32_t *reference;
    uint32_t bytes;
    unsigned i, allowed;
    static const uint32_t kernel[20] = {
        0x0080005a,0x2e203ae8,0x3a0000c0,0x008d0040,
        0x0080005a,0x2e603ae8,0x3a0000d0,0x008d0040,
        0x0080005a,0x2ea03ae8,0x3a0000e0,0x008d0040,
        0x0080005a,0x2ee03ae8,0x3a0000f0,0x008d0040,
        0x05800031,0x20003a40,0x0e8d0e20,0x90031000
    };
    poison();
    assert(!wgpu_nt_build_triangle(NULL, WGPU_NT_TRI_MIN, 0x5001a000, 6, 7, 1));
    assert(!wgpu_nt_build_triangle(aperture, WGPU_NT_TRI_MIN - 1, 0x5001a000, 6, 7, 1)); unchanged();
    assert(!wgpu_nt_build_triangle(aperture, WGPU_NT_TRI_MIN, 0x5001a001, 6, 7, 1)); unchanged();
    assert(!wgpu_nt_build_triangle(aperture, WGPU_NT_TRI_MIN, 0xfffff000, 6, 7, 1)); unchanged();
    assert(!wgpu_nt_build_triangle(aperture, WGPU_NT_TRI_MIN, 0x5001a000, 6, 0, 1)); unchanged();
    assert(!wgpu_nt_build_triangle(aperture, WGPU_NT_TRI_MIN, 0x5001a000, 6, 7, 2)); unchanged();
    reference = nt_triangle_reference(0x5001a000, &bytes);
    assert(wgpu_nt_build_triangle(aperture, WGPU_NT_TRI_MIN, 0x5001a000, 6, 7, 1) == bytes);
    assert(memcmp(aperture + WGPU_NT_TRI_BATCH / 4, reference, WGPU_NT_TRI_BATCH_BYTES) == 0);
    assert(memcmp(aperture + WGPU_NT_TRI_KERNEL / 4, kernel, sizeof(kernel)) == 0);
    assert(aperture[0] == 6 && aperture[64] == WGPU_NT_TRI_BATCH);
    assert(aperture[65] == bytes && aperture[66] == 7 && aperture[67] == 1);
    for (i = 0; i < sizeof(aperture) / sizeof(aperture[0]); ++i) {
        allowed = i == 0 || (i >= 64 && i < 68) ||
            (i >= WGPU_NT_TRI_KERNEL / 4 && i < WGPU_NT_TRI_KERNEL / 4 + 20) ||
            (i >= WGPU_NT_TRI_BATCH / 4 && i < WGPU_NT_TRI_MIN / 4);
        if (!allowed) assert(aperture[i] == 0xa5a5a5a5ul);
        if (i >= WGPU_NT_TRI_TARGET / 4 && i < WGPU_NT_TRI_MIN / 4)
            assert(aperture[i] == 0x10821082ul);
    }
    assert(wgpu_nt_build_triangle(aperture, WGPU_NT_TRI_MIN, 0x5001a000, 0xffffffff, 1, 0));
    assert(aperture[0] == 0xffffffff && aperture[66] == 1 && aperture[67] == 0);
    reference_pixels(); assert(wgpu_nt_check_triangle(&image));
    image.pixels[0] ^= 1; assert(!wgpu_nt_check_triangle(&image));
    reference_pixels(); image.width = 640; assert(!wgpu_nt_check_triangle(&image));
    reference_pixels(); image.fence = 0; assert(!wgpu_nt_check_triangle(&image));
    reference_pixels(); memset(image.pixels, 0, sizeof(image.pixels)); assert(!wgpu_nt_check_triangle(&image));
    reference_pixels();
    for (i = 0; i < 128 * 128; ++i) image.pixels[i] = 0x1082;
    assert(!wgpu_nt_check_triangle(&image));
    reference_pixels();
    for (i = 0; i < 128 * 128; ++i) {
        unsigned p = image.pixels[i];
        if (p != 0x1082) image.pixels[i] = (unsigned short)((p & 31) << 11 | (p & 0x7e0) | (p >> 11));
    }
    assert(!wgpu_nt_check_triangle(&image));
    puts("PASS: exact DOS Gen8 reference, shader, reserved ranges, invalid input and pixel negatives");
    return 0;
}
