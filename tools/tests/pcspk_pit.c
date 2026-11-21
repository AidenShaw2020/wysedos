#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "i8254.h"
#include "pcspk.h"

static uint32_t now_us;
static unsigned irq_edges;
uint32_t get_uticks(void) { return now_us; }
static void count_irq(void *opaque, int irq, int level) {
    (void)opaque;
    (void)irq;
    irq_edges += level != 0;
}
static uint8_t speaker_status(PITState *pit) {
    i8254_ioport_write(pit, 3, 0xe8);
    return i8254_ioport_read(pit, 2);
}
int main(void) {
    PITState *pit = i8254_init(0, NULL, count_irq);
    PCSpkState *speaker = pcspk_init(pit);
    assert(pit && speaker);
    i8254_ioport_write(pit, 3, 0xb6);
    pcspk_ioport_write(speaker, 3);
    for (unsigned i = 0; i < 3000; ++i) assert(pcspk_sample(speaker) == 0);
    assert((speaker_status(pit) & 0xc0) == 0xc0);
    i8254_ioport_write(pit, 2, 0xa9);
    assert((speaker_status(pit) & 0xc0) == 0xc0);
    assert(pcspk_sample(speaker) == 0);
    i8254_ioport_write(pit, 2, 0x04);
    assert(!(speaker_status(pit) & 0x40));
    bool positive = false, negative = false;
    for (unsigned i = 0; i < 120; ++i) {
        int16_t sample = pcspk_sample(speaker);
        positive |= sample > 0;
        negative |= sample < 0;
    }
    assert(positive && negative);
    i8254_ioport_write(pit, 3, 0xb6);
    assert(pcspk_sample(speaker) == 0);
    i8254_ioport_write(pit, 3, 0x36);
    now_us += 200000;
    i8254_update_irq(pit);
    assert(!irq_edges);
    i8254_ioport_write(pit, 0, 0);
    i8254_ioport_write(pit, 0, 0);
    now_us += 60000;
    i8254_update_irq(pit);
    assert(irq_edges == 1);
    free(speaker);
    free(pit);
    return 0;
}
