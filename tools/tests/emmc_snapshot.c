/* clang -O2 tools/tests/emmc_snapshot.c -o work/emmc_snapshot_test.exe
 * Exercise production whole-disk backup without physical-device writes. */
#include "../../efi/platform/emmc_snapshot.c"
#include <stdlib.h>
#define BYTES (SNAPSHOT_CHUNK + 3 * 512)
static unsigned char disk_bytes[BYTES], image_bytes[BYTES];
static EFI_BLOCK_IO_MEDIA media;
static EFI_BLOCK_IO_PROTOCOL device;
static EFI_BOOT_SERVICES services;
EFI_BOOT_SERVICES *gBS = &services;
static unsigned pos, length, reads, renames, opens;
static int short_write, corrupt_readback, change_media, fail_close, existing;
static int backend = FS_NETFS;
static int watchdog, marker_bad, candidates = 1, bad_layout;
static EFI_HANDLE handles[2] = {(void *)1, (void *)2};
int fs_default_backend(void) { return backend; }
FRESULT f_stat(const char *path, FILINFO *info) {
    (void)info;
    return !strcmp(path, SNAPSHOT_MARKER) || existing ? FR_OK : FR_NO_FILE;
}
FRESULT f_open(FIL *file, const char *path, BYTE mode) {
    memset(file, 0, sizeof(*file));
    if (!strcmp(path, SNAPSHOT_MARKER)) {
        file->flag = 99; file->obj.objsize = strlen("wyseDOS full eMMC backup v1\n");
        return FR_OK;
    }
    ++opens;
    if ((mode & FA_CREATE_NEW) && existing) return FR_EXIST;
    pos = 0;
    file->obj.objsize = length;
    return FR_OK;
}
FRESULT f_write(FIL *file, const void *data, UINT count, UINT *put) {
    (void)file;
    if (short_write) --count;
    if (pos + count > BYTES) abort();
    memcpy(image_bytes + pos, data, count); pos += count; length = pos; *put = count;
    return FR_OK;
}
FRESULT f_read(FIL *file, void *data, UINT count, UINT *got) {
    if (file->flag == 99) {
        const char *marker = "wyseDOS full eMMC backup v1\n";
        *got = (UINT)strlen(marker); memcpy(data, marker, *got);
        if (marker_bad) ((char *)data)[0] ^= 1;
        return FR_OK;
    }
    if (pos + count > length) count = length - pos;
    memcpy(data, image_bytes + pos, count); pos += count; *got = count;
    if (corrupt_readback && count) ((unsigned char *)data)[count - 1] ^= 1;
    return FR_OK;
}
FRESULT f_sync(FIL *file) { (void)file; return FR_OK; }
FRESULT f_close(FIL *file) { (void)file; return fail_close ? FR_DISK_ERR : FR_OK; }
FRESULT f_rename(const char *from, const char *to) {
    if (strcmp(from, SNAPSHOT_PARTIAL) || strcmp(to, SNAPSHOT_FINAL)) abort();
    ++renames; return FR_OK;
}
EFI_STATUS bio_read(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint64_t blocks, void *out) {
    if (bio != &device || (lba + blocks) * 512 > BYTES) abort();
    ++reads; memcpy(out, disk_bytes + lba * 512, (size_t)blocks * 512);
    if (change_media) ++media.MediaId;
    return EFI_SUCCESS;
}
EFI_DEVICE_PATH *bio_device_path(EFI_HANDLE handle) { return (EFI_DEVICE_PATH *)handle; }
bool bio_path_has(EFI_DEVICE_PATH *path, uint8_t type, uint8_t subtype) {
    (void)path; return type == DP_MESSAGING && subtype == DP_MSG_EMMC;
}
bool installer_read_layout(EFI_BLOCK_IO_PROTOCOL *bio, WyseInstallLayout *layout) {
    (void)bio; (void)layout; return !bad_layout;
}
static EFI_STATUS locate(uint32_t search, const EFI_GUID *guid, void *key, uint64_t *n, EFI_HANDLE **out) {
    (void)search; (void)guid; (void)key; *n = candidates; *out = handles; return EFI_SUCCESS;
}
static EFI_STATUS protocol(EFI_HANDLE h, const EFI_GUID *guid, void **out) {
    (void)h; (void)guid; *out = &device; return EFI_SUCCESS;
}
static EFI_STATUS release(void *p) { (void)p; return EFI_SUCCESS; }
static EFI_STATUS timer(uint64_t seconds, uint64_t code, uint64_t size, CHAR16 *text) {
    (void)code; (void)size; (void)text; if (seconds) abort(); ++watchdog; return EFI_SUCCESS;
}
static void reset(void) {
    memset(&media, 0, sizeof(media)); media.MediaPresent = true;
    media.MediaId = 17; media.BlockSize = 512; media.LastBlock = BYTES / 512 - 1;
    device.Media = &media;
    pos = length = reads = renames = opens = 0;
    short_write = corrupt_readback = change_media = fail_close = existing = marker_bad = watchdog = 0;
    backend = FS_NETFS; candidates = 1; bad_layout = 0;
}
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n", __LINE__); return 1; } } while (0)
int main(void) {
    services.LocateHandleBuffer = locate; services.HandleProtocol = protocol;
    services.FreePool = release; services.SetWatchdogTimer = timer;
    for (unsigned i = 0; i < BYTES; ++i) disk_bytes[i] = (unsigned char)(i ^ (i >> 9));
    reset(); CHECK(emmc_snapshot_requested()); CHECK(emmc_snapshot_run());
    CHECK(length == BYTES && reads == 4 && renames == 1 && watchdog == 1);
    CHECK(!memcmp(disk_bytes, image_bytes, BYTES));
    reset(); short_write = 1; CHECK(!emmc_snapshot_run() && renames == 0);
    reset(); corrupt_readback = 1; CHECK(!emmc_snapshot_run() && renames == 0);
    reset(); change_media = 1; CHECK(!emmc_snapshot_run() && renames == 0);
    reset(); fail_close = 1; CHECK(!emmc_snapshot_run() && renames == 0);
    reset(); existing = 1; CHECK(!emmc_snapshot_run() && opens == 0 && reads == 0);
    reset(); backend = FS_FAT; CHECK(!emmc_snapshot_requested()); CHECK(!emmc_snapshot_run() && reads == 0);
    reset(); candidates = 2; CHECK(!emmc_snapshot_run() && opens == 0 && reads == 0);
    reset(); marker_bad = 1; CHECK(!emmc_snapshot_run() && opens == 0 && reads == 0);
    reset(); bad_layout = 1; CHECK(!emmc_snapshot_run() && opens == 0 && reads == 0);
    reset(); media.LastBlock = UINT64_MAX; CHECK(!snapshot_copy(&device) && opens == 0);
    puts("PASS full eMMC snapshot: exact whole disk, readback, failure retention, media identity, no overwrite");
    return 0;
}
