/* Actual codec initialization and volume writes, with a register-level I2C
 * device and an independent check of the audible headphone signal paths.
 * clang -O2 -ffunction-sections tools/tests/analog_volume.c '-Wl,/opt:ref'
 *       -o work/analog_volume.exe
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define WYSE_ANALOG_TEST 1
static uint32_t r32(uint32_t a);
static void w32(uint32_t a, uint32_t v);
#include "../../efi/platform/analog.c"

#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static uint16_t regs[256];
static uint8_t address, high, rx[2];
static int tx_count, rx_count, rx_pos;
static bool reject_write;

void wyse_udelay(uint32_t us) { (void)us; }

static void reset_codec(void) {
    for (int i = 0; i < 256; i++) regs[i] = 0;
    regs[0x02] = 0x8888;
    regs[0x45] = 0xe00f;
    regs[0xfd] = 4;
}

static uint32_t r32(uint32_t a) {
    CHECK(a >= I2C2_BASE && a < I2C2_BASE + 0x100);
    switch (a - I2C2_BASE) {
    case IC_STATUS: return 1 << 2; /* Idle and transmit FIFO empty. */
    case IC_RAW_INTR: return 0;
    case IC_CLR_INTR: return 0;
    case IC_RXFLR: return rx_count - rx_pos;
    case IC_DATA_CMD: CHECK(rx_pos < rx_count); return rx[rx_pos++];
    default: CHECK(false); return 0;
    }
}

static void w32(uint32_t a, uint32_t v) {
    CHECK(a == I2C2_BASE + IC_DATA_CMD);
    if (v & 0x100) {
        CHECK(tx_count == 1);
        if (!(v & 0x200)) {
            rx[0] = regs[address] >> 8; rx[1] = regs[address] & 255;
            rx_count = 2; rx_pos = 0;
        } else tx_count = 0;
    } else if (tx_count == 0) {
        address = v & 255; tx_count = 1;
    } else if (tx_count == 1) {
        high = v & 255; tx_count = 2;
    } else {
        CHECK(v & 0x200);
        if (!reject_write) {
            if (address == 0) reset_codec();
            else regs[address] = (uint16_t)(high << 8 | (v & 255));
        }
        tx_count = 0;
    }
}

/* Whether DAC1 reaches the amp bypassing the volume, and whether each
 * channel can reach it through its powered volume stage. These decode
 * the hardware graph, rather than asserting one magic initialization word. */
static bool direct_dac(void) { return !(regs[0x45] & (1 << 13)); }
static bool volume_path(int right) {
    unsigned power = 1u << (right ? 10 : 11);
    unsigned source_mute = 1u << (right ? 2 : 0);
    return (regs[0x66] & power) && !(regs[0x45] & (1 << 14)) &&
           !(regs[0x45] & source_mute);
}

int main(void) {
    reset_codec();
    CHECK(codec_init());
    CHECK(!direct_dac());
    CHECK(volume_path(0) && volume_path(1));
    CHECK((regs[0x45] & ((1 << 15) | (1 << 3) | (1 << 1))) ==
                       ((1 << 15) | (1 << 3) | (1 << 1)));
    analog_on = true;
    analog_set_volume(0); CHECK(regs[0x02] == 0);
    analog_set_volume(8); CHECK(regs[0x02] == 0x0808);
    analog_set_volume(30); CHECK(regs[0x02] == 0x1e1e);
    analog_set_volume(100); CHECK(regs[0x02] == 0x2727);
    analog_set_volume(-1); CHECK((regs[0x02] & 0x8080) == 0x8080);
    analog_set_volume(0); CHECK(regs[0x02] == 0); /* Recover after mute. */
    reject_write = true;
    analog_set_volume(8); CHECK(regs[0x02] == 0);
    CHECK(!direct_dac() && volume_path(0) && volume_path(1));
    puts("PASS: DAC1 uses powered stereo headphone volume without a direct bypass; attenuation, mute/unmute and readback");
    return 0;
}
