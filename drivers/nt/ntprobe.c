/* SPDX-License-Identifier: GPL-3.0-or-later
 * Minimal NT5 console client. No installer, service manipulation or raw MMIO.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "wgpu_nt.h"

static void text(const char *message)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, lstrlenA(message), &written, NULL);
}

static void print_hex(const char *name, unsigned long long value, unsigned digits_count)
{
    char output[19];
    unsigned i;
    static const char digits[] = "0123456789abcdef";
    for (i = 0; i < digits_count; ++i) {
        output[i] = digits[(value >> ((digits_count - 1 - i) * 4)) & 15];
    }
    output[i++] = '\r';
    output[i++] = '\n';
    output[i] = 0;
    text(name);
    text(output);
}

static void hex(const char *name, DWORD value)
{
    print_hex(name, value, 8);
}

static BOOL show_timing(HANDLE handle)
{
    WgpuNtTiming timing;
    DWORD returned;
    if (!DeviceIoControl(handle, WGPU_NT_IOCTL_TIMING, NULL, 0,
                         &timing, sizeof(timing), &returned, NULL) ||
        returned != sizeof(timing) || timing.size != sizeof(timing) ||
        timing.abi_version != WGPU_NT_ABI_VERSION || timing.frequency < 0) {
        text("WDM timing unavailable\r\n");
        return FALSE;
    }
    hex("Surface requests: ", timing.surface_requests);
    hex("Scheduler sleeps: ", timing.scheduler_sleeps);
    if (timing.frequency) print_hex("Counter frequency: ", timing.frequency, 16);
    else text("Raw TSC ticks\r\n");
    print_hex("Prepare ticks: ", timing.prepare_ticks, 16);
    print_hex("Build ticks: ", timing.build_ticks, 16);
    print_hex("Doorbell ticks: ", timing.doorbell_ticks, 16);
    print_hex("Fence ticks: ", timing.fence_ticks, 16);
    print_hex("Input bytes: ", timing.input_bytes, 16);
#ifdef WGPU_NT_PIPELINE
    {
        WgpuNtPipelineStats pipeline;
        if (DeviceIoControl(handle, WGPU_NT_IOCTL_PIPELINE_STATS, NULL, 0,
                            &pipeline, sizeof(pipeline), &returned, NULL) &&
            returned == sizeof(pipeline)) {
            hex("Queued groups: ", pipeline.queued);
            hex("Built with a pending fence: ", pipeline.prepared_while_pending);
            hex("Retired groups: ", pipeline.retired);
            hex("Pending fence: ", pipeline.pending_fence);
        }
    }
#endif
#ifdef WGPU_NT_BATCH_TEMPLATES
    {
        WgpuNtTemplateStats templates;
        if (DeviceIoControl(handle, WGPU_NT_IOCTL_TEMPLATE_STATS, NULL, 0,
                            &templates, sizeof(templates), &returned, NULL) &&
            returned == sizeof(templates)) {
            hex("Command templates sampled: ", templates.lookups);
            hex("Command template hits: ", templates.hits);
        }
    }
#endif
#ifdef WGPU_NT_QUEUED_GROUPS
    {
        WgpuNtQueueStats queue;
        if (DeviceIoControl(handle, WGPU_NT_IOCTL_QUEUE_STATS, NULL, 0,
                            &queue, sizeof(queue), &returned, NULL) &&
            returned == sizeof(queue)) {
            hex("Oldest slot retirement attempts: ", queue.slot_retirements);
            hex("Maximum owned slots: ", queue.max_pending);
        }
    }
#endif
    return TRUE;
}

static BOOL write_all(HANDLE file, const void *data, DWORD bytes)
{
    DWORD written;
    return WriteFile(file, data, bytes, &written, NULL) && written == bytes;
}

static void save_triangle(const WgpuNtTriangle *image)
{
    /* Fixed RGB565 BMP, bottom-up rows. CREATE_NEW preserves an earlier result. */
    static const BYTE header[66] = {
        0x42,0x4d,0x42,0x80,0,0,0,0,0,0,66,0,0,0,
        40,0,0,0,128,0,0,0,128,0,0,0,1,0,16,0,
        3,0,0,0,0,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0xf8,0,0,0xe0,7,0,0,0x1f,0,0,0
    };
    HANDLE file = CreateFileA("C:\\NTGPU.BMP", GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    BOOL ok;
    unsigned row;
    if (file == INVALID_HANDLE_VALUE) {
        hex("Image not saved; existing files are preserved. Win32 error=", GetLastError());
        return;
    }
    ok = write_all(file, header, sizeof(header));
    for (row = WGPU_NT_TRI_HEIGHT; ok && row; --row) {
        ok = write_all(file, image->pixels + (row - 1) * WGPU_NT_TRI_WIDTH,
                       WGPU_NT_TRI_WIDTH * 2);
    }
    if (ok) { ok = FlushFileBuffers(file); }
    if (!CloseHandle(file)) { ok = FALSE; }
    if (ok) {
        text("GPU image saved as C:\\NTGPU.BMP\r\n");
    } else {
        DeleteFileA("C:\\NTGPU.BMP"); /* Only the partial file created by this call. */
        text("Image write failed; partial output removed\r\n");
    }
}

void mainCRTStartup(void)
{
    char *command = GetCommandLineA();
    BOOL selftest, triangle, timing;
    HANDLE handle;
    DWORD returned, error;
    WgpuNtInfo info;
    WgpuNtInfo before;
    static WgpuNtTriangle rendered;
    if (*command == '"') {
        ++command;
        while (*command && *command != '"') {
            ++command;
        }
        if (*command) {
            ++command;
        }
    } else {
        while (*command && *command != ' ' && *command != '\t') {
            ++command;
        }
    }
    while (*command == ' ' || *command == '\t') {
        ++command;
    }
    selftest = lstrcmpA(command, "--selftest") == 0;
    triangle = lstrcmpA(command, "--triangle") == 0;
    timing = lstrcmpA(command, "--timing") == 0;
    if (*command && !selftest && !triangle && !timing) {
        text("Usage: ntprobe [--selftest|--triangle|--timing]\r\n");
        ExitProcess(2);
    }
    handle = CreateFileA("\\\\.\\WyseWgpuNT", GENERIC_READ | (selftest || triangle ? GENERIC_WRITE : 0),
                          0, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        hex("Open failed, Win32 error=", GetLastError());
        ExitProcess(1);
    }
    if (timing) {
        BOOL ok = show_timing(handle);
        CloseHandle(handle);
        ExitProcess(ok ? 0 : 1);
    }
    error = 0;
    if (triangle) {
        if (!DeviceIoControl(handle, WGPU_NT_IOCTL_QUERY, NULL, 0,
                             &before, sizeof(before), &returned, NULL) ||
            returned != sizeof(before) || before.size != sizeof(before) ||
            before.abi_version != WGPU_NT_ABI_VERSION) {
            text("Initial query failed\r\n"); CloseHandle(handle); ExitProcess(1);
        }
        if (!DeviceIoControl(handle, WGPU_NT_IOCTL_TRIANGLE, NULL, 0,
                             &rendered, sizeof(rendered), &returned, NULL)) {
            hex("Triangle IOCTL failed, Win32 error=", GetLastError());
            CloseHandle(handle); ExitProcess(1);
        }
        if (returned != sizeof(rendered) || !wgpu_nt_check_triangle(&rendered)) {
            text("FAIL: GPU triangle pixels or output ABI\r\n");
            CloseHandle(handle); ExitProcess(1);
        }
    }
    if (selftest && !DeviceIoControl(handle, WGPU_NT_IOCTL_SELFTEST, NULL, 0,
                                     &info, sizeof(info), &returned, NULL)) {
        error = GetLastError();
        hex("Self-test IOCTL failed, Win32 error=", error);
    }
    if (!DeviceIoControl(handle, WGPU_NT_IOCTL_QUERY, NULL, 0,
                          &info, sizeof(info), &returned, NULL) ||
        returned != sizeof(info) || info.size != sizeof(info) ||
        info.abi_version != WGPU_NT_ABI_VERSION) {
        text("Query failed or incompatible bridge ABI\r\n");
        CloseHandle(handle);
        ExitProcess(1);
    }
    hex("magic=", info.magic);
    hex("protocol=", info.protocol);
    hex("gpu_id=", info.gpu_id);
    hex("status=", info.status);
    hex("aperture_bytes=", info.aperture_bytes);
    hex("aperture_gfx=", info.aperture_gfx);
    hex("submits=", info.submits);
    hex("submitted=", info.submitted);
    hex("completed=", info.completed);
    hex("resets=", info.resets);
    hex("user_3d_submits=", info.user_3d_submits);
    if (selftest) {
        hex("smoke_result=", info.smoke_result);
        hex("smoke_expected=", info.smoke_expected);
        hex("smoke_value=", info.smoke_value);
        hex("smoke_fence=", info.smoke_fence);
        if (error || info.smoke_result != 1 || info.smoke_value != info.smoke_expected ||
            info.completed != info.smoke_fence) {
            text("FAIL: GPU self-test\r\n");
            CloseHandle(handle);
            ExitProcess(1);
        }
        text("PASS: GPU store and completion fence\r\n");
    } else if (triangle) {
        if (info.completed != rendered.fence || info.submitted != rendered.fence ||
            info.submits != before.submits + 1 || info.resets != before.resets ||
            (info.status & 2) ||
            info.user_3d_submits != before.user_3d_submits + rendered.flags) {
            text("FAIL: GPU rendering completion or reset counters\r\n");
            CloseHandle(handle); ExitProcess(1);
        }
        text("PASS: hardware Gouraud triangle, RGB565 pixel comparison and completion fence\r\n");
        save_triangle(&rendered);
    } else {
        text("Query only; no rendering test performed\r\n");
    }
    CloseHandle(handle);
    ExitProcess(0);
}
