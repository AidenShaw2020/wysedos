/* SPDX-License-Identifier: GPL-3.0-or-later
 * Video miniport -> existing PnP WDM owner. No PCI scans, BAR mappings, raw
 * batches or caller-selected device names. No retained cross-driver pointer.
 */
#include <ddk/ntddk.h>
#include "wgpu_display.h"

#ifdef WGPU_NT_PROFILE_TRANSPORT
#include "../wgpu_profile_clock.h"
static KMUTEX transport_timing_lock;
static WgpuDisplayTransportTiming transport_timing;

void NTAPI wgpu_display_transport_init(void)
{
    KeInitializeMutex(&transport_timing_lock, 0);
}

NTSTATUS NTAPI wgpu_display_transport_read(PVOID output, ULONG output_bytes)
{
    if (!output || output_bytes != sizeof(transport_timing)) return STATUS_INVALID_PARAMETER;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    KeWaitForSingleObject(&transport_timing_lock, Executive, KernelMode, FALSE, NULL);
    RtlCopyMemory(output, &transport_timing, sizeof(transport_timing));
    KeReleaseMutex(&transport_timing_lock, FALSE);
    return STATUS_SUCCESS;
}
#endif

static NTSTATUS wgpu_call_io(ULONG code, PVOID input, ULONG input_bytes,
                               PVOID output, ULONG output_bytes,
                               ULONG *returned)
{
    static const WCHAR name[] = L"\\Device\\WyseWgpuNT";
    UNICODE_STRING path;
    PFILE_OBJECT file;
    PDEVICE_OBJECT device;
    PIRP irp;
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;
#ifdef WGPU_NT_PROFILE_TRANSPORT
    const BOOLEAN profile = code == WGPU_NT_IOCTL_QUEUE_MATERIAL ||
        code == WGPU_NT_IOCTL_DRAIN_MATERIAL;
    ULONGLONG start, opened, prepared, dispatched;
#endif
    *returned = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
#ifdef WGPU_NT_PROFILE_TRANSPORT
    start = profile ? wgpu_profile_ticks() : 0;
#endif
    RtlInitUnicodeString(&path, name);
    status = IoGetDeviceObjectPointer(&path, FILE_READ_DATA | FILE_WRITE_DATA,
                                      &file, &device);
    if (!NT_SUCCESS(status)) return status;
#ifdef WGPU_NT_PROFILE_TRANSPORT
    opened = profile ? wgpu_profile_ticks() : 0;
#endif
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    iosb.Status = STATUS_UNSUCCESSFUL;
    iosb.Information = 0;
    irp = IoBuildDeviceIoControlRequest(code, device, input, input_bytes, output,
                                        output_bytes, FALSE, &event, &iosb);
    if (!irp) {
        ObDereferenceObject(file);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    IoGetNextIrpStackLocation(irp)->FileObject = file;
#ifdef WGPU_NT_PROFILE_TRANSPORT
    prepared = profile ? wgpu_profile_ticks() : 0;
#endif
    status = IoCallDriver(device, irp);
    if (status == STATUS_PENDING) {
        /* Nonalertable completion wait: event, IOSB and buffers must remain
         * live until the I/O manager completes/frees this synchronous IRP.
         * The WDM owner bounds GPU polling to five seconds. Never abandon a
         * pending IRP with stack-backed event/IOSB on a timeout. */
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    if (NT_SUCCESS(status)) {
        if (iosb.Information != output_bytes) status = STATUS_DEVICE_PROTOCOL_ERROR;
        else *returned = output_bytes;
    }
#ifdef WGPU_NT_PROFILE_TRANSPORT
    dispatched = profile ? wgpu_profile_ticks() : 0;
#endif
    ObDereferenceObject(file);
#ifdef WGPU_NT_PROFILE_TRANSPORT
    if (profile) {
        const ULONGLONG closed = wgpu_profile_ticks();
        KeWaitForSingleObject(&transport_timing_lock, Executive, KernelMode, FALSE, NULL);
        transport_timing.queues += code == WGPU_NT_IOCTL_QUEUE_MATERIAL;
        transport_timing.drains += code == WGPU_NT_IOCTL_DRAIN_MATERIAL;
        transport_timing.open_ticks += opened - start;
        transport_timing.prepare_ticks += prepared - opened;
        transport_timing.dispatch_ticks += dispatched - prepared;
        transport_timing.close_ticks += closed - dispatched;
        KeReleaseMutex(&transport_timing_lock, FALSE);
    }
#endif
    return status;
}

static NTSTATUS wgpu_call_owner(ULONG operation, PVOID output, ULONG output_bytes,
                               ULONG *returned)
{
    ULONG code;
    if (operation == WGPU_DISPLAY_QUERY) code=WGPU_NT_IOCTL_QUERY;
    else if (operation == WGPU_DISPLAY_SELFTEST) code=WGPU_NT_IOCTL_SELFTEST;
    else if (operation == WGPU_DISPLAY_TRIANGLE) code=WGPU_NT_IOCTL_TRIANGLE;
    else if (operation == WGPU_DISPLAY_DRAW_TEST) code=WGPU_NT_IOCTL_DRAW_TEST;
    else return STATUS_INVALID_PARAMETER;
    return wgpu_call_io(code,NULL,0,output,output_bytes,returned);
}

NTSTATUS NTAPI wgpu_display_send(const WgpuDisplayRequest *input, ULONG input_bytes,
                                 PVOID output, ULONG output_bytes)
{
    WgpuDisplayRequest request;
    WgpuDisplayReply *reply = output;
    ULONG bytes = 0, payload;
    if (!input || !reply || input_bytes != sizeof(request))
        return STATUS_INVALID_PARAMETER;
    /* Input and output can alias for METHOD_BUFFERED. Preserve the request
     * before zeroing any output; reject malformed requests before opening. */
    RtlCopyMemory(&request, input, sizeof(request));
    if (!wgpu_display_request_valid(&request, input_bytes, output_bytes))
        return STATUS_INVALID_PARAMETER;
    payload = wgpu_display_payload(request.operation);
    RtlZeroMemory(reply, output_bytes);
    reply->size = output_bytes;
    reply->abi_version = WGPU_DISPLAY_ABI;
    reply->status = wgpu_call_owner(request.operation, reply + 1, payload, &bytes);
    reply->bytes = bytes;
    if (!NT_SUCCESS((NTSTATUS)reply->status)) RtlZeroMemory(reply + 1, payload);
    return STATUS_SUCCESS;
}

static NTSTATUS wgpu_display_surface_send(ULONG code, const void *input, ULONG input_bytes,
                                          PVOID output, ULONG output_bytes)
{
    union { WgpuNtFill fill; WgpuNtCopy copy; } request;
    WgpuDisplayReply *reply=output;
    ULONG bytes=0;
    ULONG expected=code==WGPU_NT_IOCTL_COPY ? sizeof(request.copy) : sizeof(request.fill);
    if (!input || !output || input_bytes!=expected ||
        output_bytes!=sizeof(*reply)+sizeof(WgpuNtFillResult)) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(&request,input,input_bytes);
    /* Structural precheck only. The WDM owner validates the actual LFB bounds
     * and derives its GPU address from the device's read-only registers. */
    if (!(code==WGPU_NT_IOCTL_COPY ? wgpu_nt_copy_valid(&request.copy,0x10000000ul,0x1000ul)
                                 : wgpu_nt_fill_valid(&request.fill,0x10000000ul,0x1000ul)))
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(output,output_bytes);
    reply->size=output_bytes;reply->abi_version=WGPU_DISPLAY_ABI;
    reply->status=wgpu_call_io(code,&request,input_bytes,
                               reply+1,sizeof(WgpuNtFillResult),&bytes);
    reply->bytes=bytes;
    if (!NT_SUCCESS((NTSTATUS)reply->status)) RtlZeroMemory(reply+1,sizeof(WgpuNtFillResult));
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI wgpu_display_fill_send(const WgpuNtFill *input, ULONG input_bytes,
                                     PVOID output, ULONG output_bytes)
{
    return wgpu_display_surface_send(WGPU_NT_IOCTL_FILL,input,input_bytes,output,output_bytes);
}

NTSTATUS NTAPI wgpu_display_copy_send(const WgpuNtCopy *input, ULONG input_bytes,
                                     PVOID output, ULONG output_bytes)
{
    return wgpu_display_surface_send(WGPU_NT_IOCTL_COPY,input,input_bytes,output,output_bytes);
}

static NTSTATUS wgpu_display_vertices_send(ULONG code, const void *input, ULONG input_bytes,
                                           PVOID output, ULONG output_bytes)
{
    WgpuDisplayReply header;
    WgpuNtFillResult result;
    if (!output || output_bytes!=sizeof(header)+sizeof(result))
        return STATUS_INVALID_PARAMETER;
    header.size=output_bytes;
    header.abi_version=WGPU_DISPLAY_ABI;
    header.bytes=0;
    RtlZeroMemory(&result,sizeof(result));
    /* Use separate scratch output so aliased video I/O buffers do not
     * overwrite the vertices before the WDM request copies the input. */
    header.status=wgpu_call_io(code,(PVOID)input,input_bytes,
                               &result,sizeof(result),&header.bytes);
    if (!NT_SUCCESS((NTSTATUS)header.status)) RtlZeroMemory(&result,sizeof(result));
    RtlCopyMemory(output,&header,sizeof(header));
    RtlCopyMemory((WgpuDisplayReply *)output+1,&result,sizeof(result));
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI wgpu_display_draw_send(const WgpuNtDraw *input, ULONG input_bytes,
                                     PVOID output, ULONG output_bytes)
{
    if (!input || input_bytes != sizeof(*input) ||
        !wgpu_nt_draw_valid(input, 0x10000000ul, 0x1000ul)) return STATUS_INVALID_PARAMETER;
    return wgpu_display_vertices_send(WGPU_NT_IOCTL_DRAW, input, input_bytes, output, output_bytes);
}

#ifdef WGPU_NT_ENABLE_MATERIAL
NTSTATUS NTAPI wgpu_display_material_send(const WgpuNtMaterialDraw *input, ULONG input_bytes,
                                         PVOID output, ULONG output_bytes)
{
    if (!wgpu_nt_material_packet_valid(input, input_bytes) ||
        !wgpu_nt_material_valid(input, 0x10000000ul, 0x1000ul)) return STATUS_INVALID_PARAMETER;
    return wgpu_display_vertices_send(WGPU_NT_IOCTL_MATERIAL, input, input_bytes, output, output_bytes);
}
#endif

#ifdef WGPU_NT_PIPELINE
NTSTATUS NTAPI wgpu_display_pipeline_send(ULONG code, const void *input, ULONG input_bytes,
                                          PVOID output, ULONG output_bytes)
{
    ULONG returned;
    if (code == WGPU_DISPLAY_DRAIN_MATERIAL_IOCTL) {
        if (input_bytes || output_bytes) return STATUS_INVALID_PARAMETER;
        return wgpu_call_io(WGPU_NT_IOCTL_DRAIN_MATERIAL, NULL, 0, NULL, 0, &returned);
    }
    if (!wgpu_nt_material_group_valid(input, input_bytes)) return STATUS_INVALID_PARAMETER;
    return wgpu_display_vertices_send(WGPU_NT_IOCTL_QUEUE_MATERIAL, input, input_bytes,
                                      output, output_bytes);
}
#endif

#ifdef WGPU_NT_ENABLE_GROUPS
NTSTATUS NTAPI wgpu_display_material_group_send(const WgpuNtMaterialGroup *input,
                                               ULONG input_bytes, PVOID output, ULONG output_bytes)
{
    if (!wgpu_nt_material_group_valid(input, input_bytes)) return STATUS_INVALID_PARAMETER;
    return wgpu_display_vertices_send(WGPU_NT_IOCTL_MATERIAL_GROUP, input, input_bytes,
                                      output, output_bytes);
}
#endif
