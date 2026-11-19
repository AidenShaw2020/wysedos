/* Real controller tests of ATA data-out state ordering and bus timing.
 * The CPU clock is deterministic; no filesystem or physical device is used. */
#define IDE_MEDIA_EXTERNAL_CLOCK 1
#define main ide_media_fixture_main
#include "ide_media.c"
#undef main

static uint64_t now_us = 1000000;
static unsigned irq_raises;
static int irq_level, fail_write;
uint64_t time_us_64(void) { return now_us; }
uint32_t time_us_32(void) { return (uint32_t)now_us; }
static void observe_irq(void *unused, int number, int level)
{
    (void)unused; (void)number;
    if (level && !irq_level) ++irq_raises;
    irq_level = level;
}
static int backing_write(BlockDev *disk, const void *data, unsigned bytes)
{
    return fail_write ? -1 : write_bytes(disk, data, bytes);
}
static void ack(IDEIFState *bus)
{
    (void)ide_ioport_read(bus, 7);
    CHECK(!irq_level);
}
static void begin(IDEIFState *bus, unsigned select, unsigned command, unsigned count)
{
    ide_ioport_write(bus, 6, select);
    ide_ioport_write(bus, 2, count);
    ide_ioport_write(bus, 3, 0);
    ide_ioport_write(bus, 4, 0);
    ide_ioport_write(bus, 5, 0);
    ide_ioport_write(bus, 7, command);
    CHECK((ide_status_read(bus) & (BUSY_STAT | DRQ_STAT)) == DRQ_STAT);
}
static void words(IDEIFState *bus, unsigned count, unsigned value)
{
    for (unsigned i = 0; i < count; ++i) ide_data_writew(bus, value);
}
static void busy(IDEIFState *bus, unsigned before)
{
    CHECK((ide_status_read(bus) & (BUSY_STAT | DRQ_STAT)) == BUSY_STAT);
    CHECK(irq_raises == before && !irq_level);
}
static void mode(IDEIFState *bus, unsigned transfer)
{
    ide_ioport_write(bus, 1, 3);
    ide_ioport_write(bus, 2, transfer);
    ide_ioport_write(bus, 7, WIN_SETFEATURES);
    CHECK(!(ide_status_read(bus) & ERR_STAT));
    ack(bus);
}

int main(void)
{
    BlockDev disk = {read_bytes, backing_write, seek, size, sync_bytes, old_data, 0, 0};
    IDEIFState *bus = ide_allocate(14, NULL, observe_irq);
    unsigned before;
    CHECK(ide_attach_ata(bus, 0, &disk, 2, 16, 63) == 0);
    /* PIO2: 256 words at240ns need61.44us, rounded upward to62us.
     * Receiving the last word must not claim completion inside that OUT. */
    before = irq_raises;
    begin(bus, 0xe0, WIN_WRITE, 1);
    words(bus, 256, 0x1234);
    busy(bus, before);
    CHECK(!writes && old_data[0] == 0);
    ide_data_writew(bus, 0xffff); /* No DRQ: cannot corrupt queued data. */
    now_us += 61; ide_poll(bus); busy(bus, before);
    ++now_us; ide_poll(bus);
    CHECK(ide_status_read(bus) == (READY_STAT | SEEK_STAT));
    CHECK(irq_raises == before + 1 && irq_level);
    CHECK(writes == 1 && old_data[0] == 0x34 && old_data[1] == 0x12);
    ide_poll(bus); CHECK(irq_raises == before + 1); ack(bus);
    /* Normal multi-sector writes publish one DRQ/IRQ per block, and only
     * commit the gathered command before reporting its final completion. */
    before = irq_raises;
    begin(bus, 0xe0, WIN_WRITE, 2);
    words(bus, 256, 0x5678); busy(bus, before);
    now_us += 62; ide_poll(bus);
    CHECK((ide_status_read(bus) & (BUSY_STAT | DRQ_STAT)) == DRQ_STAT);
    CHECK(irq_raises == before + 1 && writes == 1); ack(bus);
    words(bus, 256, 0x9abc); busy(bus, before + 1);
    now_us += 62; ide_poll(bus);
    CHECK(ide_status_read(bus) == (READY_STAT | SEEK_STAT));
    CHECK(old_data[0] == 0x78 && old_data[512] == 0xbc);
    CHECK(irq_raises == before + 2); ack(bus);
    /* REP/string and 32-bit ports account for the same16-bit ATA cycles. */
    {
        uint16_t data[256];
        for (unsigned i = 0; i < 256; ++i) data[i] = (uint16_t)i;
        before = irq_raises;
        begin(bus, 0xe0, WIN_WRITE, 1);
        CHECK(ide_data_write_string(bus, (uint8_t *)data, 2, 256) == 256);
        busy(bus, before); now_us += 61; ide_poll(bus); busy(bus, before);
        ++now_us; ide_poll(bus); CHECK(irq_raises == before + 1); ack(bus);
        CHECK(!memcmp(old_data, data, 512));
        before = irq_raises;
        begin(bus, 0xe0, WIN_WRITE, 1);
        for (unsigned i = 0; i < 128; ++i) ide_data_writel(bus, 0xaabbccdd);
        busy(bus, before); now_us += 62; ide_poll(bus); ack(bus);
        CHECK(irq_raises == before + 1 && old_data[0] == 0xdd && old_data[3] == 0xaa);
    }
    /* A slower CPU has already spent the bus time; do not wait it twice. */
    before = irq_raises;
    begin(bus, 0xe0, WIN_WRITE, 1);
    for (unsigned i = 0; i < 256; ++i) { ++now_us; ide_data_writew(bus, 0x4321); }
    busy(bus, before); ide_poll(bus);
    CHECK(irq_raises == before + 1); ack(bus);
    /* Negotiated PIO0/1/2 timing, without an operating-system switch. */
    for (unsigned transfer = 8; transfer <= 10; ++transfer) {
        const unsigned delay[3] = {154, 99, 62};
        mode(bus, transfer);
        before = irq_raises;
        begin(bus, 0xe0, WIN_WRITE, 1); words(bus, 256, 0x2468);
        now_us += delay[transfer - 8] - 1; ide_poll(bus); busy(bus, before);
        ++now_us; ide_poll(bus); CHECK(irq_raises == before + 1); ack(bus);
    }
    for (unsigned transfer = 0x0b; transfer <= 0x20; ++transfer) {
        if (transfer != 0x0b && transfer != 0x0c && transfer != 0x20) continue;
        ide_ioport_write(bus, 1, 3); ide_ioport_write(bus, 2, transfer);
        ide_ioport_write(bus, 7, WIN_SETFEATURES);
        CHECK(ide_status_read(bus) & ERR_STAT); ack(bus);
    }
    mode(bus, 0);
    /* WRITE MULTIPLE uses one DRQ block for both sectors, and consequently
     * one completion after512 ATA words, rather than two sector IRQs. */
    ide_ioport_write(bus, 2, 2); ide_ioport_write(bus, 7, WIN_SETMULT); ack(bus);
    before = irq_raises;
    begin(bus, 0xe0, WIN_MULTWRITE, 2); words(bus, 512, 0x369c);
    now_us += 122; ide_poll(bus); busy(bus, before);
    ++now_us; ide_poll(bus);
    CHECK(ide_status_read(bus) == (READY_STAT | SEEK_STAT));
    CHECK(irq_raises == before + 1 && old_data[0] == 0x9c && old_data[512] == 0x9c); ack(bus);
    /* Every block follows the same protocol; there is no sixteenth-block
     * operating-system-specific pause in the normal controller. */
    for (unsigned block = 0; block < 17; ++block) {
        before = irq_raises;
        begin(bus, 0xe0, WIN_WRITE, 1); words(bus, 256, 0x4567);
        now_us += 62; ide_poll(bus);
        CHECK(ide_status_read(bus) == (READY_STAT | SEEK_STAT));
        CHECK(irq_raises == before + 1); ack(bus);
    }
    /* Polling clients with nIEN still get completed data and ready status. */
    before = irq_raises;
    ide_cmd_write(bus, IDE_CMD_DISABLE_IRQ);
    begin(bus, 0xe0, WIN_WRITE, 1); words(bus, 256, 0x7531);
    now_us += 62; ide_poll(bus);
    CHECK(ide_status_read(bus) == (READY_STAT | SEEK_STAT));
    CHECK(irq_raises == before && old_data[0] == 0x31);
    ide_cmd_write(bus, 0);
    /* Extra task-file/command writes while BSY cannot replace the command. */
    before = irq_raises;
    begin(bus, 0xe0, WIN_WRITE, 1); words(bus, 256, 0x1357);
    ide_ioport_write(bus, 2, 7); ide_ioport_write(bus, 7, WIN_IDENTIFY);
    busy(bus, before); now_us += 62; ide_poll(bus);
    CHECK(ide_status_read(bus) == (READY_STAT | SEEK_STAT));
    CHECK(ide_ioport_read(bus, 2) == 0 && old_data[0] == 0x57); ack(bus);
    /* Real backend errors remain errors, with exactly one completion. */
    before = irq_raises; fail_write = 1;
    begin(bus, 0xe0, WIN_WRITE, 1); words(bus, 256, 0xabcd);
    busy(bus, before); now_us += 62; ide_poll(bus);
    CHECK((ide_status_read(bus) & (BUSY_STAT | DRQ_STAT | ERR_STAT)) == ERR_STAT);
    CHECK(irq_raises == before + 1); ack(bus); fail_write = 0;
    /* Reset preserves received bytes but cancels the old completion/IRQ. */
    before = irq_raises;
    begin(bus, 0xe0, WIN_WRITE, 1); words(bus, 256, 0x7654);
    ide_cmd_write(bus, IDE_CMD_RESET); ide_cmd_write(bus, 0);
    now_us += 1000; ide_poll(bus);
    CHECK(irq_raises == before && old_data[0] == 0x54);
    /* Normal media removal refuses an outstanding block; explicit discard
     * cancels it before detaching, so a later poll cannot use freed state. */
    before = irq_raises;
    begin(bus, 0xe0, WIN_WRITE, 1); words(bus, 256, 0x9999);
    CHECK(ide_prepare_media(bus, 0, 0) < 0);
    CHECK(ide_prepare_media(bus, 0, 1) == 0);
    ide_sync_ata(bus, 0, NULL, 0, 0, 0);
    now_us += 1000; ide_poll(bus); CHECK(irq_raises == before);
    puts("PASS: ATA BSY/DRQ/IRQ order, PIO0/1/2 wire cycles, word/dword/REP, sector/multiple writes, nIEN, busy writes, errors, reset/media");
    return 0;
}
