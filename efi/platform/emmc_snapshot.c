/* Whole physical eMMC backup, including both GPTs, ESP and guest container.
 * This mode runs before pc_new: no guest can write during either pass.
 * All physical I/O is bio_read; output is restricted to the netfs server.
 * Existing images are never overwritten and failures retain .partial. */
#include <stdio.h>
#include <string.h>
#include "emmc_snapshot.h"
#include "bio.h"
#include "ff.h"
#include "installer_upgrade.h"

extern EFI_BOOT_SERVICES *gBS;
static const EFI_GUID snapshot_bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
#define SNAPSHOT_MARKER "emmc-backup.ini"
#define SNAPSHOT_PARTIAL "emmc-full.img.partial"
#define SNAPSHOT_FINAL "emmc-full.img"
#define SNAPSHOT_CHUNK (1u << 20)
static uint8_t snapshot_source[SNAPSHOT_CHUNK] __attribute__((aligned(4096)));
static uint8_t snapshot_saved[SNAPSHOT_CHUNK] __attribute__((aligned(4096)));

bool emmc_snapshot_requested(void) {
    FILINFO info;
    return fs_default_backend() == FS_NETFS && f_stat(SNAPSHOT_MARKER, &info) == FR_OK;
}

static bool snapshot_media(EFI_BLOCK_IO_PROTOCOL *bio, uint32_t id, uint64_t blocks) {
    EFI_BLOCK_IO_MEDIA *m = bio->Media;
    return m && m->MediaPresent && m->MediaId == id && m->BlockSize == 512 &&
           m->LastBlock == blocks - 1;
}

static bool snapshot_copy(EFI_BLOCK_IO_PROTOCOL *bio) {
    uint64_t blocks = bio->Media->LastBlock + 1;
    uint32_t id = bio->Media->MediaId;
    if (!blocks || blocks > UINT64_MAX / 512) return false;
    FIL image;
    if (f_open(&image, SNAPSHOT_PARTIAL, FA_WRITE | FA_CREATE_NEW) != FR_OK) return false;
    bool ok = true;
    unsigned last_percent = 101;
    for (uint64_t at = 0; ok && at < blocks;) {
        unsigned count = blocks - at < SNAPSHOT_CHUNK / 512 ?
                         (unsigned)(blocks - at) : SNAPSHOT_CHUNK / 512;
        UINT put = 0;
        ok = snapshot_media(bio, id, blocks) &&
             !EFI_ERROR(bio_read(bio, at, count, snapshot_source)) &&
             snapshot_media(bio, id, blocks) &&
             f_write(&image, snapshot_source, count * 512u, &put) == FR_OK && put == count * 512u;
        if (!ok) break;
        at += count;
        unsigned percent = (unsigned)(at * 100 / blocks);
        if (percent != last_percent) {
            printf("eMMC snapshot: copied %u%%\n", percent);
            last_percent = percent;
        }
    }
    if (ok && f_sync(&image) != FR_OK) ok = false;
    if (f_close(&image) != FR_OK) ok = false;
    if (!ok) return false;
    if (f_open(&image, SNAPSHOT_PARTIAL, FA_READ) != FR_OK) return false;
    ok = f_size(&image) == blocks * 512;
    last_percent = 101;
    for (uint64_t at = 0; ok && at < blocks;) {
        unsigned count = blocks - at < SNAPSHOT_CHUNK / 512 ?
                         (unsigned)(blocks - at) : SNAPSHOT_CHUNK / 512;
        UINT got = 0;
        ok = snapshot_media(bio, id, blocks) &&
             !EFI_ERROR(bio_read(bio, at, count, snapshot_source)) &&
             snapshot_media(bio, id, blocks) &&
             f_read(&image, snapshot_saved, count * 512u, &got) == FR_OK && got == count * 512u &&
             !memcmp(snapshot_source, snapshot_saved, got);
        if (!ok) break;
        at += count;
        unsigned percent = (unsigned)(at * 100 / blocks);
        if (percent != last_percent) {
            printf("eMMC snapshot: verified %u%%\n", percent);
            last_percent = percent;
        }
    }
    if (f_close(&image) != FR_OK) ok = false;
    return ok && f_rename(SNAPSHOT_PARTIAL, SNAPSHOT_FINAL) == FR_OK;
}

bool emmc_snapshot_run(void) {
    if (fs_default_backend() != FS_NETFS) return false;
    FIL marker;
    static const char consent[] = "wyseDOS full eMMC backup v1\n";
    char text[sizeof(consent)] = {0};
    UINT got = 0;
    if (f_open(&marker, SNAPSHOT_MARKER, FA_READ) != FR_OK) return false;
    bool valid = f_size(&marker) == sizeof(consent) - 1 &&
                 f_read(&marker, text, sizeof(text), &got) == FR_OK &&
                 got == sizeof(consent) - 1 && !memcmp(text, consent, got);
    if (f_close(&marker) != FR_OK) valid = false;
    if (!valid) return false;
    FILINFO exists;
    if (f_stat(SNAPSHOT_FINAL, &exists) != FR_NO_FILE ||
        f_stat(SNAPSHOT_PARTIAL, &exists) != FR_NO_FILE) {
        printf("eMMC snapshot: refusing existing output\n");
        return false;
    }
    uint64_t n;
    EFI_HANDLE *handles;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &snapshot_bio_guid, NULL, &n, &handles))) return false;
    EFI_BLOCK_IO_PROTOCOL *selected = NULL;
    unsigned matches = 0;
    for (uint64_t i = 0; i < n; ++i) {
        EFI_BLOCK_IO_PROTOCOL *bio;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &snapshot_bio_guid, (void **)&bio))) continue;
        EFI_BLOCK_IO_MEDIA *m = bio->Media;
        EFI_DEVICE_PATH *path = bio_device_path(handles[i]);
        if (!m || m->LogicalPartition || m->RemovableMedia || !m->MediaPresent || m->BlockSize != 512 ||
            bio_path_has(path, DP_MESSAGING, DP_MSG_USB)) continue;
        /* This firmware uses a PCI/controller path for eMMC, without an
         * eMMC messaging node. Qualify its complete existing GPT instead:
         * primary header/table CRCs, ESP and private guest type, no overlaps.
         * The eMMC boot areas expose separate 4-MB handles and fail this. */
        WyseInstallLayout layout;
        if (!installer_read_layout(bio, &layout)) continue;
        selected = bio;
        ++matches;
    }
    gBS->FreePool(handles);
    if (matches != 1) {
        printf("eMMC snapshot: expected one physical eMMC, found %u\n", matches);
        return false;
    }
    /* Netfs retry windows can exceed 30 s. No guest/helper cores exist yet;
     * this dedicated mode halts after success/failure, never boots a guest. */
    if (EFI_ERROR(gBS->SetWatchdogTimer(0, 0, 0, NULL))) return false;
    printf("eMMC snapshot: %llu physical sectors; device remains read-only\n",
           (unsigned long long)(selected->Media->LastBlock + 1));
    bool ok = snapshot_copy(selected);
    printf("eMMC snapshot: %s\n", ok ? "COMPLETE, source/readback identical; compute host SHA256" :
           "FAILED, do not erase eMMC; preserve partial output");
    return ok;
}
