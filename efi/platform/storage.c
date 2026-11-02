// Where the machine's files come from.
//
// Netbooted, they come from the development PC over netfs. Started from a
// disk - the eMMC after installation, or a USB stick - they come from the
// FatFs volume on that disk that holds 386/config.ini: the partition the
// monitor itself was loaded from, or another one on the same disk.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ff.h"
#include "fatvol.h"
#include "bio.h"
#include "storage.h"

extern EFI_BOOT_SERVICES *gBS;
extern EFI_HANDLE gImage;

static const EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static const EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

static EFI_HANDLE boot_handle;
static char source[64] = "netfs";

const char *storage_source(void) { return source; }
EFI_HANDLE storage_boot_handle(void) { return boot_handle; }

// Whether two partition paths are on the same disk: equal up to their hard drive nodes
static bool same_disk(EFI_DEVICE_PATH *a, EFI_DEVICE_PATH *b) {
    if (!a || !b) return false;
    for (;;) {
        if (dp_end(a) || dp_end(b) || a->Length < 4) return false;
        bool ha = a->Type == DP_MEDIA && a->SubType == DP_MEDIA_HARDDRIVE;
        bool hb = b->Type == DP_MEDIA && b->SubType == DP_MEDIA_HARDDRIVE;
        if (ha || hb) return ha && hb;
        if (a->Length != b->Length || memcmp(a, b, a->Length)) return false;
        a = dp_next(a);
        b = dp_next(b);
    }
}

static bool try_data(EFI_HANDLE h, EFI_BLOCK_IO_PROTOCOL *bio) {
    if (!bio->Media->MediaPresent || bio->Media->BlockSize != 512) return false;
    if (fatvol_attach(FATVOL_DATA, bio, 0, bio->Media->LastBlock + 1) != FR_OK) return false;
    fatvol_info i;
    if (fatvol_stat(FATVOL_DATA, "386/config.ini", &i) == FR_OK) return true;
    fatvol_detach(FATVOL_DATA);
    return false;
}

void storage_list(void) {
    uint64_t n;
    EFI_HANDLE *hs;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &hs))) return;
    for (uint64_t i = 0; i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &bio_guid, (void **)&bio))) continue;
        char path[160];
        bio_path_str(bio_device_path(hs[i]), path, sizeof(path));
        EFI_BLOCK_IO_MEDIA *m = bio->Media;
        printf("blockio %llu: %s, %s%s%s%s, %u x %llu (%llu MB), align %u\n", (unsigned long long)i, path,
               m->LogicalPartition ? "partition" : "device", m->RemovableMedia ? ", removable" : "",
               m->MediaPresent ? "" : ", no medium", m->ReadOnly ? ", read-only" : "", m->BlockSize,
               (unsigned long long)(m->LastBlock + 1),
               (unsigned long long)((m->LastBlock + 1) * m->BlockSize >> 20), m->IoAlign);
    }
    gBS->FreePool(hs);
}

bool storage_init(void) {
    EFI_LOADED_IMAGE_PROTOCOL *li;
    if (EFI_ERROR(gBS->HandleProtocol(gImage, &li_guid, (void **)&li))) return false;
    EFI_BLOCK_IO_PROTOCOL *boot_bio;
    if (EFI_ERROR(gBS->HandleProtocol(li->DeviceHandle, &bio_guid, (void **)&boot_bio))) {
        fs_set_default(FS_NETFS, 0);
        printf("storage: netbooted, files over netfs\n");
        return true;
    }
    boot_handle = li->DeviceHandle;
    EFI_DEVICE_PATH *boot_path = bio_device_path(boot_handle);
    char path[160];
    bio_path_str(boot_path, path, sizeof(path));
    // the boot partition itself, then the other partitions of its disk
    if (try_data(boot_handle, boot_bio)) {
        snprintf(source, sizeof(source), "%s", path);
        fs_set_default(FS_FAT, FATVOL_DATA);
        printf("storage: data on the boot partition %s\n", path);
        return true;
    }
    uint64_t n;
    EFI_HANDLE *hs;
    if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &hs))) {
        for (uint64_t i = 0; i < n; i++) {
            EFI_BLOCK_IO_PROTOCOL *bio;
            if (hs[i] == boot_handle || EFI_ERROR(gBS->HandleProtocol(hs[i], &bio_guid, (void **)&bio))) continue;
            if (!bio->Media->LogicalPartition || !same_disk(bio_device_path(hs[i]), boot_path)) continue;
            if (!try_data(hs[i], bio)) continue;
            bio_path_str(bio_device_path(hs[i]), source, sizeof(source));
            fs_set_default(FS_FAT, FATVOL_DATA);
            printf("storage: booted from %s, data on %s\n", path, source);
            gBS->FreePool(hs);
            return true;
        }
        gBS->FreePool(hs);
    }
    printf("storage: booted from %s, no 386/config.ini on its disk\n", path);
    return false;
}

//--------------------------------------------------------------------+
// "fs N [path]": mount Block I/O handle N as volume 3 and list a directory;
// "fswr N": also write, read back and delete a test file there
//--------------------------------------------------------------------+

static uint64_t usec(void) {
    uint64_t wyse_ticks_us(void);
    return wyse_ticks_us();
}

// "bench N": raw read speed of Block I/O handle N for several transfer sizes
void storage_bench(const char *args) {
    unsigned long idx = strtoul(args, NULL, 10);
    uint64_t n;
    EFI_HANDLE *hs;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &hs))) return;
    EFI_BLOCK_IO_PROTOCOL *bio = NULL;
    if (idx < n) gBS->HandleProtocol(hs[idx], &bio_guid, (void **)&bio);
    gBS->FreePool(hs);
    if (!bio) return;
    static uint8_t buf[1 << 20] __attribute__((aligned(4096)));
    static const unsigned sizes[] = {1, 8, 64, 256, 2048};
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        unsigned blocks = sizes[s], reps = blocks >= 256 ? 16 : 64;
        uint64_t t0 = usec();
        EFI_STATUS st = 0;
        for (unsigned k = 0; k < reps && !EFI_ERROR(st); k++)
            st = bio->ReadBlocks(bio, bio->Media->MediaId, 4096 + (uint64_t)k * blocks, blocks * 512ull, buf);
        uint64_t us = usec() - t0;
        printf("bench: %u x %u blocks: %llu us per call, %llu KB/s, status %llx\n", reps, blocks,
               (unsigned long long)(us / reps), (unsigned long long)(reps * blocks * 512ull * 1000 / (us + 1)),
               (unsigned long long)st);
    }
}

// "nfbench": read speed of 16 MB of 386/hdd.img through the default backend
void storage_file_bench(void) {
    static uint8_t buf[1 << 20];
    FIL f;
    if (f_open(&f, "386/hdd.img", FA_READ) != FR_OK) return;
    uint64_t t0 = usec();
    UINT got = 0;
    for (int k = 0; k < 16; k++) f_read(&f, buf, sizeof(buf), &got);
    uint64_t us = usec() - t0;
    f_close(&f);
    printf("nfbench: 16 MB in %llu ms, %llu KB/s\n", (unsigned long long)(us / 1000),
           (unsigned long long)(16ull * 1024 * 1000000 / (us + 1)));
}

void storage_debug(const char *args, bool write_test) {
    char *end;
    unsigned long idx = strtoul(args, &end, 10);
    while (*end == ' ') end++;
    uint64_t n;
    EFI_HANDLE *hs;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &hs))) return;
    EFI_BLOCK_IO_PROTOCOL *bio = NULL;
    if (idx < n) gBS->HandleProtocol(hs[idx], &bio_guid, (void **)&bio);
    gBS->FreePool(hs);
    if (!bio) { printf("fs: no handle %lu\n", idx); return; }
    int r = fatvol_attach(3, bio, 0, bio->Media->LastBlock + 1);
    printf("fs: handle %lu mount %d, free %llu MB\n", idx, r, (unsigned long long)(fatvol_free_bytes(3) >> 20));
    if (r) return;
    static uint8_t dirbuf[FATVOL_DIR_BYTES] __attribute__((aligned(8)));
    fatvol_info i;
    if (fatvol_opendir(dirbuf, 3, end) == FR_OK) {
        while (fatvol_readdir(dirbuf, &i) == FR_OK && i.name[0])
            printf("fs:   %-24s %10llu %s\n", i.name, (unsigned long long)i.size, i.attr & 0x10 ? "<DIR>" : "");
        fatvol_closedir(dirbuf);
    }
    if (write_test) {
        static uint8_t fil[FATVOL_FIL_BYTES] __attribute__((aligned(8)));
        static uint8_t buf[256 * 1024];
        for (unsigned k = 0; k < sizeof(buf); k++) buf[k] = (uint8_t)(k * 7 + k / 256);
        unsigned done;
        uint64_t t0 = usec();
        r = fatvol_open(fil, 3, "wysetest.bin", FA_CREATE_ALWAYS | FA_WRITE);
        for (int k = 0; !r && k < 16; k++) r = fatvol_write(fil, buf, sizeof(buf), &done);
        if (!r) r = fatvol_close(fil);
        uint64_t t1 = usec();
        printf("fs: wrote 4 MB: %d, %llu ms\n", r, (unsigned long long)((t1 - t0) / 1000));
        static uint8_t back[256 * 1024];
        int bad = 0;
        r = fatvol_open(fil, 3, "wysetest.bin", FA_READ);
        for (int k = 0; !r && k < 16; k++) {
            r = fatvol_read(fil, back, sizeof(back), &done);
            if (memcmp(back, buf, sizeof(buf)) || done != sizeof(buf)) bad++;
        }
        fatvol_close(fil);
        printf("fs: read back: %d, %d bad blocks, %llu ms\n", r, bad, (unsigned long long)((usec() - t1) / 1000));
        printf("fs: unlink %d\n", fatvol_unlink(3, "wysetest.bin"));
    }
    fatvol_detach(3);
}
