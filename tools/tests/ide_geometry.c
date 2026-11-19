/* Reuse the existing memory-only controller fixture and lifecycle checks.
 * clang -O2 -Wno-incompatible-pointer-types -Iefi/platform -Ipc
 *   tools/tests/ide_geometry.c -o work/ide-geometry-test.exe */
#define main ide_media_lifecycle_checks
#include "ide_media.c"
#undef main

static uint64_t large_size(BlockDev *d) { (void)d; return 7ull << 30; }
static uint8_t save_cmos(void *context, int address, uint8_t value) {
    ((uint8_t *)context)[address] = value;
    return value;
}

static void identify_words(IDEIFState *bus, uint16_t words[256]) {
    ide_ioport_write(bus, 6, 0xa0);
    ide_ioport_write(bus, 7, WIN_IDENTIFY);
    CHECK(ide_ioport_read(bus, 7) & DRQ_STAT);
    for (int i = 0; i < 256; ++i) words[i] = (uint16_t)ide_data_readw(bus);
    CHECK(!(ide_ioport_read(bus, 7) & DRQ_STAT));
}

int main(void) {
    CHECK(ide_media_lifecycle_checks() == 0);
    BlockDev disk = {0}; disk.size = large_size; disk.read = read_bytes; disk.sync = sync_bytes;
    IDEIFState *bus = ide_allocate(14, NULL, irq);
    const int bios_cylinders = (7ull << 30) / (255 * 63 * 512);
    CHECK(ide_attach_ata(bus, 0, &disk, bios_cylinders, 255, 63) == 0);
    IDEState *drive = bus->drives[0];
    uint16_t words[256]; identify_words(bus, words);
    CHECK(words[3] == 16 && words[55] == 16 && words[6] == 63);
    CHECK(words[1] == (7ull << 30) / (16 * 63 * 512));
    CHECK(((uint32_t)words[60] | (uint32_t)words[61] << 16) == (7ull << 30) / 512);
    CHECK(drive->cur_heads == 16);

    /* Reproduce NT's 256-sector VERIFY progression from the first partition.
     * With 255 advertised ATA heads, its fifth command selects head17,
     * which is physically the device-select bit plus head1: absent slave. */
    const unsigned heads = words[3], sectors = words[6];
    for (unsigned lba = 63; lba < 63 + 4096; lba += 256) {
        unsigned cylinder = lba / (heads * sectors);
        unsigned head = lba / sectors % heads;
        CHECK(head < 16);
        ide_ioport_write(bus, 6, 0xa0 | head);
        CHECK(bus->cur_drive == drive);
        ide_ioport_write(bus, 2, 0); /* ATA count0 means256 sectors. */
        ide_ioport_write(bus, 3, lba % sectors + 1);
        ide_ioport_write(bus, 4, cylinder & 255);
        ide_ioport_write(bus, 5, cylinder >> 8);
        CHECK(ide_get_sector(drive) == lba);
        ide_ioport_write(bus, 7, WIN_VERIFY);
        CHECK(ide_ioport_read(bus, 7) == (READY_STAT | SEEK_STAT));
        CHECK(ide_get_sector(drive) == lba + 255);
    }
    /* DOS/BIOS CHS translation remains the original 255-head geometry. */
    uint8_t cmos[256] = {0};
    ide_fill_cmos(bus, cmos, save_cmos);
    CHECK(cmos[0x1d] == 255 && cmos[0x23] == 63);
    CHECK((cmos[0x1b] | cmos[0x1c] << 8) == bios_cylinders);
    CHECK(ide_prepare_media(bus, 0, 0) == 0);
    ide_sync_ata(bus, 0, &disk, bios_cylinders, 255, 63);
    identify_words(bus, words);
    CHECK(words[3] == 16 && words[55] == 16 && drive->cur_heads == 16);
    /* Genuine small ATA geometry is retained. */
    ide_sync_ata(bus, 0, &disk, 400, 4, 63);
    identify_words(bus, words);
    CHECK(words[1] == 400 && words[3] == 4 && words[55] == 4);
    puts("PASS: ATA16-head limit, NT CHS VERIFY crosses head15 without selecting slave, full LBA capacity, BIOS255-head translation, media rebind and small CHS");
    return 0;
}
