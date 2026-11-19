/* Exact NT maintenance-batch contract and rejection without partial writes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/nt/wgpu_nt.h"

static WgpuNtU32 aperture[WGPU_NT_APERTURE_MIN / 4];

static void unchanged(void)
{
    unsigned i;
    for (i = 0; i < sizeof(aperture) / sizeof(aperture[0]); ++i) {
        assert(aperture[i] == 0xa5a5a5a5ul);
    }
}

int main(void)
{
    unsigned i;
    WgpuNtU32 expected[sizeof(aperture) / sizeof(aperture[0])];
    memset(aperture, 0xa5, sizeof(aperture));
    assert(!wgpu_nt_build_smoke(NULL, 8192, 0x08000000, 41, 42, 0x5eed002a));
    assert(!wgpu_nt_build_smoke(aperture, 8191, 0x08000000, 41, 42, 0x5eed002a));
    unchanged();
    assert(!wgpu_nt_build_smoke(aperture, 8192, 0x08000001, 41, 42, 0x5eed002a));
    unchanged();
    assert(!wgpu_nt_build_smoke(aperture, 8192, 0xfffff000, 41, 42, 0x5eed002a));
    unchanged();
    assert(!wgpu_nt_build_smoke(aperture, 8192, 0x08000000, 41, 0, 0x5eed002a));
    unchanged();
    memcpy(expected, aperture, sizeof(expected));
    expected[0] = 41;
    expected[4] = 0;
    expected[64] = 0x1000;
    expected[65] = 24;
    expected[66] = 42;
    expected[67] = 0;
    expected[1024] = 0x10400002;
    expected[1025] = 0x08000010;
    expected[1026] = 0;
    expected[1027] = 0x5eed002a;
    expected[1028] = 0x05000000;
    expected[1029] = 0;
    assert(wgpu_nt_build_smoke(aperture, 8192, 0x08000000, 41, 42, 0x5eed002a));
    for (i = 0; i < sizeof(aperture) / sizeof(aperture[0]); ++i) {
        assert(aperture[i] == expected[i]);
    }
    puts("PASS: NT WGPU fixed batch, descriptor, bounds and untouched surrounding memory");
    return 0;
}
