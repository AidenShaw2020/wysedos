/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Guest-only Direct3D 7 HAL probe. Certify both pixels and WGPU fences. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#define DIRECTDRAW_VERSION 0x0700
#define DIRECT3D_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stddef.h>
#include "display/wgpu_display.h"

void *memset(void *memory, int value, size_t bytes)
{
    BYTE *p = memory;
    while (bytes--) *p++ = (BYTE)value;
    return memory;
}

static void say(const char *s)
{
    DWORD bytes;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, lstrlenA(s), &bytes, NULL);
}

static void hex(DWORD value)
{
    static const char digits[] = "0123456789abcdef";
    char output[11];
    unsigned i;
    for (i = 0; i < 8; ++i) output[i] = digits[(value >> (28 - 4 * i)) & 15];
    output[8] = '\r';
    output[9] = '\n';
    output[10] = 0;
    say(output);
}

static BOOL query(WgpuNtInfo *info)
{
    struct { WgpuDisplayReply header; WgpuNtInfo info; } reply;
    WgpuDisplayRequest request = {sizeof(request), WGPU_DISPLAY_ABI, WGPU_DISPLAY_QUERY, 0};
    HDC dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    int result;
    if (!dc) return FALSE;
    memset(&reply, 0, sizeof(reply));
    result = ExtEscape(dc, WGPU_DISPLAY_ESCAPE, sizeof(request), (LPCSTR)&request,
                        sizeof(reply), (LPSTR)&reply);
    DeleteDC(dc);
    if (result <= 0 || reply.header.size != sizeof(reply) ||
        reply.header.abi_version != WGPU_DISPLAY_ABI || reply.header.status ||
        reply.header.bytes != sizeof(*info) || reply.info.size != sizeof(*info) ||
        reply.info.abi_version != WGPU_NT_ABI_VERSION || reply.info.magic != WGPU_NT_MAGIC)
        return FALSE;
    *info = reply.info;
    return TRUE;
}

static BOOL read_presents(HDC dc, DWORD *count)
{
    return ExtEscape(dc, WGPU_DISPLAY_PRESENT_COUNT, 0, NULL, sizeof(*count),
                     (LPSTR)count) == 1;
}

static void hex64(const char *label, unsigned long long value)
{
    static const char digits[] = "0123456789abcdef";
    char output[19];
    for (unsigned i = 0; i < 16; ++i) output[i] = digits[(value >> (60 - 4 * i)) & 15];
    output[16] = '\r';
    output[17] = '\n';
    output[18] = 0;
    say(label);
    say(output);
}

static BOOL show_timing(void)
{
    WgpuD3dTiming timing;
    WgpuD3dTargetStats targets;
    BOOL extended, scene_counters;
    BOOL target_counters, gdi_counters;
    HDC dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    int result;
    if (!dc) return FALSE;
    result = ExtEscape(dc, WGPU_DISPLAY_D3D_TIMING, 0, NULL, sizeof(timing), (LPSTR)&timing);
    scene_counters = result == 1;
    if (!scene_counters)
        result = ExtEscape(dc, WGPU_DISPLAY_D3D_TIMING, 0, NULL,
                           offsetof(WgpuD3dTiming, scene_group_holds), (LPSTR)&timing);
    extended = result == 1;
    if (!extended)
        result = ExtEscape(dc, WGPU_DISPLAY_D3D_TIMING, 0, NULL,
                           offsetof(WgpuD3dTiming, material_groups), (LPSTR)&timing);
    gdi_counters = ExtEscape(dc, WGPU_DISPLAY_D3D_TARGETS, 0, NULL,
                             sizeof(targets), (LPSTR)&targets) == 1;
    target_counters = gdi_counters || ExtEscape(dc, WGPU_DISPLAY_D3D_TARGETS, 0, NULL,
        offsetof(WgpuD3dTargetStats, deferred_gdi_primary), (LPSTR)&targets) == 1;
    DeleteDC(dc);
    if (result != 1 || timing.frequency < 0) return FALSE;
    say("Submissions: "); hex(timing.submits);
    say("Draw callbacks: "); hex(timing.draw_callbacks);
    if (timing.frequency) hex64("Counter frequency: ", timing.frequency);
    else say("Raw TSC ticks\r\n");
    hex64("Draw ticks: ", timing.draw_ticks);
    hex64("Transport ticks: ", timing.transport_ticks);
    hex64("Target cache ticks: ", timing.target_ticks);
    hex64("Texture cache ticks: ", timing.texture_ticks);
    if (extended) {
        say("Material groups: "); hex(timing.material_groups);
        say("Chained groups: "); hex(timing.chained_groups);
        say("Material records: "); hex(timing.material_records);
        say("Merged records: "); hex(timing.merged_records);
        say("Callback-end groups: "); hex(timing.end_groups);
        say("Surface lookups: "); hex(timing.surface_lookups);
        hex64("Surface lookup ticks: ", timing.surface_lookup_ticks);
    }
    if (scene_counters) {
        say("Scene group holds: "); hex(timing.scene_group_holds);
        say("Scene begin signals: "); hex(timing.scene_begin);
        say("Scene end signals: "); hex(timing.scene_end);
        say("Ineligible target returns: "); hex(timing.visible_group_returns);
    }
    if (target_counters) {
        say("Rejected scanout overlaps: "); hex(targets.scanout);
        say("Rejected GDI primary overlaps: "); hex(targets.gdi_primary);
        say("Rejected front or overlay flags: "); hex(targets.flagged);
        say("Rejected without backbuffer flag: "); hex(targets.missing_backbuffer);
        say("Of those, hidden off-screen surfaces: "); hex(targets.hidden_offscreen);
    }
    if (gdi_counters) {
        say("Deferred hidden GDI primary returns: "); hex(targets.deferred_gdi_primary);
        say("GDI synchronization calls: "); hex(targets.gdi_syncs);
    }
    return TRUE;
}

static BOOL show_barriers(void)
{
    WgpuD3dBarriers stats;
    HDC dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    int result;
    BOOL flip_stats;
    if (!dc) return FALSE;
    result = ExtEscape(dc, WGPU_DISPLAY_D3D_BARRIERS, 0, NULL,
                       sizeof(stats), (LPSTR)&stats);
    flip_stats = result == 1;
    if (!flip_stats)
        result = ExtEscape(dc, WGPU_DISPLAY_D3D_BARRIERS, 0, NULL,
                           offsetof(WgpuD3dBarriers, flips), (LPSTR)&stats);
    DeleteDC(dc);
    if (result != 1) return FALSE;
    say("Color-only clear rectangles: "); hex(stats.color_clears);
    say("Depth-only clear rectangles: "); hex(stats.depth_clears);
    say("Combined clear rectangles: "); hex(stats.combined_clears);
    say("Target binds: "); hex(stats.target_binds);
    say("Target binds with pending work: "); hex(stats.pending_binds);
    say("Same surface pointers with pending work: "); hex(stats.pending_same_surface_binds);
    hex64("Clear ticks: ", stats.clear_ticks);
    hex64("Queue and drain round-trip ticks: ", stats.queued_transport_ticks);
    if (flip_stats) {
        say("Flip calls: "); hex(stats.flips);
        hex64("Flip ticks excluding drain: ", stats.flip_ticks);
        say("Blocking vertical blank waits: "); hex(stats.vblank_waits);
        hex64("Blocking vertical blank ticks: ", stats.vblank_ticks);
    }
    return TRUE;
}

static BOOL show_transport_timing(void)
{
    WgpuDisplayTransportTiming timing;
    HDC dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    int result;
    if (!dc) return FALSE;
    result = ExtEscape(dc, WGPU_DISPLAY_TRANSPORT_TIMING, 0, NULL,
                       sizeof(timing), (LPSTR)&timing);
    DeleteDC(dc);
    if (result != 1) return FALSE;
    say("Queue requests: "); hex(timing.queues);
    say("Drain requests: "); hex(timing.drains);
    hex64("Open device ticks: ", timing.open_ticks);
    hex64("Allocate and populate IRP ticks: ", timing.prepare_ticks);
    hex64("Dispatch and complete IRP ticks: ", timing.dispatch_ticks);
    hex64("Close device ticks: ", timing.close_ticks);
    return TRUE;
}

static BOOL measure_presents(void)
{
    DWORD before, after, started, elapsed;
    BOOL ok = FALSE;
    HDC dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    if (!dc) return FALSE;
    if (read_presents(dc, &before)) {
        started = GetTickCount();
        Sleep(20000);
        if (read_presents(dc, &after)) {
            elapsed = GetTickCount() - started;
            say("Completed flips (hex): "); hex(after - before);
            say("Elapsed milliseconds (hex): "); hex(elapsed);
            ok = elapsed && after != before;
        }
    }
    DeleteDC(dc);
    return ok;
}

static BOOL check_pixels(IDirectDrawSurface7 *surface, BOOL textured, BOOL alpha,
                         BOOL specular, DWORD culled_color)
{
    static const DWORD samples[][3] = {
        {4, 4, 0x0000ff}, {24, 24, 0xff0000}, {88, 24, 0x00ff00},
        {127, 127, 0x0000ff}, {36, 20, 0xff0000}, {20, 36, 0xff0000}
    };
    DDSURFACEDESC2 desc;
    HRESULT hr;
    DWORD i;
    BOOL ok;
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    hr = IDirectDrawSurface7_Lock(surface, NULL, &desc, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (FAILED(hr)) { say("Lock: "); hex(hr); return FALSE; }
    ok = desc.lpSurface && desc.lPitch >= 256 && desc.dwWidth >= 128 && desc.dwHeight >= 128 &&
        (desc.ddpfPixelFormat.dwRGBBitCount == 32 || desc.ddpfPixelFormat.dwRGBBitCount == 16);
    if (!ok) {
        IDirectDrawSurface7_Unlock(surface, NULL);
        return FALSE;
    }
    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        DWORD expected = textured && i == 1 ? 0xffff00 : samples[i][2];
        if (expected == culled_color) expected = 0x0000ff;
        if (textured && i == 4) expected = 0xff0000;
        if (textured && i == 5) expected = 0x00ffff;
        if (specular && (i == 1 || i >= 4)) expected |= 0x0000ff;
        if (alpha && (i == 1 || i >= 4)) expected = 0xff00ff;
        if (alpha && i == 2) expected = 0x0000ff;
        DWORD actual;
        BYTE *row = (BYTE *)desc.lpSurface + samples[i][1] * desc.lPitch;
        if (desc.ddpfPixelFormat.dwRGBBitCount == 16) {
            expected = ((expected >> 8) & 0xf800) | ((expected >> 5) & 0x07e0) |
                       ((expected >> 3) & 0x1f);
            actual = ((WORD *)row)[samples[i][0]];
        } else {
            actual = ((DWORD *)row)[samples[i][0]] & 0xffffff;
        }
        if (actual != expected) {
            say("sample: "); hex(i);
            say("expected: "); hex(expected);
            say("actual: "); hex(actual);
            ok = FALSE;
        }
    }
    if (ok) say("Exact render-target pixels\r\n");
    return SUCCEEDED(IDirectDrawSurface7_Unlock(surface, NULL)) && ok;
}

static BOOL check_depth_pixels(IDirectDrawSurface7 *surface)
{
    DDSURFACEDESC2 desc;
    BOOL ok = FALSE;
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (FAILED(IDirectDrawSurface7_Lock(surface, NULL, &desc, DDLOCK_WAIT | DDLOCK_READONLY, NULL)))
        return FALSE;
    if (desc.lpSurface && desc.lPitch >= 256 && desc.dwWidth >= 128 && desc.dwHeight >= 128) {
        WORD red = ((WORD *)((BYTE *)desc.lpSurface + 24 * desc.lPitch))[24];
        WORD green = ((WORD *)((BYTE *)desc.lpSurface + 24 * desc.lPitch))[88];
        WORD clear = ((WORD *)((BYTE *)desc.lpSurface + 4 * desc.lPitch))[4];
        say("Depth near: "); hex(red);
        say("Depth far: "); hex(green);
        say("Depth clear: "); hex(clear);
        ok = red == 0x4000 && green == 0xbfff && clear == 0xffff;
    }
    return SUCCEEDED(IDirectDrawSurface7_Unlock(surface, NULL)) && ok;
}

static BOOL show_trace(void)
{
    static const ULONG sizes[] = {
        sizeof(WgpuD3dTrace),
        offsetof(WgpuD3dTrace, init_state_count),
        offsetof(WgpuD3dTrace, unindexed_first),
        offsetof(WgpuD3dTrace, rejected_state),
        offsetof(WgpuD3dTrace, rejected_phase),
        offsetof(WgpuD3dTrace, texture_creates),
        offsetof(WgpuD3dTrace, clear_commands)
    };
    WgpuD3dTrace trace;
    HDC dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    ULONG i, bytes = 0;
    if (!dc) return FALSE;
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        if (ExtEscape(dc, WGPU_DISPLAY_D3D_TRACE, 0, NULL, sizes[i], (LPSTR)&trace) == 1) {
            bytes = sizes[i];
            break;
        }
    }
    DeleteDC(dc);
    if (!bytes) return FALSE;
    say("context creates: "); hex(trace.context_creates);
    say("create status: "); hex(trace.create_status);
    say("DP2 calls: "); hex(trace.dp2_calls);
    say("DP2 status: "); hex(trace.dp2_status);
    say("phase: "); hex(trace.phase);
    say("opcode: "); hex(trace.opcode);
    say("state: "); hex(trace.state);
    say("value: "); hex(trace.value);
    say("command caps: "); hex(trace.command_caps);
    say("command capacity: "); hex(trace.command_capacity);
    say("command offset: "); hex(trace.command_offset);
    say("command bytes: "); hex(trace.command_bytes);
    say("vertex type: "); hex(trace.vertex_type);
    say("vertex count: "); hex(trace.vertex_count);
    say("vertex stride: "); hex(trace.vertex_stride);
    say("vertex capacity: "); hex(trace.vertex_capacity);
    say("vertex caps: "); hex(trace.vertex_caps);
    say("vertex offset: "); hex(trace.vertex_offset);
    say("available vertices: "); hex(trace.available_vertices);
    say("flags: "); hex(trace.flags);
    if (bytes >= offsetof(WgpuD3dTrace, texture_creates)) {
        say("Clear commands: "); hex(trace.clear_commands);
        say("Clear rectangles: "); hex(trace.clear_rects);
        say("Clear flags: "); hex(trace.clear_flags);
        say("Clear status: "); hex(trace.clear_status);
        say("Rejected DP2 batches: "); hex(trace.rejected_batches);
        say("Last rejected status: "); hex(trace.last_rejected_status);
        say("Last rejected opcode: "); hex(trace.last_rejected_opcode);
    }
    if (bytes >= offsetof(WgpuD3dTrace, rejected_phase)) {
        say("Texture creates: "); hex(trace.texture_creates);
        say("Texture handles: "); hex(trace.texture_registered);
        say("Texture pitch: "); hex(trace.texture_pitch);
        say("Texture row allocation: "); hex(trace.texture_block);
    }
    if (bytes >= offsetof(WgpuD3dTrace, rejected_state)) {
        say("Rejected phase: "); hex(trace.rejected_phase);
        say("Rejected FVF: "); hex(trace.rejected_fvf);
        say("Rejected vertices: "); hex(trace.rejected_vertices);
        say("Rejected command bytes: "); hex(trace.rejected_cmd_bytes);
        say("Unindexed draws: "); hex(trace.unindexed_calls);
        say("First unindexed X: "); hex(trace.unindexed_x);
        say("First unindexed Y: "); hex(trace.unindexed_y);
        say("First unindexed ARGB: "); hex(trace.unindexed_argb);
    }
    if (bytes >= offsetof(WgpuD3dTrace, unindexed_first)) {
        say("Rejected state: "); hex(trace.rejected_state);
        say("Rejected value: "); hex(trace.rejected_value);
    }
    if (bytes >= offsetof(WgpuD3dTrace, init_state_count)) {
        say("Unindexed start: "); hex(trace.unindexed_first);
        say("Unindexed stride: "); hex(trace.unindexed_stride);
        say("Unindexed buffer offset: "); hex(trace.unindexed_offset);
        say("Unindexed available vertices: "); hex(trace.unindexed_vertex_count);
    }
    if (bytes == sizeof(trace)) {
        say("Texture queries: "); hex(trace.texture_queries);
        say("Texture denials: "); hex(trace.texture_denials);
        if (trace.texture_denials) {
            say("Denied texture caps: "); hex(trace.texture_query_caps);
            say("Denied texture flags: "); hex(trace.texture_query_flags);
            say("Denied texture bits: "); hex(trace.texture_query_bits);
            say("Denied texture width: "); hex(trace.texture_query_width);
            say("Denied texture height: "); hex(trace.texture_query_height);
            say("Denied texture R: "); hex(trace.texture_query_r);
            say("Denied texture G: "); hex(trace.texture_query_g);
            say("Denied texture B: "); hex(trace.texture_query_b);
            say("Denied texture A: "); hex(trace.texture_query_a);
        }
        say("First rejected render state and following pairs: "); hex(trace.init_state_count);
        for (i = 0; i < trace.init_state_count && i < WGPU_D3D_TRACE_STATES; ++i) {
            hex(trace.init_states[i][0]);
            hex(trace.init_states[i][1]);
        }
    }
    return TRUE;
}

#define CHECK(step, call) do { hr = (call); if (FAILED(hr)) { say(step ": "); hex(hr); goto done; } } while (0)

static D3DRECT clear565_regions[] = {
    {.x1 = 0, .y1 = 0, .x2 = 128, .y2 = 128},
    {.x1 = 7, .y1 = 9, .x2 = 47, .y2 = 61},
    {.x1 = 43, .y1 = 51, .x2 = 69, .y2 = 79}
};

static BOOL check_clear565_surface(IDirectDrawSurface7 *surface, const WORD values[3])
{
    DDSURFACEDESC2 desc = {.dwSize = sizeof(desc)};
    LONG x, y;
    BOOL ok;
    if (FAILED(IDirectDrawSurface7_Lock(surface, NULL, &desc,
                                      DDLOCK_WAIT | DDLOCK_READONLY, NULL))) return FALSE;
    ok = desc.lpSurface && desc.lPitch >= 256 && desc.dwWidth >= 128 && desc.dwHeight >= 128;
    for (y = 0; ok && y < 96; ++y) {
        const WORD *row = (const WORD *)((const BYTE *)desc.lpSurface + y * desc.lPitch);
        for (x = 0; x < 96; ++x) {
            unsigned region;
            for (region = 2; region; --region) {
                const D3DRECT *r = &clear565_regions[region];
                if (x >= r->x1 && x < r->x2 && y >= r->y1 && y < r->y2) break;
            }
            if (row[x] == values[region]) continue;
            say("Clear mismatch x: "); hex(x);
            say("Clear mismatch y: "); hex(y);
            say("Clear expected: "); hex(values[region]);
            say("Clear actual: "); hex(row[x]);
            ok = FALSE;
            break;
        }
    }
    return SUCCEEDED(IDirectDrawSurface7_Unlock(surface, NULL)) && ok;
}

static BOOL read_clear565_fence(HANDLE owner, WgpuNtInfo *info)
{
    DWORD bytes = 0;
    return DeviceIoControl(owner, WGPU_NT_IOCTL_QUERY, NULL, 0,
        info, sizeof(*info), &bytes, NULL) && bytes == sizeof(*info) &&
        info->size == sizeof(*info) && info->abi_version == WGPU_NT_ABI_VERSION &&
        info->magic == WGPU_NT_MAGIC;
}

static BOOL check_clear565(IDirect3DDevice7 *device, IDirectDrawSurface7 *target,
                          IDirectDrawSurface7 *depth_surface)
{
    static const DWORD colors[] = {0xff3864ad, 0xffa86d17, 0xff2ac58e};
    static const float depths[] = {1.0f, 0.25f, 0.75f};
    static const WORD rgb565[] = {0x3b35, 0xab62, 0x2e31};
    static const WORD z16[] = {0xffff, 0x4000, 0xbfff};
    static struct { float x, y, z, rhw; DWORD diffuse; } marker[] = {
        {100, 100, 0.5f, 1, 0xffff0000}, {116, 100, 0.5f, 1, 0xffff0000},
        {100, 116, 0.5f, 1, 0xffff0000}
    };
    WgpuNtInfo before, after;
    HRESULT hr;
    unsigned i;
    BOOL ok = FALSE;
    /* Read the exact owner fence without relying on a display mode transition. */
    HANDLE owner = CreateFileA("\\\\.\\WyseWgpuNT", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (owner == INVALID_HANDLE_VALUE) {
        say("Could not open WGPU for fence queries\r\n");
        return FALSE;
    }
    if (!read_clear565_fence(owner, &before)) {
        say("WGPU fence read before clear failed\r\n");
        goto done;
    }
    CHECK("Begin clear scene", IDirect3DDevice7_BeginScene(device));
    for (i = 0; i < sizeof(clear565_regions) / sizeof(clear565_regions[0]); ++i)
        CHECK("Clear RGB565 and Z16", IDirect3DDevice7_Clear(device, 1,
              &clear565_regions[i], D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, colors[i], depths[i], 0));
    /* Force the runtime to send its clear DP2 stream. The marker is
     * deliberately outside the region whose color and depth are checked. */
    CHECK("Draw marker", IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLELIST,
          D3DFVF_XYZRHW | D3DFVF_DIFFUSE, marker, 3, 0));
    CHECK("End clear scene", IDirect3DDevice7_EndScene(device));
    if (!read_clear565_fence(owner, &after)) {
        say("WGPU fence read after clear failed\r\n");
        goto done;
    }
    say("RGB565/Z16 clear and marker GPU submissions: "); hex(after.submits - before.submits);
    say("GPU resets: "); hex(after.resets - before.resets);
    if (after.submits - before.submits != 4 || after.completed == before.completed ||
        after.resets != before.resets) goto done;
    if (!check_clear565_surface(target, rgb565) || !check_clear565_surface(depth_surface, z16))
        goto done;
    say("Exact RGB565 and Z16, including overlap and rectangle edges\r\n");
    ok = TRUE;
done:
    CloseHandle(owner);
    return ok;
}

typedef struct {
    DDPIXELFORMAT argb1555;
    DWORD count;
} LegacyFormats;

static HRESULT CALLBACK legacy_texture_format(DDSURFACEDESC *description, void *context)
{
    LegacyFormats *formats = context;
    const DDPIXELFORMAT *pixel = &description->ddpfPixelFormat;
    ++formats->count;
    say("Texture flags: "); hex(pixel->dwFlags);
    say("Texture bits: "); hex(pixel->dwRGBBitCount);
    say("Alpha mask: "); hex(pixel->dwRGBAlphaBitMask);
    if (pixel->dwRGBBitCount == 16 && pixel->dwRBitMask == 0x7c00 &&
        pixel->dwGBitMask == 0x03e0 && pixel->dwBBitMask == 0x001f &&
        pixel->dwRGBAlphaBitMask == 0x8000)
        formats->argb1555 = *pixel;
    return D3DENUMRET_OK;
}

static BOOL probe_legacy_texture(void)
{
    LegacyFormats formats = {0};
    IDirectDraw *dd = NULL;
    IDirectDrawSurface *target = NULL, *texture = NULL, *source = NULL;
    IDirect3D2 *d3d = NULL;
    IDirect3DDevice2 *device = NULL;
    D3DDEVICEDESC hardware = { .dwSize = sizeof(hardware) };
    D3DDEVICEDESC software = { .dwSize = sizeof(software) };
    DDSURFACEDESC desc = { .dwSize = sizeof(desc) };
    HWND window = CreateWindowExA(0, "STATIC", "NT legacy Direct3D texture test",
                                  WS_POPUP, 0, 0, 128, 128, NULL, NULL,
                                  GetModuleHandleA(NULL), NULL);
    HRESULT hr;
    BOOL ok = FALSE;
    if (!window) return FALSE;
    CHECK("DirectDrawCreate", DirectDrawCreate(NULL, &dd, NULL));
    CHECK("Legacy SetCooperativeLevel", IDirectDraw_SetCooperativeLevel(dd, window, DDSCL_NORMAL));
    CHECK("QueryInterface IDirect3D2", IDirectDraw_QueryInterface(dd, &IID_IDirect3D2, (void **)&d3d));
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY | DDSCAPS_3DDEVICE;
    desc.dwWidth = desc.dwHeight = 128;
    CHECK("Legacy render surface", IDirectDraw_CreateSurface(dd, &desc, &target, NULL));
    CHECK("Legacy CreateDevice HAL", IDirect3D2_CreateDevice(d3d, &IID_IDirect3DHALDevice, target, &device));
    CHECK("Legacy GetCaps", IDirect3DDevice2_GetCaps(device, &hardware, &software));
    say("Device caps: "); hex(hardware.dwDevCaps);
    say("Raster caps: "); hex(hardware.dpcTriCaps.dwRasterCaps);
    say("Texture caps: "); hex(hardware.dpcTriCaps.dwTextureCaps);
    say("Maximum texture width: "); hex(hardware.dwMaxTextureWidth);
    say("Maximum texture height: "); hex(hardware.dwMaxTextureHeight);
    CHECK("Legacy EnumTextureFormats", IDirect3DDevice2_EnumTextureFormats(device, legacy_texture_format, &formats));
    say("Legacy texture formats: "); hex(formats.count);
    if (!formats.argb1555.dwSize) {
        say("Legacy ARGB1555 format missing\r\n");
        goto done;
    }
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
    desc.dwWidth = desc.dwHeight = 64;
    desc.ddpfPixelFormat = formats.argb1555;
    CHECK("Legacy ARGB1555 surface", IDirectDraw_CreateSurface(dd, &desc, &texture, NULL));
    say("Legacy plain ARGB1555 texture PASS\r\n");
    IDirectDrawSurface_Release(texture);
    texture = NULL;
    /* Some DX5 applications use this system-memory/allocation pair with
     * a single level even for a textured rectangle with no mip filtering. */
    desc.dwFlags |= DDSD_MIPMAPCOUNT;
    desc.dwMipMapCount = 1;
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY | DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;
    CHECK("Legacy single-level system texture", IDirectDraw_CreateSurface(dd, &desc, &source, NULL));
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_COMPLEX |
                        DDSCAPS_MIPMAP | DDSCAPS_ALLOCONLOAD;
    CHECK("Legacy single-level video texture", IDirectDraw_CreateSurface(dd, &desc, &texture, NULL));
    say("Legacy Direct3D 5 single-level texture pair PASS\r\n");
    ok = TRUE;
done:
    if (source) IDirectDrawSurface_Release(source);
    if (texture) IDirectDrawSurface_Release(texture);
    if (device) IDirect3DDevice2_Release(device);
    if (target) IDirectDrawSurface_Release(target);
    if (d3d) IDirect3D2_Release(d3d);
    if (dd) IDirectDraw_Release(dd);
    DestroyWindow(window);
    return ok;
}

static BOOL probe_legacy_depth(void)
{
    IDirectDraw *dd = NULL;
    IDirectDrawSurface *depth = NULL;
    DDSURFACEDESC desc = { .dwSize = sizeof(desc) };
    HWND window = CreateWindowExA(0, "STATIC", "NT legacy Direct3D depth test",
                                  WS_POPUP | WS_VISIBLE, 0, 0, 640, 480,
                                  NULL, NULL, GetModuleHandleA(NULL), NULL);
    HRESULT hr, video;
    BOOL ok = FALSE;
    if (!window) return FALSE;
    CHECK("Depth DirectDrawCreate", DirectDrawCreate(NULL, &dd, NULL));
    CHECK("Depth exclusive mode", IDirectDraw_SetCooperativeLevel(dd, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN));
    CHECK("Depth 640x480x16 mode", IDirectDraw_SetDisplayMode(dd, 640, 480, 16));
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_ZBUFFERBITDEPTH;
    desc.dwWidth = 640;
    desc.dwHeight = 480;
    desc.dwZBufferBitDepth = 16;
    desc.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_SYSTEMMEMORY;
    hr = IDirectDraw_CreateSurface(dd, &desc, &depth, NULL);
    say("Legacy system Z16 result: "); hex(hr);
    if (depth) { IDirectDrawSurface_Release(depth); depth = NULL; }
    desc.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    video = IDirectDraw_CreateSurface(dd, &desc, &depth, NULL);
    say("Legacy video Z16 result: "); hex(video);
    if (depth) { IDirectDrawSurface_Release(depth); depth = NULL; }
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
    desc.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
    desc.ddpfPixelFormat.dwZBufferBitDepth = 16;
    desc.ddpfPixelFormat.dwZBitMask = 0xffff;
    hr = IDirectDraw_CreateSurface(dd, &desc, &depth, NULL);
    say("Explicit video Z16 result: "); hex(hr);
    ok = SUCCEEDED(video) && SUCCEEDED(hr);
done:
    if (depth) IDirectDrawSurface_Release(depth);
    if (dd) {
        IDirectDraw_RestoreDisplayMode(dd);
        IDirectDraw_Release(dd);
    }
    DestroyWindow(window);
    return ok;
}


void WINAPI mainCRTStartup(void)
{
    static struct { float x, y, z, rhw; DWORD color; } vertices[] = {
        {16, 16, 0.5f, 1, 0xffff0000}, {48, 16, 0.5f, 1, 0xffff0000},
        {16, 48, 0.5f, 1, 0xffff0000}, {80, 16, 0.5f, 1, 0xff00ff00},
        {112, 16, 0.5f, 1, 0xff00ff00}, {80, 48, 0.5f, 1, 0xff00ff00},
        {16, 16, 0.75f, 1, 0xff00ff00}, {48, 16, 0.75f, 1, 0xff00ff00},
        {16, 48, 0.75f, 1, 0xff00ff00}
    };
    static const WORD indices[] = {3, 4, 5};
    static const WORD cull_indices[] = {0, 1, 2, 3, 5, 4};
    static const WORD depth_indices[] = {3, 4, 5, 6, 7, 8};
    static struct { float x, y, z, rhw; DWORD color, specular; float u, v; } textured_vertices[] = {
        {16, 16, 0.5f, 1, 0xffffffff, 0, 0, 0},
        {48, 16, 0.5f, 1, 0xffffffff, 0, 1, 0},
        {16, 48, 0.5f, 1, 0xffffffff, 0, 0, 1}
    };
    IDirectDraw7 *dd = NULL;
    IDirectDrawSurface7 *target = NULL;
    IDirectDrawSurface7 *texture = NULL;
    IDirectDrawSurface7 *depth_surface = NULL;
    IDirect3D7 *d3d = NULL;
    IDirect3DDevice7 *device = NULL;
    DDSURFACEDESC2 desc;
    D3DDEVICEDESC7 caps;
    D3DVIEWPORT7 viewport = {0, 0, 128, 128, 0, 1};
    D3DRECT clear_rect = {.x1 = 0, .y1 = 0, .x2 = 128, .y2 = 128};
    WgpuNtInfo before, after;
    HRESULT hr;
    DWORD status = 1;
    BOOL textured, alpha, depth, specular, cull, clear565, pixels_ok;
    char *args = GetCommandLineA();
    char argument[24];
    size_t length, i;
    HWND window;
    if (*args == '"') {
        ++args;
        while (*args && *args != '"') ++args;
        if (*args) ++args;
    } else {
        while (*args && *args != ' ' && *args != '\t') ++args;
    }
    while (*args == ' ' || *args == '\t') ++args;
    length = lstrlenA(args);
    while (length && (args[length - 1] == ' ' || args[length - 1] == '\t')) --length;
    if (length >= sizeof(argument)) ExitProcess(2);
    for (i = 0; i < length; ++i) argument[i] = args[i];
    argument[length] = 0;
    args = argument;
    specular = !lstrcmpA(args, "--specular");
    textured = specular || !lstrcmpA(args, "--texture");
    alpha = !lstrcmpA(args, "--alpha");
    clear565 = !lstrcmpA(args, "--clear565");
    depth = clear565 || !lstrcmpA(args, "--depth");
    cull = !lstrcmpA(args, "--cull");
    if (*args && !textured && !alpha && !depth && !cull) {
        if (!lstrcmpA(args, "--legacy")) ExitProcess(probe_legacy_texture() ? 0 : 1);
        if (!lstrcmpA(args, "--legacy-depth")) ExitProcess(probe_legacy_depth() ? 0 : 1);
        if (!lstrcmpA(args, "--trace")) ExitProcess(show_trace() ? 0 : 1);
        if (!lstrcmpA(args, "--timing")) ExitProcess(show_timing() ? 0 : 1);
        if (!lstrcmpA(args, "--transport")) ExitProcess(show_transport_timing() ? 0 : 1);
        if (!lstrcmpA(args, "--barriers")) ExitProcess(show_barriers() ? 0 : 1);
        if (!lstrcmpA(args, "--fps")) {
            BOOL ok = measure_presents();
            if (!ok) say("No completed flip measurement\r\n");
            ExitProcess(ok ? 0 : 1);
        }
        say("Usage: NTD3D [--fps | --texture | --alpha | --depth | --clear565 | --specular | --legacy | --legacy-depth | --cull | --trace | --timing | --transport | --barriers]\r\n");
        ExitProcess(2);
    }
    window = CreateWindowExA(0, "STATIC", "NT Direct3D GPU test", WS_POPUP,
                              0, 0, 128, 128, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!window) { say("Test window failed\r\n"); goto done; }
    CHECK("DirectDrawCreateEx", DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL));
    CHECK("SetCooperativeLevel", IDirectDraw7_SetCooperativeLevel(dd, window, DDSCL_NORMAL));
    CHECK("QueryInterface IDirect3D7", IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d));
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY | DDSCAPS_3DDEVICE;
    desc.dwWidth = desc.dwHeight = 128;
    CHECK("CreateSurface", IDirectDraw7_CreateSurface(dd, &desc, &target, NULL));
    if (clear565) {
        CHECK("Get clear target format", IDirectDrawSurface7_GetSurfaceDesc(target, &desc));
        if (desc.ddpfPixelFormat.dwRGBBitCount != 16 ||
            desc.ddpfPixelFormat.dwRBitMask != 0xf800 || desc.ddpfPixelFormat.dwGBitMask != 0x07e0 ||
            desc.ddpfPixelFormat.dwBBitMask != 0x001f) {
            say("The clear test requires a 16-bit RGB565 desktop\r\n");
            goto done;
        }
    }
    if (depth) {
        desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        desc.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
        desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
        desc.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
        desc.ddpfPixelFormat.dwZBufferBitDepth = 16;
        desc.ddpfPixelFormat.dwZBitMask = 0xffff;
        CHECK("Create Z16 surface", IDirectDraw7_CreateSurface(dd, &desc, &depth_surface, NULL));
        CHECK("Attach Z16 surface", IDirectDrawSurface7_AddAttachedSurface(target, depth_surface));
    }
    CHECK("CreateDevice HAL", IDirect3D7_CreateDevice(d3d, &IID_IDirect3DHALDevice, target, &device));
    CHECK("GetCaps", IDirect3DDevice7_GetCaps(device, &caps));
    if (!(caps.dwDevCaps & D3DDEVCAPS_HWRASTERIZATION)) {
        say("Hardware rasterization missing\r\n");
        goto done;
    }
    CHECK("SetViewport", IDirect3DDevice7_SetViewport(device, &viewport));
    if (clear565) {
        CHECK("Set clear test cull mode", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE));
        CHECK("Set clear test depth enable", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, D3DZB_TRUE));
        CHECK("Set clear test depth writes", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZWRITEENABLE, TRUE));
        CHECK("Set clear test depth comparison", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZFUNC, D3DCMP_ALWAYS));
        status = check_clear565(device, target, depth_surface) ? 0 : 1;
        goto done;
    }
    if (cull) {
        static const DWORD modes[] = {D3DCULL_NONE, D3DCULL_CW, D3DCULL_CCW};
        static const DWORD culled[] = {0, 0xff0000, 0x00ff00};
        DWORD cull_caps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
        if ((caps.dpcTriCaps.dwMiscCaps & cull_caps) != cull_caps || !query(&before)) goto done;
        for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
            CHECK("SetRenderState CULLMODE", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_CULLMODE, modes[i]));
            CHECK("Begin cull scene", IDirect3DDevice7_BeginScene(device));
            CHECK("Clear cull target", IDirect3DDevice7_Clear(device, 1, &clear_rect, D3DCLEAR_TARGET, 0xff0000ff, 1, 0));
            CHECK("Draw opposite windings", IDirect3DDevice7_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST,
                  D3DFVF_XYZRHW | D3DFVF_DIFFUSE, vertices, 6, (WORD *)cull_indices, 6, 0));
            CHECK("End cull scene", IDirect3DDevice7_EndScene(device));
            if (!check_pixels(target, FALSE, FALSE, FALSE, culled[i])) goto done;
        }
        if (!query(&after) || after.completed == before.completed ||
            after.submits - before.submits < 6 || after.resets != before.resets) goto done;
        say("CULLNONE, CULLCW and CULLCCW pixels and GPU fences PASS\r\n");
        status = 0;
        goto done;
    }
    CHECK("SetRenderState CULLNONE", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE));
    if (depth) {
        for (i = 0; i < 6; ++i) vertices[i].z = i < 3 ? 0.25f : 0.75f;
        CHECK("Enable depth", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, D3DZB_TRUE));
        CHECK("Enable depth writes", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZWRITEENABLE, TRUE));
        CHECK("Depth compare LESS", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZFUNC, D3DCMP_LESS));
    }
    if (textured) {
        DWORD x, y;
        DWORD total = 0, available = 0;
        DDSCAPS2 memory_caps = {.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY};
        hr = IDirectDraw7_GetAvailableVidMem(dd, &memory_caps, &total, &available);
        say("Texture heap status: "); hex(hr);
        say("Texture heap total: "); hex(total);
        say("Texture heap available: "); hex(available);
        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
        desc.dwWidth = desc.dwHeight = 32;
        desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
        desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
        desc.ddpfPixelFormat.dwRGBBitCount = 16;
        desc.ddpfPixelFormat.dwRBitMask = 0xf800;
        desc.ddpfPixelFormat.dwGBitMask = 0x07e0;
        desc.ddpfPixelFormat.dwBBitMask = 0x001f;
        CHECK("Create RGB565 texture", IDirectDraw7_CreateSurface(dd, &desc, &texture, NULL));
        CHECK("Lock texture", IDirectDrawSurface7_Lock(texture, NULL, &desc, DDLOCK_WAIT, NULL));
        if (!desc.lpSurface || desc.lPitch < 64) {
            IDirectDrawSurface7_Unlock(texture, NULL);
            goto done;
        }
        for (y = 0; y < 32; ++y) {
            WORD *row = (WORD *)((BYTE *)desc.lpSurface + y * desc.lPitch);
            for (x = 0; x < 32; ++x)
                row[x] = y >= 16 ? 0x07ff : x >= 16 ? 0xf800 : 0xffe0;
        }
        CHECK("Unlock texture", IDirectDrawSurface7_Unlock(texture, NULL));
    }
    if (!query(&before)) { say("WGPU query before draw failed\r\n"); goto done; }
    CHECK("BeginScene", IDirect3DDevice7_BeginScene(device));
    CHECK("Clear", IDirect3DDevice7_Clear(device, 1, &clear_rect,
          D3DCLEAR_TARGET | (depth ? D3DCLEAR_ZBUFFER : 0), 0xff0000ff, 1, 0));
    if (alpha) {
        vertices[3].color = vertices[4].color = vertices[5].color = 0x0000ff00;
        CHECK("Source blend ONE", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE));
        CHECK("Destination blend ONE", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_DESTBLEND, D3DBLEND_ONE));
        CHECK("Enable blend", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ALPHABLENDENABLE, TRUE));
        CHECK("Alpha compare GREATER", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ALPHAFUNC, D3DCMP_GREATER));
        CHECK("Alpha reference", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ALPHAREF, 127));
        CHECK("Enable alpha test", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ALPHATESTENABLE, TRUE));
    }
    if (textured) {
        CHECK("Bind texture", IDirect3DDevice7_SetTexture(device, 0, texture));
        if (specular) {
            for (i = 0; i < 3; ++i) textured_vertices[i].specular = 0xff0000ff;
            CHECK("Enable specular", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_SPECULARENABLE, TRUE));
        }
        CHECK("Draw textured triangle", IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLELIST,
              D3DFVF_TLVERTEX, (void *)textured_vertices, 3, 0));
        CHECK("Unbind texture", IDirect3DDevice7_SetTexture(device, 0, NULL));
        if (specular)
            CHECK("Disable specular", IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_SPECULARENABLE, FALSE));
    } else {
        CHECK("DrawPrimitive", IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLELIST,
              D3DFVF_XYZRHW | D3DFVF_DIFFUSE, (void *)vertices, 3, 0));
    }
    CHECK("EndScene first draw", IDirect3DDevice7_EndScene(device));
    if (query(&after)) {
        say("Submissions after clear and first draw: "); hex(after.submits - before.submits);
    }
    CHECK("BeginScene indexed draw", IDirect3DDevice7_BeginScene(device));
    CHECK("DrawIndexedPrimitive", IDirect3DDevice7_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE, (void *)vertices, depth ? 9 : 6,
            (WORD *)(depth ? depth_indices : indices), depth ? 6 : 3, 0));
    CHECK("EndScene", IDirect3DDevice7_EndScene(device));
    pixels_ok = check_pixels(target, textured, alpha, specular, 0);
    if (depth && !check_depth_pixels(depth_surface)) pixels_ok = FALSE;
    if (!query(&after)) { say("WGPU query after draw failed\r\n"); goto done; }
    say("GPU submissions: "); hex(after.submits - before.submits);
    say("Prior GPU fence: "); hex(before.completed);
    say("Completed GPU fence: "); hex(after.completed);
    say("GPU resets: "); hex(after.resets - before.resets);
    if (after.completed == before.completed || after.submits - before.submits < (depth ? 4u : 3u) ||
        after.resets != before.resets) {
        say("Expected completed hardware clear, draw, and indexed draw without reset\r\n");
        goto done;
    }
    if (!pixels_ok) goto done;
    status = 0;
done:
    if (status) show_trace();
    if (device) IDirect3DDevice7_Release(device);
    if (depth_surface) IDirectDrawSurface7_Release(depth_surface);
    if (texture) IDirectDrawSurface7_Release(texture);
    if (target) IDirectDrawSurface7_Release(target);
    if (d3d) IDirect3D7_Release(d3d);
    if (dd) IDirectDraw7_Release(dd);
    if (window) DestroyWindow(window);
    say(status ? "Direct3D 7 HAL FAIL\r\n" : "Direct3D 7 HAL PASS\r\n");
    ExitProcess(status);
}
