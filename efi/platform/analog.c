// Analog audio (headphone jack): Realtek RT5672 codec on I2C2, fed over I2S
// from the LPE's SSP2, MCLK 19.2 MHz from the PMC's platform clock 3.
//
// Linux drives this through the SST DSP and its firmware; here the DSP is
// held stalled and the pieces are programmed directly: the codec is I2S
// master (its PLL makes 512 fs from MCLK), SSP2 is slave and its FIFO is
// filled by the sound core from a ring the mixer writes.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "../include/efi.h"

void wyse_udelay(uint32_t us);

#define I2C2_BASE   0x91524000u     // DesignWare I2C, ACPI \_SB.PCI0.I2C2
#define LPE_BASE    0x91000000u     // SST LPE: SSP at +0xA0000, SHIM at +0x140000
#define RT5672_ADDR 0x1c

// DesignWare I2C
#define IC_CON          0x00
#define IC_TAR          0x04
#define IC_DATA_CMD     0x10
#define IC_SS_SCL_HCNT  0x14
#define IC_SS_SCL_LCNT  0x18
#define IC_RAW_INTR     0x34
#define IC_CLR_INTR     0x40
#define IC_CLR_TX_ABRT  0x54
#define IC_ENABLE       0x6c
#define IC_STATUS       0x70
#define IC_TXFLR        0x74
#define IC_RXFLR        0x78
#define IC_TX_ABRT_SRC  0x80
#define IC_ENABLE_STAT  0x9c
#define IC_COMP_TYPE    0xfc
#define LPSS_CLOCK      0x800
#define LPSS_RESETS     0x804

static uint32_t pmc_base;

#ifndef WYSE_ANALOG_TEST
static inline uint32_t r32(uint32_t a) { return *(volatile uint32_t *)(uint64_t)a; }
static inline void w32(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uint64_t)a = v; }
#endif

static inline uint32_t pci_read(int bus, int dev, int fn, int reg) {
    uint32_t addr = 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xfc);
    __asm__ volatile("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xcf8));
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xcfc));
    return v;
}

static inline void pci_write(int bus, int dev, int fn, int reg, uint32_t v) {
    uint32_t addr = 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xfc);
    __asm__ volatile("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xcf8));
    __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"((uint16_t)0xcfc));
}

static bool i2c_wait_idle(void) {
    for (int i = 0; i < 20000; i++) {
        uint32_t st = r32(I2C2_BASE + IC_STATUS);
        if (!(st & (1 << 5)) && (st & (1 << 2))) return true;   // not active, TX FIFO empty
        wyse_udelay(5);
    }
    return false;
}

static bool i2c_init(void) {
    uint32_t comp = r32(I2C2_BASE + IC_COMP_TYPE);
    printf("analog: i2c2 comp %08x clock %08x resets %08x\n", comp, r32(I2C2_BASE + LPSS_CLOCK),
           r32(I2C2_BASE + LPSS_RESETS));
    w32(I2C2_BASE + LPSS_RESETS, r32(I2C2_BASE + LPSS_RESETS) | 3);    // out of reset
    comp = r32(I2C2_BASE + IC_COMP_TYPE);
    if (comp != 0x44570140) {
        printf("analog: i2c2 not a DesignWare controller (%08x)\n", comp);
        return false;
    }
    w32(I2C2_BASE + IC_ENABLE, 0);
    for (int i = 0; i < 100 && (r32(I2C2_BASE + IC_ENABLE_STAT) & 1); i++) wyse_udelay(10);
    w32(I2C2_BASE + IC_CON, 0x63);              // master, standard speed, restart, slave off
    w32(I2C2_BASE + IC_SS_SCL_HCNT, 532);       // 133 MHz input: 4.0 us high
    w32(I2C2_BASE + IC_SS_SCL_LCNT, 798);       //                6.0 us low
    w32(I2C2_BASE + IC_TAR, RT5672_ADDR);
    w32(I2C2_BASE + IC_ENABLE, 1);
    return true;
}

// RT5672 registers: 8-bit address, 16-bit big-endian data
static bool codec_read(uint8_t reg, uint16_t *val) {
    r32(I2C2_BASE + IC_CLR_INTR);
    w32(I2C2_BASE + IC_DATA_CMD, reg);
    w32(I2C2_BASE + IC_DATA_CMD, 0x100);
    w32(I2C2_BASE + IC_DATA_CMD, 0x100 | 0x200);    // read, stop
    for (int i = 0; i < 20000; i++) {
        if (r32(I2C2_BASE + IC_RAW_INTR) & (1 << 6)) {
            printf("analog: i2c abort, source %08x\n", r32(I2C2_BASE + IC_TX_ABRT_SRC));
            r32(I2C2_BASE + IC_CLR_TX_ABRT);
            return false;
        }
        if (r32(I2C2_BASE + IC_RXFLR) >= 2) {
            uint8_t hi = (uint8_t)r32(I2C2_BASE + IC_DATA_CMD), lo = (uint8_t)r32(I2C2_BASE + IC_DATA_CMD);
            *val = (uint16_t)(hi << 8 | lo);
            return true;
        }
        wyse_udelay(5);
    }
    printf("analog: i2c read timeout\n");
    return false;
}

bool codec_write(uint8_t reg, uint16_t val) {
    r32(I2C2_BASE + IC_CLR_INTR);
    w32(I2C2_BASE + IC_DATA_CMD, reg);
    w32(I2C2_BASE + IC_DATA_CMD, val >> 8);
    w32(I2C2_BASE + IC_DATA_CMD, (val & 0xff) | 0x200);
    if (!i2c_wait_idle()) return false;
    if (r32(I2C2_BASE + IC_RAW_INTR) & (1 << 6)) {
        r32(I2C2_BASE + IC_CLR_TX_ABRT);
        return false;
    }
    return true;
}

static void analog_probe(void) {
    pmc_base = pci_read(0, 0x1f, 0, 0x44) & 0xfffffe00;
    printf("analog: pmc %08x, func_dis %08x, pss %08x, d3_sts %08x\n", pmc_base, r32(pmc_base + 0x34),
           r32(pmc_base + 0x98), r32(pmc_base + 0xa0));
    for (int i = 0; i < 6; i++) printf("analog: plt_clk%d %08x\n", i, r32(pmc_base + 0x60 + 4 * i));
    printf("analog: lpe shim csr %08x clkctl %08x, ssp2 sscr0 %08x\n", r32(LPE_BASE + 0x140000),
           r32(LPE_BASE + 0x140078), r32(LPE_BASE + 0xa2000));

    // MCLK on (plt_clk3 forced on, 19.2 MHz) before the codec is touched
    uint32_t c3 = r32(pmc_base + 0x6c);
    w32(pmc_base + 0x6c, (c3 & ~0x7u) | 0x4 | 0x1);
    printf("analog: plt_clk3 now %08x\n", r32(pmc_base + 0x6c));

    if (!i2c_init()) return;
    uint16_t id1 = 0, id2 = 0, r0 = 0;
    bool ok = codec_read(0xfe, &id2) && codec_read(0xfd, &id1) && codec_read(0x00, &r0);
    printf("analog: rt5672 %s: vendor id1 %04x id2 %04x reg0 %04x\n", ok ? "answers" : "silent", id1, id2, r0);
}

//--------------------------------------------------------------------+
// codec set-up: playback DAC1 -> HPOVOL -> HPO mixer -> headphone amp, I2S1 master
//--------------------------------------------------------------------+

#define SSP2_BASE   (LPE_BASE + 0xa2000)
#define SSCR0       0x00
#define SSCR1       0x04
#define SSSR        0x08
#define SSDR        0x10
#define SSPSP       0x2c
#define SSTSA       0x30
#define SSRSA       0x34

static bool codec_update(uint8_t reg, uint16_t mask, uint16_t val) {
    uint16_t v;
    if (!codec_read(reg, &v)) return false;
    return codec_write(reg, (uint16_t)((v & ~mask) | (val & mask)));
}

static bool codec_priv_write(uint8_t reg, uint16_t val) {
    return codec_write(0x6a, reg) && codec_write(0x6c, val);
}

static bool codec_init(void) {
    bool ok = true;
    // reset, as the Linux driver does on probe
    ok &= codec_write(0x00, 0);
    ok &= codec_update(0x63, (1 << 7) | (1 << 6) | (1 << 4), 1 << 4);
    wyse_udelay(100000);
    ok &= codec_write(0x00, 0);
    uint16_t ver = 0;
    codec_read(0xfd, &ver);
    ok &= codec_write(0xc2, ver >= 4 ? 0x0980 : 0x0d00);     // GPIO_CTRL3
    ok &= codec_priv_write(0x14, 0x9a8a);                     // init_list patches
    ok &= codec_priv_write(0x38, 0x1fe1);
    ok &= codec_priv_write(0x3d, 0x3640);
    ok &= codec_write(0x8a, 0x0123);
    ok &= codec_update(0xfa, 1 << 3, 1 << 3);                 // DIG_MISC: MCLK detect

    // clocks: PLL1 from MCLK 19.2 MHz -> 24.576 MHz = 512 fs at 48 kHz (N 30, K 3, M 3)
    ok &= codec_write(0x81, (30 << 7) | 3);
    ok &= codec_write(0x82, 3 << 12);
    ok &= codec_update(0x80, (3 << 14) | (7 << 11), (1 << 14));    // sysclk = PLL1, PLL source = MCLK
    ok &= codec_update(0x64, 1 << 9, 1 << 9);                 // PLL power
    ok &= codec_update(0x73, 7 << 12, 1 << 12);               // I2S1 pre-divider /2: 256 fs
    // I2S1: master, 16-bit, I2S format
    ok &= codec_update(0x70, (1 << 15) | (3 << 2) | 3, 0);

    // bias (set_bias_level PREPARE)
    ok &= codec_update(0x63, (1 << 15) | (1 << 13) | (1 << 11) | (1 << 4), (1 << 15) | (1 << 13) | (1 << 11) | (1 << 4));
    wyse_udelay(10000);
    ok &= codec_update(0x63, (1 << 14) | (1 << 3), (1 << 14) | (1 << 3));
    ok &= codec_update(0x91, (1 << 11) | (1 << 10), 0);
    ok &= codec_update(0xfa, 1, 1);
    ok &= codec_update(0x63, 7, 5);

    // digital path power: I2S1, DAC L1/R1, stereo DAC filter; HPO volume, mic-det supply
    ok &= codec_update(0x61, (1 << 15) | (1 << 12) | (1 << 11), (1 << 15) | (1 << 12) | (1 << 11));
    ok &= codec_update(0x62, 1 << 11, 1 << 11);
    ok &= codec_update(0x66, (1 << 11) | (1 << 10) | (1 << 5), (1 << 11) | (1 << 10) | (1 << 5));
    // routing (mute bits are active high): DAC1 source IF1, DAC1 mixer, stereo DAC mixer, HPO mixer
    ok &= codec_update(0x29, (1 << 14) | (1 << 6) | (3 << 8) | (3 << 10), 0);
    ok &= codec_update(0x2a, (1 << 14) | (1 << 6), 0);
    // HPO MIX has a direct DAC1 input which bypasses HP_VOL (02h).
    // Mute that bypass and DAC2, enable HPVOL, and route DAC L1/R1 into
    // HPOVOL MIXL/R while keeping the analog INL/INR inputs muted.
    // RT5670/72 HPMIX 45h mute bits are active high (Linux rt5670.h).
    ok &= codec_update(0x45, 0xe00f, 0xa00a);
    ok &= codec_write(0x19, 0xafaf);                          // DAC1 digital volume 0 dB

    // headphone amp power-up and unmute (hp_power_event / hp_event)
    ok &= codec_update(0x63, (1 << 7) | (1 << 6), (1 << 7) | (1 << 6));
    ok &= codec_update(0x91, 3 << 8, 2 << 8);
    ok &= codec_update(0xfb, 0x0400, 0x0400);
    ok &= codec_update(0x63, (1 << 5) | (1 << 14) | (1 << 3), (1 << 5) | (1 << 14) | (1 << 3));
    ok &= codec_write(0x8f, 0x3100);
    ok &= codec_write(0x8e, 0x8009);
    ok &= codec_priv_write(0x77, 0x9f00);
    wyse_udelay(20000);
    ok &= codec_write(0x8e, 0x8019);
    ok &= codec_priv_write(0x37, 0xb400);
    ok &= codec_write(0x90, 0x0772);
    ok &= codec_write(0x8e, 0x805d);
    ok &= codec_write(0x8e, 0x831d);
    ok &= codec_update(0xfb, 0x0300, 0x0300);
    ok &= codec_write(0x02, 0x0000);                          // HP volume +12 dB (full), unmuted
    wyse_udelay(80000);
    ok &= codec_write(0x8e, 0x8019);
    return ok;
}

//--------------------------------------------------------------------+
// SSP2: I2S slave, two 16-bit slots, filled by the CPU
//--------------------------------------------------------------------+

#define LPE_CFG     0x91528000u     // the LPE's PCI configuration space, ACPI mode

static void ssp_init(void) {
    // the LPE hides from PCI in ACPI mode; its config space is memory mapped:
    // power it to D0 and let it decode memory before touching SSP or SHIM
    printf("analog: lpe cfg id %08x cmd %08x bar0 %08x bar1 %08x pmcsr %08x\n", r32(LPE_CFG), r32(LPE_CFG + 4),
           r32(LPE_CFG + 0x10), r32(LPE_CFG + 0x14), r32(LPE_CFG + 0x84));
    // the LPE as a PCI function (00:15.0), in case the firmware has not hidden it yet
    printf("analog: pci 00:15.0 id %08x cmd %08x bar0 %08x bar1 %08x bar2 %08x pm %08x\n", pci_read(0, 0x15, 0, 0),
           pci_read(0, 0x15, 0, 4), pci_read(0, 0x15, 0, 0x10), pci_read(0, 0x15, 0, 0x14),
           pci_read(0, 0x15, 0, 0x18), pci_read(0, 0x15, 0, 0x84));
    for (int d = 0x10; d < 0x20; d++) {
        uint32_t id = pci_read(0, d, 0, 0);
        if (id != 0xffffffff) printf("analog: pci 00:%02x.0 %08x class %08x\n", d, id, pci_read(0, d, 0, 8));
    }
    // still a visible PCI function at this stage of the firmware, with memory
    // decoding off: D0, then memory space and bus mastering on
    pci_write(0, 0x15, 0, 0x84, pci_read(0, 0x15, 0, 0x84) & ~3u);
    wyse_udelay(10000);
    pci_write(0, 0x15, 0, 4, (pci_read(0, 0x15, 0, 4) & 0xffff) | 6);
    printf("analog: lpe now cmd %08x pmcsr %08x, shim csr %08x\n", r32(LPE_CFG + 4), r32(LPE_CFG + 0x84),
           r32(LPE_BASE + 0x140000));
    w32(LPE_BASE + 0x140000, r32(LPE_BASE + 0x140000) | (1 << 2));    // keep the DSP stalled
    w32(SSP2_BASE + SSCR0, 0);
    w32(SSP2_BASE + SSCR1, (1u << 25) | (1u << 24) | (1u << 30));    // clock and frame from the codec, TX tristate
    w32(SSP2_BASE + SSPSP, (1u << 4) | (16u << 16));    // start delay 1, frame 16 bits, active low (I2S)
    w32(SSP2_BASE + SSTSA, 3);
    w32(SSP2_BASE + SSRSA, 0);
    w32(SSP2_BASE + SSCR0, (1u << 31) | (3u << 4) | 15 | (1u << 24));  // network mode, PSP, 16 bits, 2 slots
    w32(SSP2_BASE + SSCR0, r32(SSP2_BASE + SSCR0) | (1u << 7));       // enable
}

//--------------------------------------------------------------------+
// ring between the mixer (core 1) and the SSP FIFO (core 3)
//--------------------------------------------------------------------+

// The SSP FIFO holds 16 words, 8 frames or 167 us: too little to fill between
// mixer passes, so a core of its own keeps it topped up. The mixer runs on
// the HDMI clock, the FIFO drains on the codec's; the small difference is
// taken up a frame at a time (one dropped or repeated), never in bursts.

#define ARING 8192      // frames (170 ms)
static int16_t aring[ARING * 2];
static volatile uint32_t a_head, a_tail;
static bool analog_on;
static volatile bool analog_core;
uint32_t g_analog_fed, g_analog_drop, g_analog_starve, g_analog_underrun;

uint32_t analog_level(void) { return (a_head + ARING - a_tail) % ARING; }

void analog_put(const int16_t *frames, int n) {
    if (!analog_on) return;
    uint32_t head = a_head;
    for (int i = 0; i < n; i++) {
        uint32_t level = (head + ARING - a_tail) % ARING;
        // running ahead of the codec: leave out one frame in 256
        if (level > ARING / 2 && (i & 255) == 128) { g_analog_drop++; continue; }
        uint32_t next = (head + 1) % ARING;
        if (next == a_tail) { g_analog_drop++; continue; }
        aring[2 * head] = frames[2 * i];
        aring[2 * head + 1] = frames[2 * i + 1];
        head = next;
    }
    __atomic_store_n(&a_head, head, __ATOMIC_RELEASE);
}

// Top the SSP FIFO up, a word (left, then right) at a time while it has room.
// The SSP's frame format, switchable while running for listening tests
// ("sspmode" on the control port): 0 two 16-bit slots a frame (network mode),
// 1 one 32-bit slot a frame carrying both channels (network mode), which
// doubles what the 16-word FIFO holds to 16 frames (333 us) - enough to ride
// out the firmware's SMIs (up to 256 us).
static volatile int ssp_mode_wanted = 1;     // 1: measured 0 clicks a second as the mouse moves, against 130 with 0
static int ssp_mode;

void analog_set_ssp_mode(int mode) { ssp_mode_wanted = mode; }

static void apply_ssp_mode(void) {
    int m = ssp_mode_wanted;
    w32(SSP2_BASE + SSCR0, r32(SSP2_BASE + SSCR0) & ~(1u << 7));
    if (m == 1) {
        w32(SSP2_BASE + SSTSA, 1);
        w32(SSP2_BASE + SSCR0, (1u << 31) | (1u << 20) | (3u << 4) | 15);   // network, 1 slot, 32 bits
    } else {
        w32(SSP2_BASE + SSTSA, 3);
        w32(SSP2_BASE + SSCR0, (1u << 31) | (3u << 4) | 15 | (1u << 24));   // network, 2 slots, 16 bits
    }
    w32(SSP2_BASE + SSCR0, r32(SSP2_BASE + SSCR0) | (1u << 7));
    ssp_mode = m;
    printf("analog: SSP2 mode %d, cr0 %08x tsa %x\n", m, r32(SSP2_BASE + SSCR0), r32(SSP2_BASE + SSTSA));
}

uint32_t g_analog_gap_max;       // longest pause between feeds, on either feeder core

void analog_feed(void) {
    static int16_t last[2];
    static int half;
    static uint64_t previous_feed;
    if (!analog_on) return;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t now = (uint64_t)hi << 32 | lo;
    uint64_t gap = now - previous_feed;
    if (previous_feed && gap > g_analog_gap_max)
        g_analog_gap_max = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
    previous_feed = now;
    if (ssp_mode != ssp_mode_wanted) {
        apply_ssp_mode();
        half = 0;
    }
    if (ssp_mode == 1) {
        uint32_t sr1 = r32(SSP2_BASE + SSSR);
        if (sr1 & (1u << 21)) {
            w32(SSP2_BASE + SSSR, 1u << 21);
            g_analog_underrun++;
        }
        for (int k = 0; k < 16 && (r32(SSP2_BASE + SSSR) & (1 << 2)); k++) {
            uint32_t tail = a_tail;
            if (tail == __atomic_load_n(&a_head, __ATOMIC_ACQUIRE)) {
                last[0] -= last[0] >> 6;
                last[1] -= last[1] >> 6;
                g_analog_starve++;
            } else {
                last[0] = aring[2 * tail];
                last[1] = aring[2 * tail + 1];
                __atomic_store_n(&a_tail, (tail + 1) % ARING, __ATOMIC_RELEASE);
                g_analog_fed++;
            }
            w32(SSP2_BASE + SSDR, (uint32_t)(uint16_t)last[0] << 16 | (uint16_t)last[1]);
        }
        return;
    }
    uint32_t sr = r32(SSP2_BASE + SSSR);
    if (sr & (1u << 21)) {              // the FIFO ran dry
        w32(SSP2_BASE + SSSR, 1u << 21);
        g_analog_underrun++;
    }
    for (int k = 0; k < 16 && (r32(SSP2_BASE + SSSR) & (1 << 2)); k++) {
        if (!half) {
            uint32_t tail = a_tail;
            if (tail == __atomic_load_n(&a_head, __ATOMIC_ACQUIRE)) {
                // nothing mixed: repeat the last frame (no click) and let it decay
                last[0] -= last[0] >> 6;
                last[1] -= last[1] >> 6;
                g_analog_starve++;
            } else {
                last[0] = aring[2 * tail];
                last[1] = aring[2 * tail + 1];
                __atomic_store_n(&a_tail, (tail + 1) % ARING, __ATOMIC_RELEASE);
                g_analog_fed++;
            }
        }
        w32(SSP2_BASE + SSDR, (uint16_t)last[half]);
        half ^= 1;
    }
}

// Headphone volume in 1.5 dB steps: 0 is full (+12 dB), 8 is 0 dB, 39 is -46.5 dB;
// a negative step mutes.
void analog_set_volume(int step) {
    if (!analog_on) return;
    if (step > 39) step = 39;
    uint16_t wanted = step < 0 ? (0x8080 | 39 << 8 | 39) : (uint16_t)(step << 8 | step);
    uint16_t actual = 0;
    if (!codec_write(0x02, wanted) || !codec_read(0x02, &actual) || actual != wanted) {
        printf("analog: headphone volume write/readback failed (wanted %04x, read %04x)\n", wanted, actual);
        return;
    }
    if (step < 0) {
        printf("analog: headphones muted (HP_VOL %04x)\n", actual);
        return;
    }
    int db10 = 120 - 15 * step, mag = db10 < 0 ? -db10 : db10;
    printf("analog: headphone volume %d (%c%d.%d dB)\n", step, db10 < 0 ? '-' : '+', mag / 10, mag % 10);
}

bool analog_owns_core(void) { return analog_core; }

void mp_hang_watch(void);
extern volatile uint64_t g_core_heartbeat[4];
int gmsynth_step(void);

static void analog_core_run(void) {
    analog_core = true;
    for (;;) {
        analog_feed();
        gmsynth_step();         // General MIDI renders ahead of the mixer, as on X86Pi's core 3
        mp_hang_watch();
        g_core_heartbeat[3]++;
        __asm__ volatile("pause");
    }
}

bool mp_start(int core, void (*fn)(void));

// The feeder on core 3; without it the sound core feeds between passes.
void analog_start(void) {
    if (!analog_on) return;
    // how deep the transmit FIFO is: it is empty now, so count the samples it takes
    int depth = 0;
    while (depth < 1024 && (r32(SSP2_BASE + SSSR) & (1 << 2))) {
        w32(SSP2_BASE + SSDR, 0);
        depth++;
    }
    printf("analog: SSP2 transmit FIFO takes %d samples\n", depth);
    mp_start(3, analog_core_run);
}

bool analog_init(void) {
    analog_probe();
    if (!codec_init()) {
        printf("analog: codec set-up failed\n");
        return false;
    }
    ssp_init();
    uint16_t pll = 0, glb = 0, pa = 0, hp_mix = 0;
    codec_read(0x80, &glb);
    codec_read(0x81, &pll);
    codec_read(0x63, &pa);
    codec_read(0x45, &hp_mix);
    printf("analog: headphone route HPMIX %04x (DAC1 via HPVOL, direct bypass muted)\n", hp_mix);
    printf("analog: codec ready (glb %04x pll %04x anlg1 %04x), ssp2 cr0 %08x cr1 %08x sr %08x\n", glb, pll, pa,
           r32(SSP2_BASE + SSCR0), r32(SSP2_BASE + SSCR1), r32(SSP2_BASE + SSSR));
    analog_on = true;
    return true;
}
