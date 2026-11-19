/* SPDX-License-Identifier: GPL-3.0-or-later
 * XDDM framebuffer baseline: keep Direct3D capability off until the WGPU
 * rendering backend is connected to the NT display DDI. No software HAL.
 */
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#include "d3dnt.h"

void (*d3dnt_log)(const char *, ULONG, ULONG);

int d3dnt_init(void) { return 0; }
void d3dnt_mode(void *fb, ULONG vram, ULONG bpp, ULONG r, ULONG g, ULONG b, int on)
{
    (void)fb; (void)vram; (void)bpp; (void)r; (void)g; (void)b; (void)on;
}
void d3dnt_halinfo(DD_HALINFO *info) { (void)info; }
int d3dnt_info(PDD_GETDRIVERINFODATA data) { (void)data; return 0; }
int d3dnt_can_create(DDSURFACEDESC *description) { (void)description; return 0; }
void d3dnt_destroy(PDD_SURFACE_LOCAL surface) { (void)surface; }
int d3dnt_dxt(PDD_SURFACE_GLOBAL surface, DWORD fourcc, LONG stride)
{
    (void)surface; (void)fourcc; (void)stride; return 0;
}
