/* Actual SSP feeder with an overlapping service attempt at the MMIO seam. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define WYSE_ANALOG_TEST 1
static uint32_t r32(uint32_t a);
static void w32(uint32_t a, uint32_t v);
#include "../../efi/platform/analog.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static unsigned writes, overlap, nested, capacity;
static uint32_t output[32];
volatile uint64_t g_core_heartbeat[4];
void wyse_udelay(uint32_t us) { (void)us; abort(); }
bool mp_start(int core, void (*fn)(void)) { (void)core; (void)fn; abort(); }
int gmsynth_step(void) { abort(); }
void mp_hang_watch(void) { abort(); }
static uint32_t r32(uint32_t a) {
    if (a == SSP2_BASE + SSSR) return writes < capacity ? 1u << 2 : 0;
    return 0;
}
static void w32(uint32_t a, uint32_t v) {
    if (a != SSP2_BASE + SSDR) return;
    CHECK(writes < capacity);
    output[writes++] = v;
    if (overlap && !nested) {
        nested = 1;
        analog_feed(); /* Other core offers service before this pass finishes. */
        nested = 0;
    }
}
int main(void) {
    analog_on = true;
    for (int mode = 0; mode <= 1; mode++) {
        int16_t samples[64];
        a_head = a_tail = 0;
        for (int i=0;i<32;i++) { samples[2*i]=(int16_t)(i+100); samples[2*i+1]=(int16_t)(-i-200); }
        analog_put(samples,32);
        ssp_mode = ssp_mode_wanted = mode;
        writes=0; capacity=16; overlap=1;
        analog_feed();
        CHECK(writes == 16);
        CHECK(a_tail == (mode ? 16u : 8u));
        for (unsigned i=0;i<16;i++) {
            uint32_t expected = mode ? (uint32_t)(uint16_t)samples[2*i] << 16 | (uint16_t)samples[2*i+1]
                                     : (uint16_t)samples[i];
            CHECK(output[i] == expected);
        }
        /* An empty FIFO must not retain ownership on the packed-mode return. */
        overlap=0; analog_feed();
        writes=0; analog_feed();
        CHECK(writes == 16 && a_tail == (mode ? 32u : 16u));
    }
    puts("PASS overlapping SSP service preserves stereo order and releases ownership in both modes");
    return 0;
}
