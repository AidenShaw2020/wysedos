#include "../../pc/fdd.c"

static int medium_present = 1;
static uint8_t dma_buffer[36 * FDC_SECTOR_SIZE];
static unsigned sectors_read;
static unsigned irq_rises;
static int irq_level;
static uint64_t mock_time_us;

uint64_t time_us_64(void) { return mock_time_us; }

static int medium_seek(BlockDev *device, uint64_t offset)
{
    device->pos = offset;
    return 0;
}

static int medium_read(BlockDev *device, void *buffer, unsigned length)
{
    if (length != FDC_SECTOR_SIZE) abort();
    memset(buffer, (uint8_t)(device->pos / FDC_SECTOR_SIZE + 1), length);
    device->pos += length;
    sectors_read++;
    return (int)length;
}

static BlockDev medium = { .read = medium_read, .seek = medium_seek };
uint8_t fdd_is_inserted(uint8_t drive) { return drive < 2 && medium_present; }
uint16_t fdd_get_cyls(uint8_t drive) { (void)drive; return 80; }
uint16_t fdd_get_heads(uint8_t drive) { (void)drive; return 2; }
uint16_t fdd_get_sects(uint8_t drive) { (void)drive; return 18; }
BlockDev *fdd_get_dev(uint8_t drive) { (void)drive; return &medium; }
void i8257_dma_register_channel(IsaDma *dma, int channel,
                               IsaDmaTransferHandler handler, void *opaque)
{
    (void)dma; (void)channel; (void)handler; (void)opaque; abort();
}
void i8257_dma_hold_DREQ(IsaDma *dma, int channel)
{
    (void)dma; (void)channel;
}
void i8257_dma_release_DREQ(IsaDma *dma, int channel)
{
    (void)dma; (void)channel;
}
int i8257_dma_read_memory(IsaDma *dma, int channel, void *buffer, int offset, int length)
{
    (void)dma; (void)channel; (void)buffer; (void)offset; (void)length; abort();
}
int i8257_dma_write_memory(IsaDma *dma, int channel, void *buffer, int offset, int length)
{
    (void)dma; (void)channel;
    if (offset < 0 || length < 0 || (unsigned)offset + (unsigned)length > sizeof(dma_buffer)) abort();
    memcpy(dma_buffer + offset, buffer, (size_t)length);
    return length;
}
void i8259_set_irq(PicState2 *pic, int irq, int level)
{
    (void)pic;
    if (irq != FDC_IRQ) abort();
    if (level && !irq_level) irq_rises++;
    irq_level = level;
}
void io_trace_note(int kind, int port, int value)
{
    (void)kind; (void)port; (void)value;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void read_id(FDCState *controller, uint8_t drive_head, uint8_t result[7])
{
    fdc_ioport_write(controller, 0x3f5, 0x4a);
    fdc_ioport_write(controller, 0x3f5, drive_head);
    mock_time_us += 20000;
    fdc_tick(controller);
    for (unsigned index = 0; index < 7; index++)
        result[index] = fdc_ioport_read(controller, 0x3f5);
}

int main(void)
{
    FDCState controller = {0};
    controller.phase = PHASE_CMD;
    CHECK(fdc_ioport_read(&controller, 0x3f4) == MSR_RQM);
    controller.cmd_received = 1;
    CHECK(fdc_ioport_read(&controller, 0x3f4) == (MSR_RQM | MSR_CB));
    controller.cmd_received = 2;
    CHECK(fdc_ioport_read(&controller, 0x3f4) == (MSR_RQM | MSR_CB));
    controller.cmd_received = 0;
    CHECK(fdc_ioport_read(&controller, 0x3f4) == MSR_RQM);
    controller.phase = PHASE_EXEC;
    CHECK(fdc_ioport_read(&controller, 0x3f4) == MSR_CB);
    controller.phase = PHASE_RESULT;
    CHECK(fdc_ioport_read(&controller, 0x3f4) == (MSR_RQM | MSR_DIO | MSR_CB));
    controller.drive[0].seeking = 1;
    CHECK(fdc_ioport_read(&controller, 0x3f4) ==
          (MSR_RQM | MSR_DIO | MSR_CB | MSR_ACTA));
    puts("PASS: FDC MSR idle, command parameters, execution, result and seek states");
    memset(&controller, 0, sizeof(controller));
    controller.dor = DOR_NRESET | DOR_DMAEN;
    fdc_reset(&controller, 0);
    uint8_t result[7];
    irq_level = 0;
    irq_rises = 0;
    read_id(&controller, 0, result);
    fdc_tick(&controller);
    fdc_tick(&controller);
    CHECK(irq_rises == 1);
    CHECK(irq_level == 0 && controller.irq_pending == 0);
    puts("PASS: FDC completion IRQ survives immediate result polling without a late interrupt");
    fdc_reset(&controller, 0);
    unsigned before_read_id = irq_rises;
    fdc_ioport_write(&controller, 0x3f5, 0x4a);
    fdc_ioport_write(&controller, 0x3f5, 0);
    CHECK(fdc_ioport_read(&controller, 0x3f4) == MSR_CB);
    CHECK(irq_rises == before_read_id && irq_level == 0);
    mock_time_us += 19999;
    for (unsigned tick = 0; tick < 100; tick++) fdc_tick(&controller);
    CHECK(fdc_ioport_read(&controller, 0x3f4) == MSR_CB);
    CHECK(irq_rises == before_read_id && irq_level == 0);
    mock_time_us++;
    fdc_tick(&controller);
    CHECK(fdc_ioport_read(&controller, 0x3f4) == (MSR_RQM | MSR_DIO | MSR_CB));
    CHECK(irq_rises == before_read_id + 1 && irq_level == 1);
    for (unsigned index = 0; index < 7; index++) result[index] = fdc_ioport_read(&controller, 0x3f5);
    CHECK(result[0] == 0 && result[5] == 1 && result[6] == 2);
    CHECK(irq_level == 0);
    fdc_ioport_write(&controller, 0x3f5, 0x4a);
    fdc_ioport_write(&controller, 0x3f5, 0);
    fdc_reset(&controller, 0);
    unsigned before_cancel = irq_rises;
    mock_time_us += 20000;
    fdc_tick(&controller);
    CHECK(fdc_ioport_read(&controller, 0x3f4) == MSR_RQM);
    CHECK(irq_rises == before_cancel && irq_level == 0);
    puts("PASS: FDC READ ID wall-clock deadline, execution status and reset cancellation");
    for (unsigned index = 0; index < 36; index++) {
        read_id(&controller, 0, result);
        CHECK(result[0] == 0 && result[1] == 0 && result[2] == 0);
        CHECK(result[3] == 0 && result[4] == 0 && result[5] == index % 18 + 1 && result[6] == 2);
        CHECK(fdc_ioport_read(&controller, 0x3f4) == MSR_RQM);
    }
    read_id(&controller, 5, result);
    CHECK(result[0] == (ST0_HD | 1) && result[4] == 1 && result[5] == 1);
    read_id(&controller, 4, result);
    CHECK(result[0] == ST0_HD && result[4] == 1 && result[5] == 1);
    medium_present = 0;
    read_id(&controller, 0, result);
    CHECK(result[0] == (ST0_IC_ABNORM | ST0_NR) && result[1] == ST1_MA);
    medium_present = 1;
    read_id(&controller, 0, result);
    CHECK(result[5] == 2);
    fdc_reset(&controller, 0);
    read_id(&controller, 0, result);
    CHECK(result[5] == 1);
    puts("PASS: FDC READ ID rotation, wrap, head/drive selection, no media and reset");
    unsigned before_reset = irq_rises;
    fdc_reset(&controller, 1);
    CHECK(controller.irq_pending && irq_level == 0);
    fdc_tick(&controller);
    CHECK(irq_rises == before_reset && irq_level == 0);
    fdc_tick(&controller);
    CHECK(irq_rises == before_reset + 1 && irq_level == 1);
    for (unsigned drive = 0; drive < 4; drive++) {
        fdc_ioport_write(&controller, 0x3f5, 0x08);
        CHECK(fdc_ioport_read(&controller, 0x3f5) == (ST0_IC_RDYCHG | drive));
        CHECK(fdc_ioport_read(&controller, 0x3f5) == 0);
        CHECK(fdc_ioport_read(&controller, 0x3f4) == MSR_RQM);
    }
    fdc_ioport_write(&controller, 0x3f5, 0x08);
    CHECK(fdc_ioport_read(&controller, 0x3f5) == ST0_IC_INVAL);
    CHECK(fdc_ioport_read(&controller, 0x3f4) == MSR_RQM);
    CHECK(fdc_ioport_read(&controller, 0x3f5) == 0xff);
    fdc_ioport_write(&controller, 0x3f5, 0x10);
    CHECK(fdc_ioport_read(&controller, 0x3f5) == 0x90);
    CHECK(fdc_ioport_read(&controller, 0x3f4) == MSR_RQM);
    puts("PASS: FDC reset interrupt queue and single-byte invalid SENSE INTERRUPT result");
    controller.dor = DOR_NRESET;
    unsigned before_masked = irq_rises;
    read_id(&controller, 0, result);
    fdc_tick(&controller);
    fdc_tick(&controller);
    CHECK(irq_rises == before_masked && irq_level == 0);
    CHECK(result[0] == 0 && result[1] == 0 && result[2] == 0);
    controller.dor = DOR_NRESET | DOR_DMAEN;
    puts("PASS: FDC DOR IRQ gate and deferred reset interrupt");
    const uint8_t read_cylinder[] = { 0xc6, 0, 0, 0, 1, 2, 18, 27, 255 };
    const unsigned transfer_sectors[] = { 1, 18, 36 };
    for (unsigned test = 0; test < sizeof(transfer_sectors) / sizeof(transfer_sectors[0]); test++) {
        fdc_reset(&controller, 0);
        for (unsigned index = 0; index < sizeof(read_cylinder); index++)
            fdc_ioport_write(&controller, 0x3f5, read_cylinder[index]);
        CHECK(controller.phase == PHASE_EXEC && controller.dma_active);
        sectors_read = 0;
        memset(dma_buffer, 0, sizeof(dma_buffer));
        unsigned length = transfer_sectors[test] * FDC_SECTOR_SIZE;
        unsigned before_dma = irq_rises;
        CHECK(fdc_dma_handler(&controller, FDC_DMA_CHAN, 0, (int)length) == (int)length);
        CHECK(irq_rises == before_dma + 1 && irq_level == 1);
        CHECK(controller.irq_pending == 0);
        CHECK(sectors_read == transfer_sectors[test]);
        for (unsigned index = 0; index < length; index++)
            CHECK(dma_buffer[index] == index / FDC_SECTOR_SIZE + 1);
        for (unsigned index = length; index < sizeof(dma_buffer); index++)
            CHECK(dma_buffer[index] == 0);
        for (unsigned index = 0; index < sizeof(result); index++)
            result[index] = fdc_ioport_read(&controller, 0x3f5);
        CHECK(irq_level == 0);
        fdc_tick(&controller);
        fdc_tick(&controller);
        CHECK(irq_rises == before_dma + 1 && irq_level == 0);
        uint8_t cylinder = transfer_sectors[test] == 36;
        uint8_t head = transfer_sectors[test] == 18;
        uint8_t sector = transfer_sectors[test] == 1 ? 2 : 1;
        CHECK(result[0] == ((cylinder ? ST0_SE : 0) | (head ? ST0_HD : 0)));
        CHECK(result[1] == 0 && result[2] == 0 && result[6] == 2);
        CHECK(result[3] == cylinder && result[4] == head && result[5] == sector);
        CHECK(controller.drive[0].track == cylinder);
        read_id(&controller, 0, result);
        CHECK(result[3] == cylinder);
    }
    puts("PASS: FDC 1/18/36-sector DMA data, terminal-count bounds, seek status and READ ID cylinder");
    return 0;
}