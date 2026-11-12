/* Native Windows regression test of the actual USB BlockDev backend.
 * clang -std=gnu11 -O2 -Wall -Wextra -Ipc tools/tests/hostblk.c -o work/hostblk-test.exe
 * No real firmware, USB device or disk is accessed. */
#include "../../efi/platform/hostblk.c"

#define SECTORS 8192u
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

EFI_BOOT_SERVICES *gBS;
EFI_HANDLE gImage;
static EFI_BOOT_SERVICES services;
static EFI_BLOCK_IO_PROTOCOL bios[2];
static EFI_BLOCK_IO_MEDIA media[2];
static uint8_t physical[2][SECTORS * 512];
static int order[2], live_count;
static unsigned reads[2], writes[2], flushes[2];
static uint32_t written_id;
static EFI_STATUS write_status, flush_status;
static bool change_on_write, change_on_read;
static uint64_t ticks;

uint64_t wyse_ticks_us(void) { return ticks; }
void wyse_vga_flush_before_io(void) {}
EFI_HANDLE storage_boot_handle(void) { return NULL; }
EFI_DEVICE_PATH *bio_device_path(EFI_HANDLE h) { return (EFI_DEVICE_PATH *)h; }
bool bio_path_has(EFI_DEVICE_PATH *p, uint8_t t, uint8_t s) {
    (void)p; return t == DP_MESSAGING && s == DP_MSG_USB;
}
const char *fdd_get_filename(int i) { (void)i; return "mock"; }
uint8_t insertdisk(uint8_t n, bool fd, bool cd, const char *p) {
    (void)n; (void)fd; (void)cd; (void)p; abort();
}
void blk_host_media_changed(BlockDev *d) { CHECK(d != NULL); }

static EFI_STATUS handles(uint32_t type, const EFI_GUID *guid, void *key,
                          uint64_t *n, EFI_HANDLE **out) {
    (void)type; (void)guid; (void)key;
    *n = live_count;
    *out = malloc((live_count ? live_count : 1) * sizeof(**out));
    for (int i = 0; i < live_count; i++) (*out)[i] = &bios[order[i]];
    return 0;
}
static EFI_STATUS protocol(EFI_HANDLE handle, const EFI_GUID *guid, void **out) {
    if (memcmp(guid, &bio_guid, sizeof(*guid))) return EFI_UNSUPPORTED;
    *out = handle; return 0;
}
static EFI_STATUS free_pool(void *p) { free(p); return 0; }
static int index_of(EFI_BLOCK_IO_PROTOCOL *b) {
    CHECK(b == &bios[0] || b == &bios[1]); return (int)(b - bios);
}
static void check_io(EFI_BLOCK_IO_PROTOCOL *b, uint32_t id, uint64_t lba, uint64_t bytes) {
    CHECK(id == b->Media->MediaId && b->Media->MediaPresent);
    CHECK(bytes && bytes % b->Media->BlockSize == 0);
    CHECK(lba < SECTORS * 512u / b->Media->BlockSize);
    CHECK(bytes <= sizeof(physical[0]) - lba * b->Media->BlockSize);
}
static EFI_STATUS read_blocks(EFI_BLOCK_IO_PROTOCOL *b, uint32_t id, uint64_t lba,
                              uint64_t bytes, void *buf) {
    int i = index_of(b); check_io(b, id, lba, bytes); reads[i]++;
    if (change_on_read) { b->Media->MediaId++; return EFI_MEDIA_CHANGED; }
    memcpy(buf, physical[i] + lba * b->Media->BlockSize, (size_t)bytes); return 0;
}
static EFI_STATUS write_blocks(EFI_BLOCK_IO_PROTOCOL *b, uint32_t id, uint64_t lba,
                               uint64_t bytes, const void *buf) {
    int i = index_of(b); check_io(b, id, lba, bytes); writes[i]++; written_id = id;
    if (change_on_write) b->Media->MediaId++;
    if (EFI_ERROR(write_status)) return write_status;
    memcpy(physical[i] + lba * b->Media->BlockSize, buf, (size_t)bytes); return 0;
}
static EFI_STATUS flush_blocks(EFI_BLOCK_IO_PROTOCOL *b) {
    flushes[index_of(b)]++; return flush_status;
}
static void reset(void) {
    memset(usb_devs, 0, sizeof(usb_devs)); usb_change = NULL;
    memset(usb_blocked, 0, sizeof(usb_blocked));
    memset(usb_not_before,0,sizeof(usb_not_before));
    for (int i = 0; i < ndrives; i++) free(drives[i].disk_cache);
    memset(drives, 0, sizeof(drives)); ndrives = moans = 0;
    memset(physical, 0x7b, sizeof(physical));
    memset(reads, 0, sizeof(reads)); memset(writes, 0, sizeof(writes));
    memset(flushes, 0, sizeof(flushes)); ticks = 100;
    write_status = flush_status = 0; change_on_write = change_on_read = false;
    for (int i = 0; i < 2; i++) {
        media[i] = (EFI_BLOCK_IO_MEDIA){0}; media[i].MediaPresent = true;
        media[i].MediaId = 42 + i; media[i].BlockSize = 512; media[i].LastBlock = SECTORS - 1;
        bios[i] = (EFI_BLOCK_IO_PROTOCOL){0}; bios[i].Media = &media[i];
        bios[i].ReadBlocks = read_blocks; bios[i].WriteBlocks = write_blocks;
        bios[i].FlushBlocks = flush_blocks; order[i] = i;
        memset(physical[i], 0, 512); physical[i][510] = 0x55; physical[i][511] = 0xaa;
    }
    live_count = 1;
}
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void fat_vbr(uint32_t lba, bool fat32, uint32_t hidden) {
    uint8_t *p = physical[0] + lba * 512u;
    memset(p, 0, 512); p[0] = 0xeb; p[11] = 0; p[12] = 2; p[13] = 1;
    p[510] = 0x55; p[511] = 0xaa; put32(p + 28, hidden);
    memcpy(p + (fat32 ? 82 : 54), fat32 ? "FAT32   " : "FAT16   ", 8);
    p[50] = 9; // A backup at +9, not +6; neither should be patched.
}
static void partition(uint8_t type, uint32_t hidden) {
    write_mbr_entry(physical[0] + 446, 0x80, type, 63, SECTORS - 63);
    fat_vbr(63, true, hidden);
}
static void standard_and_bounds(void) {
    reset(); partition(0x0c, 63); BlockDev d = {0}, other = {0}; uint8_t buf[1024];
    CHECK(blk_bind_host(&d, "uhd1") && !d.readonly);
    CHECK(!blk_bind_host(&other, "uhd1") && !blk_bind_host(&d, "uhd1"));
    CHECK(blk_pread(&d, 0, buf, 512) == 512 && !memcmp(buf, physical[0], 512));
    memset(buf, 0xa5, sizeof(buf));
    CHECK(blk_pwrite(&d, 0, buf, 512) == 512 && writes[0] == 0);
    CHECK(blk_sync(&d) == 0 && writes[0] == 1 && flushes[0] == 1 && written_id == 42);
    CHECK(!memcmp(physical[0], buf, 512));
    CHECK(blk_sync(&d) == 0 && writes[0] == 1 && flushes[0] == 2);
    uint64_t size = blk_size(&d); CHECK(size == SECTORS * 512u);
    CHECK(blk_pwrite(&d, size - 512, buf, 512) == 512 && blk_sync(&d) == 0);
    unsigned calls = reads[0] + writes[0];
    CHECK(blk_seek(&d, size) == 0 && blk_read(&d, buf, 1) < 0 && blk_write(&d, buf, 1) < 0);
    CHECK(blk_seek(&d, size + 1) < 0 && blk_seek(&d, UINT64_MAX) < 0);
    CHECK(blk_seek(&d, size - 512) == 0 && blk_read(&d, buf, 1024) < 0 && blk_write(&d, buf, 1024) < 0);
    d.pos = UINT64_MAX - 511; CHECK(host_read(&d, buf, 1024) < 0 && host_write(&d, buf, 1024) < 0);
    CHECK(reads[0] + writes[0] == calls);
    CHECK(blk_seek(&d, 1) == 0 && blk_write(&d, buf, 13) == 13 && blk_sync(&d) == 0);
    flush_status = EFI_DEVICE_ERROR; CHECK(blk_sync(&d) < 0);
    flush_status = EFI_MEDIA_CHANGED; CHECK(blk_sync(&d) < 0 && blk_size(&d) == 0);
}
static void overlays(void) {
    for (int mode = 0; mode < 4; mode++) {
        reset();
        if (mode == 0) fat_vbr(0, true, 0); // superfloppy
        else if (mode == 1) partition(0xef, 63); // synthetic type only
        else if (mode == 2) partition(0x0c, 0); // VBR-only overlay
        else {
            write_mbr_entry(physical[0] + 446, 0, 0xee, 1, SECTORS - 1);
            memset(physical[0] + 512, 0, 1024); memcpy(physical[0] + 512, "EFI PART", 8);
            put64(physical[0] + 512 + 72, 2); put32(physical[0] + 512 + 80, 1);
            put32(physical[0] + 512 + 84, 128); put64(physical[0] + 1024 + 32, 63);
            put64(physical[0] + 1024 + 40, SECTORS - 1); fat_vbr(63, true, 0);
        }
        uint8_t original[512]; memcpy(original, physical[0], 512);
        BlockDev d = {0}; uint8_t buf[512]; CHECK(blk_bind_host(&d, "uhd1") && d.readonly);
        CHECK(blk_pread(&d, 0, buf, 512) == 512);
        CHECK(mode == 2 ? !memcmp(buf, original, 512) : memcmp(buf, original, 512) != 0);
        CHECK(blk_pread(&d, 63 * 512u, buf, 512) == 512);
        if (mode != 1) { uint32_t hidden; memcpy(&hidden, buf + 28, 4); CHECK(hidden == 63); }
        CHECK(host_write(&d, buf, 512) < 0 && blk_pwrite(&d, 0, buf, 512) < 0);
        CHECK(blk_pread(&d, 69 * 512u, buf, 512) == 512 && !memcmp(buf, physical[0] + (mode == 0 ? 6 : 69) * 512u, 512));
        CHECK(blk_pread(&d, 72 * 512u, buf, 512) == 512 && !memcmp(buf, physical[0] + (mode == 0 ? 9 : 72) * 512u, 512));
        CHECK(blk_sync(&d) == 0 && writes[0] == 0 && !memcmp(physical[0], original, 512));
    }
    // Invalid/non-FAT GPT entries must not overwrite the fallback MBR buffer.
    reset(); write_mbr_entry(physical[0] + 446, 0, 0xee, 1, SECTORS - 1);
    memset(physical[0] + 512, 0, 1024); memcpy(physical[0] + 512, "EFI PART", 8);
    put64(physical[0] + 512 + 72, 2); put32(physical[0] + 512 + 80, 1);
    put32(physical[0] + 512 + 84, 128); put64(physical[0] + 1024 + 32, 63);
    put64(physical[0] + 1024 + 40, 100);
    BlockDev d = {0}; uint8_t buf[512]; CHECK(blk_bind_host(&d, "uhd1"));
    CHECK(blk_pread(&d, 0, buf, 512) == 512 && !memcmp(buf, physical[0], 512));
}
static void identities(void) {
    uint8_t buf[512]; BlockDev d = {0};
    reset(); CHECK(blk_bind_host(&d, "uhd1")); CHECK(blk_pread(&d, 0, buf, 512) == 512);
    unsigned calls = reads[0]; live_count = 0;
    CHECK(host_read(&d, buf, 512) < 0 && reads[0] == calls && blk_size(&d) == 0);
    live_count = 1; CHECK(host_read(&d, buf, 512) < 0); // failed binding cannot resurrect

    reset(); CHECK(blk_bind_host(&d, "uhd1")); CHECK(blk_pwrite(&d, 0, buf, 512) == 512);
    live_count = 1; order[0] = 1;
    CHECK(blk_sync(&d) < 0 && writes[0] == 0 && writes[1] == 0 && drives[0].bio == &bios[0]);

    reset(); live_count = 2; CHECK(blk_bind_host(&d, "uhd1"));
    order[0] = 1; order[1] = 0; // reordering must still use the original bio
    CHECK(blk_pwrite(&d, 0, buf, 512) == 512 && blk_sync(&d) == 0 && writes[0] == 1 && writes[1] == 0);

    reset(); CHECK(blk_bind_host(&d, "uhd1")); CHECK(blk_pwrite(&d, 0, buf, 512) == 512);
    media[0].MediaId++; CHECK(host_write(&d, buf, 512) < 0 && blk_sync(&d) < 0 && writes[0] == 0);

    reset(); CHECK(blk_bind_host(&d, "uhd1")); CHECK(blk_pwrite(&d, 0, buf, 512) == 512);
    write_status = EFI_MEDIA_CHANGED; change_on_write = true;
    CHECK(blk_sync(&d) < 0 && writes[0] == 1 && written_id == 42 && flushes[0] == 0);
    CHECK(blk_sync(&d) < 0 && writes[0] == 1); // never retry with new ID

    reset(); CHECK(blk_bind_host(&d, "uhd1")); change_on_read = true;
    calls = reads[0]; CHECK(blk_pread(&d, 512, buf, 512) < 0 && reads[0] == calls + 1);

    reset(); CHECK(blk_bind_host(&d, "uhd1")); CHECK(blk_pread(&d, 0, buf, 512) == 512);
    media[0].MediaId++; calls = reads[0]; CHECK(host_read(&d, buf, 512) < 0 && reads[0] == calls);

    reset(); media[0].LastBlock = 2879; CHECK(blk_bind_host(&d, "ufd1"));
    CHECK(blk_pread(&d, 0, buf, 512) == 512);
    CHECK(blk_pread(&d, 8 * 512u, buf, 512) == 512);
    calls = reads[0];
    CHECK(blk_pread(&d, 0, buf, 512) == 512 && reads[0] == calls); // full-floppy cache hit
    CHECK(blk_pread(&d, 8 * 512u, buf, 512) == 512);
    media[0].MediaPresent = false; calls = reads[0];
    d.pos = 0; CHECK(host_read(&d, buf, 512) < 0 && reads[0] == calls);

    reset(); media[0].ReadOnly = true; CHECK(blk_bind_host(&d, "uhd1") && d.readonly);
    CHECK(host_write(&d, buf, 512) < 0 && writes[0] == 0);
    reset(); CHECK(blk_bind_host(&d, "uhd1")); CHECK(blk_pwrite(&d, 0, buf, 512) == 512);
    media[0].ReadOnly = true; CHECK(blk_sync(&d) < 0 && writes[0] == 0);
}
static void media_bounds(void) {
    BlockDev d = {0}; uint8_t buf[2048];
    reset(); media[0].MediaPresent = false; CHECK(blk_bind_host(&d, "uhd1"));
    CHECK(blk_size(&d) == 0 && blk_seek(&d, 0) < 0 && host_read(&d, buf, 1) < 0 && host_write(&d, buf, 1) < 0 && reads[0] == 0);
    reset(); media[0].BlockSize = 4096; CHECK(!blk_bind_host(&d, "uhd1") && reads[0] == 0);
    reset(); media[0].LastBlock = UINT64_MAX; CHECK(!blk_bind_host(&d, "uhd1") && blk_host_size_mb("uhd1") == 0);
    reset(); media[0].LastBlock = UINT64_MAX / 512; CHECK(!blk_bind_host(&d, "uhd1"));
    reset(); media[0].BlockSize = 2048; media[0].LastBlock = SECTORS / 4 - 1;
    CHECK(blk_bind_host(&d, "ucd1") && d.readonly && blk_size(&d) == sizeof(physical[0]));
    CHECK(blk_pread(&d, sizeof(physical[0]) - 2048, buf, 2048) == 2048);
    CHECK(host_write(&d, buf, 1) < 0 && blk_sync(&d) == 0 && flushes[0] == 1);
    reset(); CHECK(blk_bind_host(&d, "uhd1")); media[0].BlockSize = 4096;
    CHECK(host_read(&d, buf, 1) < 0 && blk_size(&d) == 0);
}
static unsigned connected, disconnected;
static void usb_changed(int port, BlockDev *dev, unsigned serial) {
    CHECK(port >= 0 && port < 2);
    /* Repeated insertion must reuse the same Windows PnP identity. */
    if (dev) { CHECK(serial == (unsigned)port + 1 && blk_size(dev)); connected++; }
    else disconnected++;
}
static void hotplug(void) {
    reset(); connected = disconnected = 0;
    live_count = 2; hostblk_usb_start(usb_changed);
    CHECK(connected == 2 && usb_devs[0].ctx && usb_devs[1].ctx);
    uint8_t data[512]; memset(data, 0x99, sizeof(data));
    CHECK(blk_pwrite(&usb_devs[0], 512, data, 512) == 512);
    order[0] = 1; live_count = 1; hostblk_poll();
    CHECK(disconnected == 1 && !usb_devs[0].ctx && usb_devs[1].ctx);
    CHECK(writes[0] == 0 && writes[1] == 0);
    live_count = 2; order[1] = 0; hostblk_poll();
    CHECK(connected == 3 && usb_devs[0].ctx);
    CHECK(blk_pread(&usb_devs[0], 512, data, 512) == 512 && data[0] == 0x7b);
    media[0].MediaId++; hostblk_poll();
    CHECK(disconnected == 2 && !usb_devs[0].ctx);
    hostblk_poll(); CHECK(connected == 4 && usb_devs[0].ctx);
    flush_status = EFI_DEVICE_ERROR;
    CHECK(!hostblk_usb_eject(0) && disconnected == 2);
    flush_status = 0; CHECK(hostblk_usb_eject(0) && disconnected == 3);
    hostblk_poll(); CHECK(!usb_devs[0].ctx && connected == 4);
    CHECK(hostblk_usb_insert(0, "uhd2") && connected == 5);
    CHECK(!hostblk_usb_insert(1, "uhd2") && writes[0] == 0 && writes[1] == 0);
}
static void image_ownership(void) {
    reset(); connected=disconnected=0; hostblk_usb_start(usb_changed);
    void *identity; uint32_t media_id;
    CHECK(hostblk_image_info("uhd1",&identity,&media_id));
    BlockDev image={0},duplicate={0}; int port=-1;
    CHECK(!hostblk_image_take("uhd1",identity,media_id+1,&image,&port));
    CHECK(disconnected==0 && usb_devs[0].ctx);
    CHECK(hostblk_image_take("uhd1",identity,media_id,&image,&port));
    CHECK(port==0 && disconnected==1 && !usb_devs[0].ctx && image.ctx);
    hostblk_poll(); CHECK(!usb_devs[0].ctx); /* Not handed back while owned. */
    CHECK(!hostblk_image_take("uhd1",identity,media_id,&duplicate,&port));
    uint8_t data[512]; memset(data,0x56,sizeof(data));
    CHECK(blk_pwrite(&image,512,data,512)==512);
    flush_status=EFI_DEVICE_ERROR;
    CHECK(!hostblk_image_return(&image,0) && image.ctx && !usb_devs[0].ctx);
    flush_status=0; CHECK(hostblk_image_return(&image,0) && !image.ctx);
    hostblk_poll(); CHECK(!usb_devs[0].ctx); /* Disconnect must be observable. */
    ticks+=500001; hostblk_poll(); CHECK(usb_devs[0].ctx && connected==2);
    CHECK(physical[0][512]==0x56);
}
int main(void) {
    services.LocateHandleBuffer = handles; services.HandleProtocol = protocol;
    services.FreePool = free_pool; gBS = &services;
#if WYSE_POC_READ_ONLY
    reset(); partition(0x0c, 63);
    BlockDev device = {0}; uint8_t data[512];
    CHECK(blk_bind_host(&device, "uhd1"));
    CHECK(blk_pread(&device, 512, data, sizeof(data)) == sizeof(data));
    memset(data, 0x56, sizeof(data));
    CHECK(blk_pwrite(&device, 512, data, sizeof(data)) == sizeof(data));
    CHECK(blk_sync(&device) < 0 && writes[0] == 0 && physical[0][512] == 0x7b);
    puts("PASS: isolated POC rejects USB cache writes before firmware access");
#else
    standard_and_bounds(); overlays(); identities(); media_bounds(); hotplug(); image_ownership(); reset();
    puts("PASS: standard MBR read/write, all overlay RO, GPT fallback, primary-only VBR, cache unplug/MediaId rejection, pinned bio, duplicate binding, bounds, 512/2048, FlushBlocks and no write retries");
#endif
    return 0;
}
