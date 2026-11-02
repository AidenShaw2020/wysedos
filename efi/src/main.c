// wyseDOS: UEFI entry point.
//
// Reports firmware, GOP, CPU/VMX capabilities and the memory map (screen and
// UDP log), then starts the virtual PC (platform/wyse.c).

#include "lib.h"
#include "net.h"

#define VERSION "0.1"

void wyse_main(void);
void exc_install(void);

static const EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static const EFI_GUID acpi20_guid = ACPI_20_TABLE_GUID;

static void report_firmware(void) {
    char vendor[64];
    int i;
    for (i = 0; i < 63 && gST->FirmwareVendor[i]; i++) vendor[i] = (char)gST->FirmwareVendor[i];
    vendor[i] = 0;
    printf("firmware: %s rev 0x%x, UEFI %u.%u\n", vendor, gST->FirmwareRevision,
           gST->Hdr.Revision >> 16, (gST->Hdr.Revision & 0xffff) / 10);

    for (uint64_t t = 0; t < gST->NumberOfTableEntries; t++)
        if (!memcmp(&gST->ConfigurationTable[t].VendorGuid, &acpi20_guid, sizeof(EFI_GUID)))
            printf("acpi 2.0 rsdp: %p\n", gST->ConfigurationTable[t].VendorTable);
}

static void report_gop(void) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    if (EFI_ERROR(gBS->LocateProtocol(&gop_guid, NULL, (void **)&gop))) {
        printf("gop: not found\n");
        return;
    }
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *cur = gop->Mode->Info;
    printf("gop: mode %u/%u %ux%u fmt %u stride %u, fb 0x%llx size 0x%llx\n", gop->Mode->Mode, gop->Mode->MaxMode,
           cur->HorizontalResolution, cur->VerticalResolution, cur->PixelFormat, cur->PixelsPerScanLine,
           gop->Mode->FrameBufferBase, gop->Mode->FrameBufferSize);
    char line[256];
    int n = snprintf(line, sizeof(line), "gop modes:");
    for (uint32_t m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        uint64_t size;
        if (EFI_ERROR(gop->QueryMode(gop, m, &size, &info))) continue;
        n += snprintf(line + n, sizeof(line) - n, " %u:%ux%u", m, info->HorizontalResolution, info->VerticalResolution);
        if (n > 180) {
            printf("%s\n", line);
            n = snprintf(line, sizeof(line), "          ");
        }
    }
    printf("%s\n", line);
}

static void report_cpu(void) {
    uint32_t r[4];
    char vendor[13], brand[49];

    cpuid(0, 0, r);
    memcpy(vendor, &r[1], 4); memcpy(vendor + 4, &r[3], 4); memcpy(vendor + 8, &r[2], 4);
    vendor[12] = 0;
    for (uint32_t i = 0; i < 3; i++) {
        cpuid(0x80000002 + i, 0, r);
        memcpy(brand + 16 * i, r, 16);
    }
    brand[48] = 0;
    const char *b = brand;
    while (*b == ' ') b++;
    cpuid(1, 0, r);
    bool vmx = r[2] & (1 << 5);
    printf("cpu: %s, %s, family/model/step %x, vmx %s\n", vendor, b, r[0], vmx ? "yes" : "NO");
    printf("cr0 0x%llx cr4 0x%llx\n", read_cr0(), read_cr4());
    if (!vmx) return;

    uint64_t fc = rdmsr(0x3a);
    uint64_t basic = rdmsr(0x480);
    uint64_t proc2 = rdmsr(0x48b);
    uint64_t ept = rdmsr(0x48c);
    uint64_t misc = rdmsr(0x485);
    printf("feature_control 0x%llx: lock %u, vmx outside smx %u\n", fc, (unsigned)(fc & 1), (unsigned)((fc >> 2) & 1));
    printf("vmx_basic 0x%llx: rev %u, vmcs %u bytes, true ctls %u, ins/outs info %u\n", basic,
           (unsigned)(basic & 0x7fffffff), (unsigned)((basic >> 32) & 0x1fff), (unsigned)((basic >> 55) & 1),
           (unsigned)((basic >> 54) & 1));
    uint32_t p2 = (uint32_t)(proc2 >> 32);
    printf("secondary ctls allowed 0x%x: ept %u, vpid %u, unrestricted guest %u\n", p2,
           (p2 >> 1) & 1, (p2 >> 5) & 1, (p2 >> 7) & 1);
    printf("ept/vpid caps 0x%llx: 4-level %u, wb %u, 2M %u, a/d %u\n", ept,
           (unsigned)((ept >> 6) & 1), (unsigned)((ept >> 14) & 1), (unsigned)((ept >> 16) & 1), (unsigned)((ept >> 21) & 1));
    printf("vmx_misc 0x%llx: preemption timer shift %u\n", misc, (unsigned)(misc & 0x1f));

    // TSC calibration against the firmware's Stall (1 s)
    uint64_t t0 = rdtsc();
    gBS->Stall(1000000);
    printf("tsc: %llu kHz\n", (rdtsc() - t0) / 1000);
}

static void report_memory(void) {
    uint64_t size = 0, key, desc_size;
    uint32_t desc_ver;
    gBS->GetMemoryMap(&size, NULL, &key, &desc_size, &desc_ver);
    size += 4 * desc_size;
    uint8_t *map;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, size, (void **)&map))) return;
    if (EFI_ERROR(gBS->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)map, &key, &desc_size, &desc_ver))) {
        gBS->FreePool(map);
        return;
    }
    uint64_t free_pages = 0, largest = 0, largest_at = 0, below1m = 0, top = 0;
    for (uint64_t off = 0; off < size; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(map + off);
        uint64_t end = d->PhysicalStart + d->NumberOfPages * 4096;
        if (d->Type != EfiMemoryMappedIO && d->Type != EfiMemoryMappedIOPortSpace && end > top) top = end;
        if (d->Type != EfiConventionalMemory) continue;
        free_pages += d->NumberOfPages;
        if (d->PhysicalStart < 0x100000) below1m += (end > 0x100000 ? 0x100000 : end) - d->PhysicalStart;
        if (d->NumberOfPages > largest) {
            largest = d->NumberOfPages;
            largest_at = d->PhysicalStart;
        }
    }
    printf("memory: %llu entries, free %llu MB, largest free %llu MB at 0x%llx, free below 1M %llu KB, top 0x%llx\n",
           size / desc_size, free_pages / 256, largest / 256, largest_at, below1m / 1024, top);
    gBS->FreePool(map);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    gST = st;
    gBS = st->BootServices;
    gImage = image;

    // a hang anywhere before the main loop takes over resets the machine (and netboots again)
    gBS->SetWatchdogTimer(60, 0x10000, 0, NULL);
    gST->ConOut->ClearScreen(gST->ConOut);
    gST->ConOut->EnableCursor(gST->ConOut, 0);
    net_init();
    {
        static const EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
        EFI_LOADED_IMAGE_PROTOCOL *li;
        if (!EFI_ERROR(gBS->HandleProtocol(image, &li_guid, (void **)&li))) gImageBase = li->ImageBase;
    }
    exc_install();

    printf("== wyseDOS " VERSION " ==\n");
    if (net_active()) {
        const uint8_t *ip = net_my_ip(), *srv = net_server_ip();
        printf("net: %u.%u.%u.%u, server %u.%u.%u.%u, log udp :5555\n", ip[0], ip[1], ip[2], ip[3],
               srv[0], srv[1], srv[2], srv[3]);
    } else {
        printf("net: no network\n");
    }
    report_firmware();
    report_gop();
    report_cpu();
    report_memory();

    wyse_main();
    return EFI_SUCCESS;
}
