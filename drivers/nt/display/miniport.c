/* SPDX-License-Identifier: GPL-3.0-or-later
 * Include the pinned upstream miniport without changing the vendor tree.
 * Interpose only registration; all standard video requests retain StartIO.
 */
#include <ntdef.h>
#include <ddk/miniport.h>
#include <ddk/video.h>
#include "wgpu_display.h"
static ULONG NTAPI wgpu_video_initialize(PVOID, PVOID,
    PVIDEO_HW_INITIALIZATION_DATA, PVOID);
#define VideoPortInitialize wgpu_video_initialize
#ifdef WGPU_NT_PROBE_LFB_TYPE
static VP_STATUS NTAPI wgpu_log_video_mapping(PVOID, PHYSICAL_ADDRESS, PULONG, PULONG, PVOID *);
#define VideoPortMapMemory wgpu_log_video_mapping
#endif
#include "winvbe.c"
#undef VideoPortInitialize
#ifdef WGPU_NT_PROBE_LFB_TYPE
#undef VideoPortMapMemory
/* Private on-device instrumentation only. It emits no user-mode replies
 * or memory contents. The monitor already supports read-only page walks. */
static VP_STATUS NTAPI wgpu_log_video_mapping(PVOID hw, PHYSICAL_ADDRESS address,
    PULONG length, PULONG space, PVOID *virtual_address)
{
    const ULONG requested = *space;
    VP_STATUS status = VideoPortMapMemory(hw, address, length, space, virtual_address);
    if (status == NO_ERROR && address.QuadPart >= 0x10000000) {
        ULONG cr0, cr3, cr4, a, b, c, d, pat_lo = 0, pat_hi = 0;
        char line[180];
        __asm__ __volatile__("mov %%cr0, %0; mov %%cr3, %1; mov %%cr4, %2"
            : "=r"(cr0), "=r"(cr3), "=r"(cr4));
        __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
        if (d & (1u << 16))
            __asm__ __volatile__("rdmsr" : "=a"(pat_lo), "=d"(pat_hi) : "c"(0x277));
        sfmt(line, sizeof(line),
            "WGPU map pa=%08x va=%08x len=%08x flags=%x/%x cr=%08x/%08x/%08x pat=%08x%08x\n",
            address.LowPart, (ULONG)*virtual_address, *length, requested, *space,
            cr0, cr3, cr4, pat_hi, pat_lo);
        /* The Wyse monitor forwards SeaBIOS's diagnostic port to its log. */
        for (const char *p = line; *p; ++p) outb(0x402, (UCHAR)*p);
    }
    return status;
}
#endif

NTSTATUS NTAPI wgpu_display_send(const WgpuDisplayRequest *, ULONG, PVOID, ULONG);
NTSTATUS NTAPI wgpu_display_fill_send(const WgpuNtFill *, ULONG, PVOID, ULONG);
NTSTATUS NTAPI wgpu_display_copy_send(const WgpuNtCopy *, ULONG, PVOID, ULONG);
NTSTATUS NTAPI wgpu_display_draw_send(const WgpuNtDraw *, ULONG, PVOID, ULONG);
#ifdef WGPU_NT_ENABLE_MATERIAL
NTSTATUS NTAPI wgpu_display_material_send(const WgpuNtMaterialDraw *, ULONG, PVOID, ULONG);
#endif
#ifdef WGPU_NT_ENABLE_GROUPS
NTSTATUS NTAPI wgpu_display_material_group_send(const WgpuNtMaterialGroup *, ULONG, PVOID, ULONG);
#endif
#ifdef WGPU_NT_PIPELINE
NTSTATUS NTAPI wgpu_display_pipeline_send(ULONG, const void *, ULONG, PVOID, ULONG);
#endif
#ifdef WGPU_NT_PROFILE_TRANSPORT
void NTAPI wgpu_display_transport_init(void);
NTSTATUS NTAPI wgpu_display_transport_read(PVOID, ULONG);
#endif
static BOOLEAN NTAPI wgpu_start_io(PVOID hw, PVIDEO_REQUEST_PACKET request)
{
#ifdef WGPU_NT_PROFILE_TRANSPORT
    if (request->IoControlCode == WGPU_DISPLAY_TRANSPORT_TIMING_IOCTL) {
        BOOLEAN ok = !request->InputBufferLength &&
            wgpu_display_transport_read(request->OutputBuffer, request->OutputBufferLength) == 0;
        request->StatusBlock->Status = ok ? NO_ERROR : ERROR_INVALID_PARAMETER;
        request->StatusBlock->Information = ok ? request->OutputBufferLength : 0;
        return TRUE;
    }
#endif
    if (request->IoControlCode == WGPU_DISPLAY_IOCTL || request->IoControlCode == WGPU_DISPLAY_FILL_IOCTL ||
        request->IoControlCode == WGPU_DISPLAY_COPY_IOCTL || request->IoControlCode == WGPU_DISPLAY_DRAW_IOCTL
#ifdef WGPU_NT_ENABLE_MATERIAL
        || request->IoControlCode == WGPU_DISPLAY_MATERIAL_IOCTL
#endif
#ifdef WGPU_NT_ENABLE_GROUPS
        || request->IoControlCode == WGPU_DISPLAY_MATERIAL_GROUP_IOCTL
#endif
#ifdef WGPU_NT_PIPELINE
        || request->IoControlCode == WGPU_DISPLAY_QUEUE_MATERIAL_IOCTL
        || request->IoControlCode == WGPU_DISPLAY_DRAIN_MATERIAL_IOCTL
#endif
        ) {
        NTSTATUS status;
        PVOID in=request->InputBuffer, out=request->OutputBuffer;
        ULONG in_bytes=request->InputBufferLength, out_bytes=request->OutputBufferLength;
#ifdef WGPU_NT_PIPELINE
        if (request->IoControlCode == WGPU_DISPLAY_QUEUE_MATERIAL_IOCTL ||
            request->IoControlCode == WGPU_DISPLAY_DRAIN_MATERIAL_IOCTL)
            status=wgpu_display_pipeline_send(request->IoControlCode, in, in_bytes, out, out_bytes);
        else
#endif
#ifdef WGPU_NT_ENABLE_GROUPS
        if (request->IoControlCode == WGPU_DISPLAY_MATERIAL_GROUP_IOCTL)
            status=wgpu_display_material_group_send(in,in_bytes,out,out_bytes);
        else
#endif
#ifdef WGPU_NT_ENABLE_MATERIAL
        if (request->IoControlCode == WGPU_DISPLAY_MATERIAL_IOCTL)
            status=wgpu_display_material_send(in,in_bytes,out,out_bytes);
        else
#endif
        if (request->IoControlCode == WGPU_DISPLAY_DRAW_IOCTL)
            status=wgpu_display_draw_send(in,in_bytes,out,out_bytes);
        else if (request->IoControlCode == WGPU_DISPLAY_COPY_IOCTL)
            status=wgpu_display_copy_send(in,in_bytes,out,out_bytes);
        else if (request->IoControlCode == WGPU_DISPLAY_FILL_IOCTL)
            status=wgpu_display_fill_send(in,in_bytes,out,out_bytes);
        else
            status=wgpu_display_send(in,in_bytes,out,out_bytes);
        request->StatusBlock->Status = status == 0 ? NO_ERROR : ERROR_INVALID_PARAMETER;
        request->StatusBlock->Information = status == 0 ? request->OutputBufferLength : 0;
        return TRUE;
    }
    return StartIO(hw, request);
}
static ULONG NTAPI wgpu_video_initialize(PVOID first, PVOID second,
    PVIDEO_HW_INITIALIZATION_DATA data, PVOID context)
{
#ifdef WGPU_NT_PROFILE_TRANSPORT
    wgpu_display_transport_init();
#endif
    data->HwStartIO = wgpu_start_io;
    return VideoPortInitialize(first, second, data, context);
}
