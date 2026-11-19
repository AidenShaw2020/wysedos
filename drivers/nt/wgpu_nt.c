/* SPDX-License-Identifier: GPL-3.0-or-later
 * Experimental x86 NT 5 WDM bridge for the WGPU co-processor (pc/wgpu.h).
 * Uses only assigned resources; does not claim the emulated VGA adapter.
 */
#include <ddk/ntddk.h>
#include "wgpu_nt.h"
#include "wgpu_fill.h"
#include "wgpu_draw.h"
#ifdef WGPU_NT_ENABLE_MATERIAL
#include "wgpu_material.h"
#endif

/* Pin the x86 NT5 fields used below when compiling with a newer DDK. */
typedef char WgpuNtDriverObjectLayout[__builtin_offsetof(DRIVER_OBJECT, MajorFunction) == 0x38 ? 1 : -1];
typedef char WgpuNtDeviceObjectLayout[__builtin_offsetof(DEVICE_OBJECT, DeviceExtension) == 0x28 ? 1 : -1];
typedef char WgpuNtIrpLayout[sizeof(IRP) == 0x70 ? 1 : -1];
typedef char WgpuNtStackLayout[sizeof(IO_STACK_LOCATION) == 0x24 ? 1 : -1];

#define WGPU_TAG 0x4e706757ul
#define WGPU_READY 1ul
#define WGPU_ERROR 2ul
#define WGPU_BUSY 4ul

typedef struct WgpuNtDevice {
    PDEVICE_OBJECT lower;
    IO_REMOVE_LOCK remove_lock;
    KMUTEX mutex;
    PUCHAR registers;
    volatile ULONG *aperture;
    ULONG aperture_map_bytes;
    ULONG aperture_bytes;
    ULONG surface_lfb_bytes, surface_lfb_gfx, surface_gfx, surface_3d_flags;
#ifdef WGPU_NT_PIPELINE
    ULONG pending_fence;
#ifdef WGPU_NT_QUEUED_GROUPS
    ULONG queue_fences[WGPU_NT_MATERIAL_ARENAS];
    unsigned queue_head, queue_count;
    WgpuNtQueueStats queue_stats;
#else
    unsigned pending_arena;
#endif
    WgpuNtPipelineStats pipeline_stats;
#endif
#ifdef WGPU_NT_SHADER_CACHE
    WgpuNtMaterialCache material_cache;
#endif
    BOOLEAN started, cpu_fences, surface_ready;
#ifdef WGPU_NT_PINNED_FENCE
    BOOLEAN fence_programmed;
#endif
    ULONG smoke_value, smoke_expected, smoke_fence, smoke_result;
#ifdef WGPU_NT_TIMING
    WgpuNtTiming timing;
#endif
} WgpuNtDevice;

#ifdef WGPU_NT_TIMING
#ifdef WGPU_NT_PROFILE_TSC
#include "wgpu_profile_clock.h"
#define WGPU_TICKS() wgpu_profile_ticks()
#else
#define WGPU_TICKS() KeQueryPerformanceCounter(NULL).QuadPart
#endif
#define WGPU_TIMER(start) LONGLONG start = WGPU_TICKS()
#define WGPU_ELAPSED(field, start) (device->timing.field += WGPU_TICKS() - (start))
#else
#define WGPU_TIMER(start) ((void)0)
#define WGPU_ELAPSED(field, start) ((void)0)
#endif

static const WCHAR device_name[] = L"\\Device\\WyseWgpuNT";
static const WCHAR link_name[] = L"\\DosDevices\\WyseWgpuNT";

static ULONG wgpu_read(WgpuNtDevice *device, ULONG offset)
{
    return *(volatile ULONG *)(device->registers + offset);
}

static void wgpu_write(WgpuNtDevice *device, ULONG offset, ULONG value)
{
    *(volatile ULONG *)(device->registers + offset) = value;
}

static void wgpu_lock(WgpuNtDevice *device)
{
    KeWaitForSingleObject(&device->mutex, Executive, KernelMode, FALSE, NULL);
}

static void wgpu_unlock(WgpuNtDevice *device)
{
    KeReleaseMutex(&device->mutex, FALSE);
}

static void wgpu_unmap(WgpuNtDevice *device)
{
    device->started = FALSE;
#ifdef WGPU_NT_PINNED_FENCE
    device->fence_programmed = FALSE;
#endif
    device->surface_ready = FALSE;
#ifdef WGPU_NT_PIPELINE
    device->pending_fence = 0;
#ifdef WGPU_NT_QUEUED_GROUPS
    device->queue_head = device->queue_count = 0;
    RtlZeroMemory(&device->queue_stats, sizeof(device->queue_stats));
#endif
    RtlZeroMemory(&device->pipeline_stats, sizeof(device->pipeline_stats));
#endif
#ifdef WGPU_NT_SHADER_CACHE
    device->material_cache.shaders = 0;
#ifdef WGPU_NT_BATCH_TEMPLATES
    if (device->material_cache.templates) {
        ExFreePoolWithTag(device->material_cache.templates, WGPU_TAG);
        device->material_cache.templates = NULL;
    }
    RtlZeroMemory(&device->material_cache.template_stats, sizeof(WgpuNtTemplateStats));
#endif
#endif
    if (device->aperture) {
        MmUnmapIoSpace((PVOID)device->aperture, device->aperture_map_bytes);
        device->aperture = NULL;
    }
    if (device->registers) {
        MmUnmapIoSpace(device->registers, WGPU_NT_REGISTER_BYTES);
        device->registers = NULL;
    }
}

static BOOLEAN wgpu_has_fences(void)
{
    ULONG a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    return (d & (1ul << 26)) != 0;
}

static NTSTATUS wgpu_start(WgpuNtDevice *device, PCM_RESOURCE_LIST resources)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor;
    PHYSICAL_ADDRESS registers, aperture;
    ULONG aperture_bytes, i, j, found;
    /* A PCI function has one full resource descriptor. Reject other layouts
     * rather than indexing a variable-length CM_FULL_RESOURCE_DESCRIPTOR. */
    if (!resources || resources->Count != 1) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    registers.QuadPart = aperture.QuadPart = 0;
    aperture_bytes = found = 0;
    for (i = 0; i < resources->Count; ++i) {
        PCM_PARTIAL_RESOURCE_LIST list = &resources->List[i].PartialResourceList;
        if (list->Count > 32) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        for (j = 0; j < list->Count; ++j) {
            descriptor = &list->PartialDescriptors[j];
            if (descriptor->Type != CmResourceTypeMemory) {
                continue;
            }
            if (descriptor->u.Memory.Length == WGPU_NT_REGISTER_BYTES && !(found & 1)) {
                registers = descriptor->u.Memory.Start;
                found |= 1;
            } else if (descriptor->u.Memory.Length >= 1024ul * 1024 && !(found & 2)) {
                aperture = descriptor->u.Memory.Start;
                aperture_bytes = descriptor->u.Memory.Length;
                found |= 2;
            } else {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
        }
    }
    if (found != 3) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    device->registers = MmMapIoSpace(registers, WGPU_NT_REGISTER_BYTES, MmNonCached);
    if (!device->registers) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (wgpu_read(device, 0x00) != WGPU_NT_MAGIC ||
        wgpu_read(device, 0x04) != WGPU_NT_PROTOCOL) {
        wgpu_unmap(device);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    device->aperture_bytes = wgpu_read(device, 0x10);
    if (device->aperture_bytes < 1024ul * 1024 ||
        (device->aperture_bytes & (device->aperture_bytes - 1)) ||
        device->aperture_bytes > aperture_bytes) {
        wgpu_unmap(device);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    device->aperture_map_bytes = aperture_bytes;
    device->aperture = MmMapIoSpace(aperture, aperture_bytes, MmWriteCombined);
    if (!device->aperture) {
        wgpu_unmap(device);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    device->cpu_fences = wgpu_has_fences();
#ifdef WGPU_NT_BATCH_TEMPLATES
    if (device->cpu_fences) {
        const SIZE_T bytes = wgpu_nt_template_cache_size();
        device->material_cache.templates = ExAllocatePoolWithTag(NonPagedPool, bytes, WGPU_TAG);
        if (device->material_cache.templates)
            RtlZeroMemory(device->material_cache.templates, bytes);
    }
#endif
    device->started = TRUE;
    return STATUS_SUCCESS;
}

static void wgpu_query(WgpuNtDevice *device, WgpuNtInfo *info)
{
    info->size = sizeof(*info);
    info->abi_version = WGPU_NT_ABI_VERSION;
    info->magic = wgpu_read(device, 0x00);
    info->protocol = wgpu_read(device, 0x04);
    info->gpu_id = wgpu_read(device, 0x08);
    info->status = wgpu_read(device, 0x0c);
    info->aperture_bytes = wgpu_read(device, 0x10);
    info->aperture_gfx = wgpu_read(device, 0x14);
    info->lfb_bytes = wgpu_read(device, 0x18);
    info->lfb_gfx = wgpu_read(device, 0x1c);
    info->submitted = wgpu_read(device, 0x28);
    info->completed = wgpu_read(device, 0x2c);
    info->resets = wgpu_read(device, 0x34);
    info->submits = wgpu_read(device, 0x38);
    info->user_3d_submits = wgpu_read(device, 0x40);
    info->smoke_value = device->smoke_value;
    info->smoke_expected = device->smoke_expected;
    info->smoke_fence = device->smoke_fence;
    info->smoke_result = device->smoke_result;
}

static NTSTATUS wgpu_ring_doorbell(WgpuNtDevice *device)
{
    ULONG result;
    WGPU_TIMER(doorbell_start);
    /* SFENCE/LFENCE do not touch XMM state; callers checked SSE2 and idle. */
    __asm__ __volatile__("sfence" ::: "memory");
#ifdef WGPU_NT_PINNED_FENCE
    /* All operations through this owner use aperture word zero. PnP starts
     * with the hardware register until the first verified-idle submission. */
    if (!device->fence_programmed) {
        wgpu_write(device, 0x20, 0);
        device->fence_programmed = TRUE;
    }
#else
    wgpu_write(device, 0x20, 0);
#endif
    wgpu_write(device, 0x24, WGPU_NT_DESCRIPTOR);
    result = wgpu_read(device, 0x24);
    WGPU_ELAPSED(doorbell_ticks, doorbell_start);
    return result ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
}

static BOOLEAN wgpu_fence_pending(ULONG completed, ULONG wanted, BOOLEAN allow_later)
{
    return allow_later ? !wgpu_nt_fence_reached(completed, wanted) : completed != wanted;
}

static ULONG wgpu_completed(WgpuNtDevice *device)
{
#ifdef WGPU_NT_PINNED_FENCE
    /* COMPLETED is the same aperture word read by the monitor. We use the
     * mapped word only once this owner has established the fence address. */
    if (device->fence_programmed) return device->aperture[0];
#endif
    return wgpu_read(device, 0x2c);
}

static NTSTATUS wgpu_wait_fence(WgpuNtDevice *device, ULONG fence, BOOLEAN allow_later)
{
    unsigned poll;
    NTSTATUS status;
    WGPU_TIMER(fence_start);
    /* Small draws can finish before NT's next scheduler tick. Check the
     * shared fence briefly before yielding; keep the slow path for busy or
     * stalled hardware. A completed fence still needs the status checks. */
    for (poll = 0; poll < 256 && wgpu_fence_pending(device->aperture[0], fence, allow_later); ++poll) {
        __asm__ __volatile__("pause");
    }
#ifdef WGPU_NT_SHORT_WAIT
    /* A multi-draw chain often outlasts the initial poll but completes
     * before a scheduler sleep. Recheck at short intervals and give up
     * the CPU after at most 256 microseconds of requested stalls. */
    for (poll = 0; poll < 32 && wgpu_fence_pending(device->aperture[0], fence, allow_later); ++poll) {
        KeStallExecutionProcessor(8);
    }
#endif
    if (wgpu_fence_pending(device->aperture[0], fence, allow_later)) {
        ULONGLONG deadline = KeQueryInterruptTime() + 5ull * 10000000;
        LARGE_INTEGER delay;
        delay.QuadPart = -10000; /* The NT timer resolution may round up. */
        while (wgpu_fence_pending(device->aperture[0], fence, allow_later)) {
            /* Status reads also let the monitor detect GPU hangs. */
            if ((wgpu_read(device, 0x0c) & WGPU_ERROR) ||
                KeQueryInterruptTime() >= deadline) {
                WGPU_ELAPSED(fence_ticks, fence_start);
                return STATUS_IO_TIMEOUT;
            }
#ifdef WGPU_NT_TIMING
            ++device->timing.scheduler_sleeps;
#endif
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }
    }
    __asm__ __volatile__("lfence" ::: "memory");
    status = (wgpu_read(device, 0x0c) & WGPU_ERROR) ||
        wgpu_fence_pending(wgpu_completed(device), fence, allow_later)
        ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
    WGPU_ELAPSED(fence_ticks, fence_start);
    return status;
}

static NTSTATUS wgpu_submit_wait(WgpuNtDevice *device, ULONG fence)
{
    NTSTATUS status = wgpu_ring_doorbell(device);
    if (!NT_SUCCESS(status)) return status;
    return wgpu_wait_fence(device, fence, FALSE);
}

#ifdef WGPU_NT_PIPELINE
static NTSTATUS wgpu_retire_pending(WgpuNtDevice *device)
{
    NTSTATUS status;
    if (!device->pending_fence) return STATUS_SUCCESS;
    status = wgpu_wait_fence(device, device->pending_fence, FALSE);
    if (NT_SUCCESS(status)) {
        device->pending_fence = 0;
#ifdef WGPU_NT_QUEUED_GROUPS
        device->pipeline_stats.retired += device->queue_count;
        device->queue_head = device->queue_count = 0;
#else
        ++device->pipeline_stats.retired;
#endif
    }
    return status;
}
#endif

static ULONG wgpu_next_fence(ULONG submitted)
{
    return submitted == MAXULONG ? 1 : submitted + 1;
}

static NTSTATUS wgpu_idle_fence(WgpuNtDevice *device, ULONG *submitted, ULONG *fence)
{
    ULONG status;
    if (!device->cpu_fences) {
        return STATUS_NOT_SUPPORTED;
    }
    status = wgpu_read(device, 0x0c);
    *submitted = wgpu_read(device, 0x28);
    if (!(status & WGPU_READY) || (status & (WGPU_BUSY | WGPU_ERROR)) ||
        *submitted != wgpu_completed(device)) {
        return STATUS_DEVICE_NOT_READY;
    }
    *fence = wgpu_next_fence(*submitted);
    return STATUS_SUCCESS;
}

#ifdef WGPU_NT_PIPELINE
static NTSTATUS wgpu_queue_material(WgpuNtDevice *device, const WgpuNtMaterialGroup *group,
                                    ULONG bytes, WgpuNtMaterialCache *cache, ULONG *fence)
{
    WgpuNtMaterialPrepared prepared;
    ULONG submitted;
    const BOOLEAN pending = device->pending_fence != 0;
#ifdef WGPU_NT_QUEUED_GROUPS
    unsigned arena;
#else
    unsigned arena = pending ? device->pending_arena ^ 1u : 0;
#endif
    NTSTATUS status;
    if (!pending) {
        status = wgpu_idle_fence(device, &submitted, fence);
        if (!NT_SUCCESS(status)) return status;
    }
#ifdef WGPU_NT_QUEUED_GROUPS
    if (device->queue_count == WGPU_NT_MATERIAL_ARENAS) {
        ++device->queue_stats.slot_retirements;
        status = wgpu_wait_fence(device, device->queue_fences[device->queue_head], TRUE);
        if (!NT_SUCCESS(status)) return status;
        device->queue_head = (device->queue_head + 1) % WGPU_NT_MATERIAL_ARENAS;
        --device->queue_count;
        ++device->pipeline_stats.retired;
    }
    arena = (device->queue_head + device->queue_count) % WGPU_NT_MATERIAL_ARENAS;
#endif
    WGPU_TIMER(build_start);
    /* A previous request may still read its arena and the shared fence.
     * Only fill the other arena and unused immutable shader slots here. */
    if (!cache || !device->cpu_fences ||
        !wgpu_nt_prepare_material_group(device->aperture, device->aperture_bytes,
            device->surface_gfx, arena, group, bytes, device->surface_lfb_bytes,
            device->surface_lfb_gfx, cache, &prepared))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    WGPU_ELAPSED(build_ticks, build_start);
    if (pending) {
        submitted = device->pending_fence;
        ++device->pipeline_stats.prepared_while_pending;
#ifndef WGPU_NT_QUEUED_GROUPS
        status = wgpu_retire_pending(device);
        if (!NT_SUCCESS(status)) return status;
        /* The guest fence precedes the ring's own completion word. The
         * consumed arena is safe even if that final ring store is pending. */
        if (wgpu_read(device, 0x28) != submitted) return STATUS_DEVICE_NOT_READY;
#endif
        *fence = wgpu_next_fence(submitted);
    }
#ifdef WGPU_NT_QUEUED_GROUPS
    wgpu_nt_queue_material_descriptor(device->aperture, &prepared, *fence, device->surface_3d_flags);
#else
    wgpu_nt_publish_material_group(device->aperture, &prepared, submitted, *fence,
                                   device->surface_3d_flags);
#endif
    status = wgpu_ring_doorbell(device);
    if (NT_SUCCESS(status)) {
        device->pending_fence = *fence;
#ifdef WGPU_NT_QUEUED_GROUPS
        device->queue_fences[arena] = *fence;
        ++device->queue_count;
        if (device->queue_count > device->queue_stats.max_pending)
            device->queue_stats.max_pending = device->queue_count;
#else
        device->pending_arena = arena;
#endif
        ++device->pipeline_stats.queued;
    }
    return status;
}
#endif

static NTSTATUS wgpu_selftest(WgpuNtDevice *device)
{
    ULONG submitted, fence, gfx;
    NTSTATUS status = wgpu_idle_fence(device, &submitted, &fence);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    gfx = wgpu_read(device, 0x14);
    device->smoke_expected = 0x5eed0000ul | (fence & 0xfffful);
    device->smoke_value = 0;
    device->smoke_fence = fence;
    device->smoke_result = 2;
    if (!wgpu_nt_build_smoke(device->aperture, device->aperture_bytes, gfx,
                             submitted, fence, device->smoke_expected)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    device->smoke_result = 3;
    status = wgpu_submit_wait(device, fence);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    device->smoke_value = device->aperture[WGPU_NT_TARGET / 4];
    if (device->smoke_value != device->smoke_expected ||
        (wgpu_read(device, 0x0c) & WGPU_ERROR)) {
        device->smoke_result = 4;
        return STATUS_IO_DEVICE_ERROR;
    }
    device->smoke_result = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS wgpu_triangle(WgpuNtDevice *device, WgpuNtTriangle *output, int draw_test)
{
    static const WgpuNtDraw sample = {
        {sizeof(WgpuNtFill), WGPU_NT_ABI_VERSION, WGPU_NT_TRI_TARGET, WGPU_NT_TRI_WIDTH*2,
         WGPU_NT_TRI_WIDTH, WGPU_NT_TRI_HEIGHT, 16, 0, 0, WGPU_NT_TRI_WIDTH, WGPU_NT_TRI_HEIGHT, 0, 0},
        sizeof(WgpuNtDraw), 6,
        {{0x41000000,0x41800000,0xffff0000}, {0x42400000,0x41800000,0xffff0000},
         {0x41000000,0x42600000,0xffff0000}, {0x42800000,0x42800000,0xff00ff00},
         {0x42e00000,0x42800000,0xff00ff00}, {0x42800000,0x42d00000,0xff00ff00}}
    };
    ULONG submitted, fence, gfx, i;
    volatile const USHORT *pixels;
    NTSTATUS status = wgpu_idle_fence(device, &submitted, &fence);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if ((wgpu_read(device, 0x08) & 0xfffful) != 0x22b0ul) {
        return STATUS_NOT_SUPPORTED;
    }
    gfx = wgpu_read(device, 0x14);
    /* Fixed driver probes do not represent application 3D activity. */
    if (draw_test) {
        if (!wgpu_nt_build_draw(device->aperture, device->aperture_bytes, gfx,
                               submitted, fence, 0, &sample, device->aperture_bytes, gfx))
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        for (i=0; i<WGPU_NT_TRI_WIDTH*WGPU_NT_TRI_HEIGHT/2; ++i)
            device->aperture[WGPU_NT_TRI_TARGET/4+i]=0x10821082u;
    } else if (!wgpu_nt_build_triangle(device->aperture, device->aperture_bytes,
                                gfx, submitted, fence, 0)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    status = wgpu_submit_wait(device, fence);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    output->size = sizeof(*output);
    output->abi_version = WGPU_NT_ABI_VERSION;
    output->width = WGPU_NT_TRI_WIDTH;
    output->height = WGPU_NT_TRI_HEIGHT;
    output->pitch = WGPU_NT_TRI_WIDTH * 2;
    output->fence = fence;
    output->flags = 0;
    pixels = (volatile const USHORT *)((PUCHAR)device->aperture + WGPU_NT_TRI_TARGET);
    for (i = 0; i < WGPU_NT_TRI_WIDTH * WGPU_NT_TRI_HEIGHT; ++i) {
        output->pixels[i] = pixels[i];
    }
    return STATUS_SUCCESS;
}

static void wgpu_surface_result(WgpuNtFillResult *output, ULONG fence)
{
    RtlZeroMemory(output, sizeof(*output));
    output->size = sizeof(*output);
    output->abi_version = WGPU_NT_ABI_VERSION;
    output->fence = fence;
}

static NTSTATUS wgpu_surface_op(WgpuNtDevice *device, ULONG code, const void *request,
                                ULONG input_bytes, WgpuNtFillResult *output)
{
    ULONG submitted,fence,gfx,flags,built;
#ifdef WGPU_NT_ENABLE_MATERIAL
    WgpuNtMaterialCache *cache = NULL;
#ifdef WGPU_NT_SHADER_CACHE
    cache = &device->material_cache;
#endif
#endif
    WGPU_TIMER(prepare_start);
    ULONG lfb_bytes, lfb_gfx;
    NTSTATUS status;
    /* GGTT placement and protocol features stay fixed for a started
     * device. Read them lazily to avoid claiming the GPU at driver load. */
    if (!device->surface_ready) {
        if ((wgpu_read(device, 0x08) & 0xfffful) != 0x22b0ul) return STATUS_NOT_SUPPORTED;
        device->surface_lfb_bytes = wgpu_read(device, 0x18);
        device->surface_lfb_gfx = wgpu_read(device, 0x1c);
        device->surface_gfx = wgpu_read(device, 0x14);
        device->surface_3d_flags = wgpu_read(device, 0x3c) & WGPU_NT_FLAG_USER_3D;
        device->surface_ready = TRUE;
    }
    lfb_bytes = device->surface_lfb_bytes;
    lfb_gfx = device->surface_lfb_gfx;
#ifdef WGPU_NT_PIPELINE
    if (code == WGPU_NT_IOCTL_QUEUE_MATERIAL) {
        WGPU_ELAPSED(prepare_ticks, prepare_start);
        status = wgpu_queue_material(device, request, input_bytes, cache, &fence);
        if (NT_SUCCESS(status)) wgpu_surface_result(output, fence);
        return status;
    }
#endif
#ifndef WGPU_NT_ENABLE_GROUPS
    (void)input_bytes;
#else
    if (code == WGPU_NT_IOCTL_MATERIAL_GROUP) {
        if (!wgpu_nt_material_group_valid(request, input_bytes)) return STATUS_INVALID_PARAMETER;
    } else
#endif
#ifdef WGPU_NT_ENABLE_MATERIAL
    if (code==WGPU_NT_IOCTL_MATERIAL) {
        if (!wgpu_nt_material_valid(request,lfb_bytes,lfb_gfx)) return STATUS_INVALID_PARAMETER;
    } else
#endif
    if (code==WGPU_NT_IOCTL_DRAW) {
        if (!wgpu_nt_draw_valid(request,lfb_bytes,lfb_gfx)) return STATUS_INVALID_PARAMETER;
    } else if (!(code==WGPU_NT_IOCTL_COPY ? wgpu_nt_copy_valid(request,lfb_bytes,lfb_gfx)
                                        : wgpu_nt_fill_valid(request,lfb_bytes,lfb_gfx))) {
        return STATUS_INVALID_PARAMETER;
    }
    status=wgpu_idle_fence(device,&submitted,&fence);
    if (!NT_SUCCESS(status)) return status;
    gfx = device->surface_gfx;
    /* 2D uses the render engine too, but must not renew the CPU boost. */
    flags = code == WGPU_NT_IOCTL_FILL || code == WGPU_NT_IOCTL_COPY ?
        0 : device->surface_3d_flags;
    WGPU_ELAPSED(prepare_ticks, prepare_start);
    WGPU_TIMER(build_start);
#ifdef WGPU_NT_ENABLE_GROUPS
    if (code == WGPU_NT_IOCTL_MATERIAL_GROUP)
        built = wgpu_nt_build_material_group(device->aperture, device->aperture_bytes,
            gfx, submitted, fence, flags, request, input_bytes, lfb_bytes, lfb_gfx, cache);
    else
#endif
#ifdef WGPU_NT_ENABLE_MATERIAL
    if (code==WGPU_NT_IOCTL_MATERIAL)
        built=wgpu_nt_build_material(device->aperture,device->aperture_bytes,gfx,submitted,fence,flags,
                                    request,lfb_bytes,lfb_gfx,cache);
    else
#endif
    if (code==WGPU_NT_IOCTL_DRAW)
        built=wgpu_nt_build_draw(device->aperture,device->aperture_bytes,gfx,submitted,fence,flags,
                                request,lfb_bytes,lfb_gfx);
    else if (code==WGPU_NT_IOCTL_COPY)
        built=wgpu_nt_build_copy(device->aperture,device->aperture_bytes,gfx,submitted,fence,flags,
                                request,lfb_bytes,lfb_gfx);
    else
        built=wgpu_nt_build_fill(device->aperture,device->aperture_bytes,gfx,submitted,fence,flags,
                                request,lfb_bytes,lfb_gfx);
    WGPU_ELAPSED(build_ticks, build_start);
    if (!built) return STATUS_DEVICE_CONFIGURATION_ERROR;
    status=wgpu_submit_wait(device,fence);
    if (!NT_SUCCESS(status)) return status;
    wgpu_surface_result(output, fence);
    return STATUS_SUCCESS;
}

static ULONG wgpu_surface_request_size(ULONG code)
{
    switch (code) {
    case WGPU_NT_IOCTL_FILL: return sizeof(WgpuNtFill);
    case WGPU_NT_IOCTL_COPY: return sizeof(WgpuNtCopy);
    case WGPU_NT_IOCTL_DRAW: return sizeof(WgpuNtDraw);
#ifdef WGPU_NT_ENABLE_GROUPS
#ifdef WGPU_NT_PIPELINE
    case WGPU_NT_IOCTL_QUEUE_MATERIAL:
#endif
    case WGPU_NT_IOCTL_MATERIAL_GROUP: return sizeof(WgpuNtMaterialGroup);
#endif
#ifdef WGPU_NT_ENABLE_MATERIAL
    case WGPU_NT_IOCTL_MATERIAL: return sizeof(WgpuNtMaterialDraw);
#endif
    default: return 0;
    }
}

static NTSTATUS wgpu_complete(WgpuNtDevice *device, PIRP irp, NTSTATUS status, ULONG bytes)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = bytes;
    if (device) {
        IoReleaseRemoveLock(&device->remove_lock, irp);
    }
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS NTAPI wgpu_pass_complete(PDEVICE_OBJECT object, PIRP irp, PVOID context)
{
    WgpuNtDevice *device = context;
    UNREFERENCED_PARAMETER(object);
    if (irp->PendingReturned) {
        IoMarkIrpPending(irp);
    }
    IoReleaseRemoveLock(&device->remove_lock, irp);
    return STATUS_SUCCESS;
}

static NTSTATUS wgpu_forward(WgpuNtDevice *device, PIRP irp, BOOLEAN power)
{
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, wgpu_pass_complete, device, TRUE, TRUE, TRUE);
    if (power) {
        PoStartNextPowerIrp(irp);
        return PoCallDriver(device->lower, irp);
    }
    return IoCallDriver(device->lower, irp);
}

static NTSTATUS NTAPI wgpu_start_complete(PDEVICE_OBJECT object, PIRP irp, PVOID context)
{
    UNREFERENCED_PARAMETER(object);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent(context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS NTAPI wgpu_dispatch(PDEVICE_OBJECT object, PIRP irp)
{
    WgpuNtDevice *device = object->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = IoAcquireRemoveLock(&device->remove_lock, irp);
    ULONG bytes = 0;
    if (!NT_SUCCESS(status)) {
        if (stack->MajorFunction == IRP_MJ_POWER) {
            PoStartNextPowerIrp(irp);
        }
        return wgpu_complete(NULL, irp, status, 0);
    }
    if (stack->MajorFunction == IRP_MJ_POWER) {
        return wgpu_forward(device, irp, TRUE);
    }
    if (stack->MajorFunction == IRP_MJ_PNP) {
        if (stack->MinorFunction == IRP_MN_START_DEVICE) {
            KEVENT event;
            PCM_RESOURCE_LIST resources = stack->Parameters.StartDevice.AllocatedResourcesTranslated;
            KeInitializeEvent(&event, NotificationEvent, FALSE);
            IoCopyCurrentIrpStackLocationToNext(irp);
            IoSetCompletionRoutine(irp, wgpu_start_complete, &event, TRUE, TRUE, TRUE);
            status = IoCallDriver(device->lower, irp);
            if (status == STATUS_PENDING) {
                KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
            }
            status = irp->IoStatus.Status;
            if (NT_SUCCESS(status)) {
                wgpu_lock(device);
                wgpu_unmap(device);
                status = wgpu_start(device, resources);
                wgpu_unlock(device);
            }
            return wgpu_complete(device, irp, status, 0);
        }
        if (stack->MinorFunction == IRP_MN_STOP_DEVICE ||
            stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL ||
            stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
            wgpu_lock(device);
#ifdef WGPU_NT_PIPELINE
            if (device->started && stack->MinorFunction == IRP_MN_STOP_DEVICE)
                wgpu_retire_pending(device);
#endif
            wgpu_unmap(device);
            wgpu_unlock(device);
            irp->IoStatus.Status = STATUS_SUCCESS;
        }
        if (stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
            UNICODE_STRING name;
            IoReleaseRemoveLockAndWait(&device->remove_lock, irp);
            irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(irp);
            status = IoCallDriver(device->lower, irp);
            RtlInitUnicodeString(&name, link_name);
            IoDeleteSymbolicLink(&name);
            IoDetachDevice(device->lower);
            IoDeleteDevice(object);
            return status;
        }
        if (stack->MinorFunction == IRP_MN_QUERY_STOP_DEVICE ||
            stack->MinorFunction == IRP_MN_QUERY_REMOVE_DEVICE ||
            stack->MinorFunction == IRP_MN_CANCEL_STOP_DEVICE ||
            stack->MinorFunction == IRP_MN_CANCEL_REMOVE_DEVICE) {
            irp->IoStatus.Status = STATUS_SUCCESS;
        }
        return wgpu_forward(device, irp, FALSE);
    }
    wgpu_lock(device);
    if (stack->MajorFunction == IRP_MJ_CLOSE || stack->MajorFunction == IRP_MJ_CLEANUP) {
        status = STATUS_SUCCESS;
    } else if (!device->started) {
        status = STATUS_DEVICE_NOT_READY;
    } else if (stack->MajorFunction == IRP_MJ_CREATE) {
        status = stack->FileObject->FileName.Length ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_SUCCESS;
    } else if (stack->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
        ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
        int image_reply=code==WGPU_NT_IOCTL_TRIANGLE || code==WGPU_NT_IOCTL_DRAW_TEST;
        ULONG expected=wgpu_surface_request_size(code);
        ULONG input_bytes=stack->Parameters.DeviceIoControl.InputBufferLength;
#ifdef WGPU_NT_QUEUED_GROUPS
        if (code == WGPU_NT_IOCTL_QUEUE_STATS) {
            status = STATUS_INVALID_PARAMETER;
            if (!input_bytes && stack->Parameters.DeviceIoControl.OutputBufferLength ==
                sizeof(WgpuNtQueueStats) && irp->AssociatedIrp.SystemBuffer) {
                RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &device->queue_stats,
                              sizeof(WgpuNtQueueStats));
                bytes = sizeof(WgpuNtQueueStats);
                status = STATUS_SUCCESS;
            }
            goto done;
        }
#endif
#ifdef WGPU_NT_BATCH_TEMPLATES
        if (code == WGPU_NT_IOCTL_TEMPLATE_STATS) {
            status = STATUS_INVALID_PARAMETER;
            if (!input_bytes && stack->Parameters.DeviceIoControl.OutputBufferLength ==
                sizeof(WgpuNtTemplateStats) && irp->AssociatedIrp.SystemBuffer) {
                RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &device->material_cache.template_stats,
                              sizeof(WgpuNtTemplateStats));
                bytes = sizeof(WgpuNtTemplateStats);
                status = STATUS_SUCCESS;
            }
            goto done;
        }
#endif
#ifdef WGPU_NT_PIPELINE
        if (code == WGPU_NT_IOCTL_PIPELINE_STATS) {
            status = STATUS_INVALID_PARAMETER;
            if (!input_bytes && stack->Parameters.DeviceIoControl.OutputBufferLength ==
                sizeof(WgpuNtPipelineStats) && irp->AssociatedIrp.SystemBuffer) {
                device->pipeline_stats.pending_fence = device->pending_fence;
                RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &device->pipeline_stats,
                              sizeof(WgpuNtPipelineStats));
                bytes = sizeof(WgpuNtPipelineStats);
                status = STATUS_SUCCESS;
            }
            goto done;
        }
        if ((code == WGPU_NT_IOCTL_QUEUE_MATERIAL || code == WGPU_NT_IOCTL_DRAIN_MATERIAL) &&
            irp->RequestorMode != KernelMode) {
            status = STATUS_ACCESS_DENIED;
            goto done;
        }
        if (code != WGPU_NT_IOCTL_QUEUE_MATERIAL && code != WGPU_NT_IOCTL_QUERY &&
            code != WGPU_NT_IOCTL_TIMING) {
            if (code == WGPU_NT_IOCTL_DRAIN_MATERIAL &&
                (input_bytes || stack->Parameters.DeviceIoControl.OutputBufferLength)) {
                status = STATUS_INVALID_PARAMETER;
                goto done;
            }
            status = wgpu_retire_pending(device);
            if (!NT_SUCCESS(status) || code == WGPU_NT_IOCTL_DRAIN_MATERIAL) goto done;
        }
#endif
        if (expected) {
            BOOLEAN valid_size = input_bytes == expected;
#ifdef WGPU_NT_ENABLE_GROUPS
            if (code == WGPU_NT_IOCTL_MATERIAL_GROUP
#ifdef WGPU_NT_PIPELINE
                || code == WGPU_NT_IOCTL_QUEUE_MATERIAL
#endif
                )
                valid_size = input_bytes >= expected;
#endif
#ifdef WGPU_NT_ENABLE_MATERIAL
            if (code == WGPU_NT_IOCTL_MATERIAL)
                valid_size = wgpu_nt_material_packet_valid(irp->AssociatedIrp.SystemBuffer, input_bytes);
#endif
            if (!valid_size ||
                stack->Parameters.DeviceIoControl.OutputBufferLength!=sizeof(WgpuNtFillResult) ||
                !irp->AssociatedIrp.SystemBuffer) status=STATUS_INVALID_PARAMETER;
            else {
                /* The buffered input stays intact until the builder has
                 * consumed it and the synchronous operation has finished. */
#ifdef WGPU_NT_TIMING
                ++device->timing.surface_requests;
                device->timing.input_bytes += input_bytes;
#endif
                status=wgpu_surface_op(device,code,irp->AssociatedIrp.SystemBuffer,
                                       input_bytes,irp->AssociatedIrp.SystemBuffer);
                if (NT_SUCCESS(status)) bytes=sizeof(WgpuNtFillResult);
            }
#ifdef WGPU_NT_TIMING
        } else if (code == WGPU_NT_IOCTL_TIMING) {
            if (input_bytes || stack->Parameters.DeviceIoControl.OutputBufferLength !=
                sizeof(WgpuNtTiming) || !irp->AssociatedIrp.SystemBuffer) {
                status = STATUS_INVALID_PARAMETER;
            } else {
#ifndef WGPU_NT_PROFILE_TSC
                LARGE_INTEGER frequency;
                KeQueryPerformanceCounter(&frequency);
                device->timing.frequency = frequency.QuadPart;
#endif
                device->timing.size = sizeof(WgpuNtTiming);
                device->timing.abi_version = WGPU_NT_ABI_VERSION;
                RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &device->timing,
                              sizeof(WgpuNtTiming));
                bytes = sizeof(WgpuNtTiming);
                status = STATUS_SUCCESS;
            }
#endif
        } else if (code != WGPU_NT_IOCTL_QUERY && code != WGPU_NT_IOCTL_SELFTEST &&
            !image_reply) {
            status = STATUS_INVALID_DEVICE_REQUEST;
        } else if (stack->Parameters.DeviceIoControl.InputBufferLength != 0) {
            status = STATUS_INVALID_PARAMETER;
        } else if (stack->Parameters.DeviceIoControl.OutputBufferLength <
                   (image_reply ? sizeof(WgpuNtTriangle) : sizeof(WgpuNtInfo)) ||
                   !irp->AssociatedIrp.SystemBuffer) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else if (image_reply) {
            status = wgpu_triangle(device, irp->AssociatedIrp.SystemBuffer,code==WGPU_NT_IOCTL_DRAW_TEST);
            if (NT_SUCCESS(status)) {
                bytes = sizeof(WgpuNtTriangle);
            }
        } else {
            status = code == WGPU_NT_IOCTL_SELFTEST ? wgpu_selftest(device) : STATUS_SUCCESS;
            wgpu_query(device, irp->AssociatedIrp.SystemBuffer);
            bytes = sizeof(WgpuNtInfo);
        }
    } else {
        status = STATUS_INVALID_DEVICE_REQUEST;
    }
#ifdef WGPU_NT_PIPELINE
done:
#endif
    wgpu_unlock(device);
    return wgpu_complete(device, irp, status, bytes);
}

static NTSTATUS NTAPI wgpu_add_device(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT object;
    WgpuNtDevice *device;
    UNICODE_STRING name, link;
    NTSTATUS status;
    RtlInitUnicodeString(&name, device_name);
    status = IoCreateDevice(driver, sizeof(WgpuNtDevice), &name, FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN, TRUE, &object);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    device = object->DeviceExtension; /* IoCreateDevice zeroes the extension. */
    KeInitializeMutex(&device->mutex, 0);
    IoInitializeRemoveLock(&device->remove_lock, WGPU_TAG, 0, 0);
    device->lower = IoAttachDeviceToDeviceStack(object, pdo);
    if (!device->lower) {
        IoDeleteDevice(object);
        return STATUS_NO_SUCH_DEVICE;
    }
    RtlInitUnicodeString(&link, link_name);
    status = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(status)) {
        IoDetachDevice(device->lower);
        IoDeleteDevice(object);
        return status;
    }
    object->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    object->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static VOID NTAPI wgpu_unload(PDRIVER_OBJECT driver)
{
    UNREFERENCED_PARAMETER(driver); /* PnP remove has already detached all devices. */
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    unsigned i;
    UNREFERENCED_PARAMETER(registry);
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i) {
        driver->MajorFunction[i] = wgpu_dispatch;
    }
    driver->DriverExtension->AddDevice = wgpu_add_device;
    driver->DriverUnload = wgpu_unload;
    return STATUS_SUCCESS;
}
