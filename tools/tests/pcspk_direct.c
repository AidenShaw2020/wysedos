#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "../../pc/pcspk.c"

static uint32_t now_us;
uint32_t get_uticks(void) { return now_us; }
struct PITState { int mode, gate, count, out; };
int pit_get_out(PITState *pit, int channel) { (void)channel; return pit->out; }
int pit_get_gate(PITState *pit, int channel) { (void)channel; return pit->gate; }
int pit_get_initial_count(PITState *pit, int channel) { (void)channel; return pit->count; }
int pit_get_mode(PITState *pit, int channel) { (void)channel; return pit->mode; }
void pit_set_gate(PITState *pit, int channel, int value) { (void)channel; pit->gate = value; }

int main(void)
{
    PITState pit = {3, 0, 1, 0};
    PCSpkState *speaker = pcspk_init(&pit);
    assert(speaker);
    assert(pcspk_sample(NULL) == 0);
    assert(pcspk_sample(speaker) == 0);
    uint32_t stamps[128];
    for (unsigned index = 0; index < 128; index++) {
        const unsigned data = index & 1;
        now_us = index * 100;
        pcspk_ioport_write(speaker, data << 1);
        assert(pit.gate == 0);
        uint32_t frame;
        assert(audioclock_read(&speaker->clock, &frame));
        stamps[index] = frame + AUDIOCLOCK_STAMP_LEAD(0);
    }
    unsigned expected = 0;
    int32_t expected_dc = 0;
    unsigned transitions = 0;
    while ((uint32_t)speaker->consumed <= stamps[127] + 16) {
        const uint32_t frame = (uint32_t)speaker->consumed;
        while (expected < 127 && frame >= stamps[expected + 1]) {
            expected++;
            transitions++;
        }
        int sample = (expected & 1) ? (int)PCSPK_BL_PEAK : 0;
        expected_dc += ((sample << 8) - expected_dc) >> 9;
        assert(pcspk_sample(speaker) == sample - (expected_dc >> 8));
    }
    assert(transitions == 127);
    assert(speaker->edge_read == speaker->edge_write);
    now_us = (uint32_t)(speaker->consumed * 1000000 / SOUND_FREQUENCY);
    pcspk_ioport_write(speaker, 0);
    for (unsigned index = 0; index < 8192; index++) pcspk_sample(speaker);
    assert(pcspk_sample(speaker) >= -2 && pcspk_sample(speaker) <= 2);
    free(speaker);
    speaker = pcspk_init(&pit);
    assert(speaker);
    now_us = 0;
    assert(pcspk_sample(speaker) == 0);
    pit.count = 1193;
    pcspk_ioport_write(speaker, 3);
    for (unsigned index = 0; index < 2048; index++) pcspk_sample(speaker);
    bool positive = false, negative = false;
    for (unsigned index = 0; index < 128; index++) {
        int16_t sample = pcspk_sample(speaker);
        positive |= sample > 0;
        negative |= sample < 0;
    }
    assert(positive && negative);
    pcspk_ioport_write(speaker, 1);
    for (unsigned index = 0; index < 2048; index++) pcspk_sample(speaker);
    assert(pcspk_sample(speaker) == 0);
    pit.mode = 2;
    pcspk_ioport_write(speaker, 3);
    assert(pcspk_sample(speaker) == 0);
    pcspk_ioport_write(speaker, 0);
    for (unsigned index = 0; index < 2048; index++) pcspk_sample(speaker);
    pcspk_ioport_write(speaker, 2);
    bool direct_mode2 = false;
    for (unsigned index = 0; index < 2048; index++)
        direct_mode2 |= pcspk_sample(speaker) > 1024;
    assert(direct_mode2);
    pcspk_ioport_write(speaker, 0);
    for (unsigned index = 0; index < 8192; index++) pcspk_sample(speaker);
    pit.mode = 0;
    pit.out = 1;
    pcspk_ioport_write(speaker, 2);
    bool direct_mode0 = false;
    for (unsigned index = 0; index < 2048; index++)
        direct_mode0 |= pcspk_sample(speaker) > 1024;
    assert(direct_mode0);
    pit.out = 0;
    for (unsigned index = 0; index < 8192; index++) pcspk_sample(speaker);
    assert(pcspk_sample(speaker) >= -2 && pcspk_sample(speaker) <= 2);
    free(speaker);
    pit.mode = 3;
    speaker = pcspk_init(&pit);
    assert(speaker);
    now_us = 0;
    assert(pcspk_sample(speaker) == 0);
    for (unsigned index = 0; index < PCSPK_EDGE_SLOTS + 8; index++)
        pcspk_ioport_write(speaker, (index & 1) << 1);
    pcspk_ioport_write(speaker, 0);
    assert(speaker->overflow_write != 0);
    assert(pcspk_sample(speaker) == 0);
    assert(speaker->edge_read == speaker->edge_write);
    assert(speaker->render_bits == 0);
    pcspk_ioport_write(speaker, 2);
    assert(speaker->edge_write - speaker->edge_read == 1);
    for (unsigned index = 0; index < 2048; index++) pcspk_sample(speaker);
    assert(speaker->render_bits == 2);
    free(speaker);
    puts("PASS actual PC speaker: 127 timed bit edges survive burst mixing; DC decays; mute and PIT tone unchanged");
    puts("PASS inherited PIT mode 2: gate-low direct samples remain audible");
    puts("PASS inherited PIT mode 0: terminal OUT permits samples; low OUT remains silent");
    puts("PASS queue overflow: final mute retained and ring reuse succeeds");
    return 0;
}