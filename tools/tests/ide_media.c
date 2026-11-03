/* Exercise the real IDE controller lifecycle with in-memory media only.
 * clang -O1 -Wno-incompatible-pointer-types -Iefi/platform -Ipc
 *   tools/tests/ide_media.c -o work/ide-media-test.exe */
#include "../../pc/ide.c"
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static uint8_t old_data[4096], new_data[4096];
static int fail_sync, writes, syncs;
uint64_t time_us_64(void) { return 1; }
uint32_t time_us_32(void) { return 1; }
FRESULT f_open(FIL *f, const TCHAR *p, BYTE m) { (void)f; (void)p; (void)m; abort(); }
FRESULT f_close(FIL *f) { (void)f; abort(); }
FRESULT f_read(FIL *f, void *p, UINT n, UINT *got) { (void)f; (void)p; (void)n; (void)got; abort(); }
FRESULT f_lseek(FIL *f, FSIZE_t n) { (void)f; (void)n; abort(); }
PCIDevice *pci_register_device(PCIBus *b, const char *n, int fn,
                              uint16_t v, uint16_t d, uint8_t r, uint16_t c) {
    (void)b; (void)n; (void)fn; (void)v; (void)d; (void)r; (void)c; abort();
}
void pci_device_set_config8(PCIDevice *d, uint8_t a, uint8_t v) { (void)d; (void)a; (void)v; abort(); }
static void irq(void *p, int n, int level) { (void)p; (void)n; (void)level; }
static int seek(BlockDev *d, uint64_t p) { d->pos = p; return p <= 4096 ? 0 : -1; }
static uint64_t size(BlockDev *d) { (void)d; return 4096; }
static int read_bytes(BlockDev *d, void *p, unsigned n) {
    if (n > 4096 - d->pos) return -1;
    memcpy(p, (uint8_t *)d->ctx + d->pos, n); d->pos += n; return n;
}
static int write_bytes(BlockDev *d, const void *p, unsigned n) {
    if (n > 4096 - d->pos) return -1;
    memcpy((uint8_t *)d->ctx + d->pos, p, n); d->pos += n; writes++; return n;
}
static int sync_bytes(BlockDev *d) { (void)d; syncs++; return fail_sync ? -1 : 0; }
int main(void) {
    BlockDev d = {read_bytes, write_bytes, seek, size, sync_bytes, old_data, 0, 0};
    IDEIFState *bus = ide_allocate(14, NULL, irq);
    CHECK(ide_attach_ata(bus, 1, &d, 2, 16, 63) == 0);
    bus->cur_drive=bus->drives[1];
    IDEState *s = bus->drives[1];
    /* Flush still goes to the original binding. */
    s->wcache = malloc(512); memset(s->wcache, 0x37, 512); s->wcache_len = 512;
    CHECK(ide_prepare_media(bus, 1, 0) == 0);
    CHECK(writes == 1 && old_data[0] == 0x37 && new_data[0] == 0);
    CHECK(!ide_dirty[0]);
    /* A failed sync or an unfinished transfer refuses normal replacement. */
    fail_sync = 1; CHECK(ide_prepare_media(bus, 1, 0) < 0);
    fail_sync = 0; s->xfer_left = 512; s->wcache_len = 512;
    CHECK(ide_prepare_media(bus, 1, 0) < 0);
    CHECK(ide_prepare_media(bus, 1, 1) == 0 && writes == 1);
    d.ctx = new_data; d.pos = 0;
    ide_sync_ata(bus, 1, &d, 2, 16, 63);
    CHECK(!s->rcache_active && !s->wcache_len && !s->xfer_left);
    /* Both personality changes keep the selected drive pointer valid. */
    CHECK(ide_prepare_media(bus, 1, 1) == 0);
    ide_sync_cd(bus, 1, NULL);
    CHECK(bus->cur_drive == bus->drives[1] && bus->cur_drive->drive_kind == IDE_CD);
    CHECK(ide_prepare_media(bus, 1, 0) == 0);
    ide_sync_ata(bus, 1, &d, 2, 16, 63);
    CHECK(bus->cur_drive == bus->drives[1] && bus->cur_drive->drive_kind == IDE_HD);
    CHECK(new_data[0] == 0 && writes == 1);
    CHECK(ide_prepare_media(bus,1,0)==0);
    ide_sync_ata(bus,1,NULL,0,0,0);
    CHECK(!bus->drives[1] && bus->cur_drive==NULL);
    puts("PASS: old-binding flush, failed sync refusal, busy refusal, forced discard, no replay, empty slot, HDD/CD selected slave");
    return 0;
}
