/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WGPU_DISPLAY_H
#define WGPU_DISPLAY_H
#include "../wgpu_nt.h"
#include "../wgpu_fill.h"
#include "../wgpu_draw.h"
#ifdef WGPU_NT_ENABLE_MATERIAL
#include "../wgpu_material.h"
#endif

#define WGPU_DISPLAY_ABI 1ul
#define WGPU_DISPLAY_ESCAPE 0x57475001ul
#define WGPU_DISPLAY_COPY_TRACE 0x57475002ul
#define WGPU_DISPLAY_D3D_TRACE 0x57475003ul
#define WGPU_DISPLAY_PRESENT_COUNT 0x57475004ul
#define WGPU_DISPLAY_D3D_TIMING 0x57475005ul
#define WGPU_DISPLAY_D3D_TARGETS 0x57475006ul
#define WGPU_DISPLAY_TRANSPORT_TIMING 0x57475007ul
#define WGPU_DISPLAY_D3D_BARRIERS 0x57475008ul
typedef struct WgpuD3dBarriers {
    WgpuNtU32 color_clears, depth_clears, combined_clears;
    WgpuNtU32 target_binds, pending_binds, pending_same_surface_binds;
    unsigned long long clear_ticks, queued_transport_ticks;
    WgpuNtU32 flips, vblank_waits;
    unsigned long long flip_ticks, vblank_ticks;
} WgpuD3dBarriers;
typedef struct WgpuDisplayTransportTiming {
    WgpuNtU32 queues, drains;
    unsigned long long open_ticks, prepare_ticks, dispatch_ticks, close_ticks;
} WgpuDisplayTransportTiming;
typedef struct WgpuD3dTargetStats {
    WgpuNtU32 scanout, gdi_primary, flagged, missing_backbuffer;
    WgpuNtU32 hidden_offscreen;
    WgpuNtU32 deferred_gdi_primary, gdi_syncs;
} WgpuD3dTargetStats;
typedef struct WgpuD3dTiming {
    WgpuNtU32 submits, draw_callbacks;
    long long frequency, draw_ticks, transport_ticks, target_ticks, texture_ticks;
    WgpuNtU32 material_groups, chained_groups, material_records, merged_records;
    WgpuNtU32 end_groups, surface_lookups;
    long long surface_lookup_ticks;
    WgpuNtU32 scene_group_holds, scene_begin, scene_end, visible_group_returns;
} WgpuD3dTiming;
typedef struct WgpuCopyTrace {
    WgpuNtU32 stage, flags, rop, clipped;
    WgpuNtU32 src_offset, src_pitch, dst_offset, dst_pitch;
} WgpuCopyTrace;
#define WGPU_D3D_TRACE_STATES 128
typedef struct WgpuD3dTrace {
    WgpuNtU32 context_creates, create_status, dp2_calls, dp2_status;
    WgpuNtU32 phase, opcode, state, value;
    WgpuNtU32 command_caps, command_capacity, command_offset, command_bytes;
    WgpuNtU32 vertex_type, vertex_count, vertex_stride, flags;
    WgpuNtU32 vertex_caps, vertex_capacity, vertex_offset, available_vertices;
    WgpuNtU32 clear_commands, clear_rects, clear_flags, clear_status;
    WgpuNtU32 rejected_batches, last_rejected_status, last_rejected_opcode;
    WgpuNtU32 texture_creates, texture_registered, texture_pitch, texture_block;
    WgpuNtU32 rejected_phase, rejected_fvf, rejected_vertices, rejected_cmd_bytes;
    WgpuNtU32 unindexed_calls, unindexed_x, unindexed_y, unindexed_argb;
    WgpuNtU32 rejected_state, rejected_value;
    WgpuNtU32 unindexed_first, unindexed_stride, unindexed_offset, unindexed_vertex_count;
    WgpuNtU32 init_state_count;
    WgpuNtU32 init_states[WGPU_D3D_TRACE_STATES][2];
    WgpuNtU32 texture_queries, texture_denials, texture_query_caps;
    WgpuNtU32 texture_query_flags, texture_query_bits, texture_query_width, texture_query_height;
    WgpuNtU32 texture_query_r, texture_query_g, texture_query_b, texture_query_a;
} WgpuD3dTrace;
/* CTL_CODE(FILE_DEVICE_VIDEO, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS). */
#define WGPU_DISPLAY_IOCTL 0x00232400ul
/* Separate miniport-only colour fill, not exposed as a raw address escape. */
#define WGPU_DISPLAY_FILL_IOCTL 0x00232404ul
#define WGPU_DISPLAY_COPY_IOCTL 0x00232408ul
#define WGPU_DISPLAY_DRAW_IOCTL 0x0023240cul
#define WGPU_DISPLAY_MATERIAL_IOCTL 0x00232410ul
#define WGPU_DISPLAY_MATERIAL_GROUP_IOCTL 0x00232414ul
#define WGPU_DISPLAY_QUEUE_MATERIAL_IOCTL 0x00232418ul
#define WGPU_DISPLAY_DRAIN_MATERIAL_IOCTL 0x0023241cul
#define WGPU_DISPLAY_TRANSPORT_TIMING_IOCTL 0x00232420ul
#define WGPU_DISPLAY_QUERY 1ul
#define WGPU_DISPLAY_SELFTEST 2ul
#define WGPU_DISPLAY_TRIANGLE 3ul
#define WGPU_DISPLAY_DRAW_TEST 4ul
typedef struct WgpuDisplayRequest {
    WgpuNtU32 size, abi_version, operation, reserved;
} WgpuDisplayRequest;
typedef struct WgpuDisplayReply {
    WgpuNtU32 size, abi_version, status, bytes;
    /* WgpuNtInfo or WgpuNtTriangle follows. No process/kernel pointers. */
} WgpuDisplayReply;

static __inline WgpuNtU32 wgpu_display_payload(WgpuNtU32 operation)
{
    if (operation == WGPU_DISPLAY_QUERY || operation == WGPU_DISPLAY_SELFTEST)
        return sizeof(WgpuNtInfo);
    if (operation == WGPU_DISPLAY_TRIANGLE || operation == WGPU_DISPLAY_DRAW_TEST)
        return sizeof(WgpuNtTriangle);
    return 0;
}
static __inline int wgpu_display_request_valid(const WgpuDisplayRequest *request,
    WgpuNtU32 input_bytes, WgpuNtU32 output_bytes)
{
    WgpuNtU32 payload;
    if (!request || input_bytes != sizeof(*request) ||
        request->size != sizeof(*request) || request->abi_version != WGPU_DISPLAY_ABI ||
        request->reserved) return 0;
    payload = wgpu_display_payload(request->operation);
    return payload && output_bytes == sizeof(WgpuDisplayReply) + payload;
}
#endif
