/* Dell Wyse 3040 hardware mirror for two HDMI/DVI (TMDS) sinks.
 *
 * The firmware can train either output alone. Capture port D's trained PHY
 * settings, then bring it back with pipe B's timing and framebuffer while
 * GOP owns port B. PHY and PLL sequence follows Intel i915's Cherryview
 * DPIO and display power code. Native DisplayPort AUX training is separate.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

extern void wyse_udelay(uint32_t);

#define DISPLAY 0x180000u
#define SB_COMMAND 0x2100u
#define SB_DATA 0x2104u
#define SB_ADDRESS 0x2108u
#define DPIO_B 0x1au
#define DPIO_D 0x12u
#define PUNIT 0x04u
#define PHY_CONTROL 0x60100u
#define PHY_STATUS 0x60104u
#define HDMI_B 0x61140u
#define HDMI_D 0x6116cu
#define DPLL_C 0x6030u
#define PIPE_B 0x71008u
#define PIPE_C 0x74008u
#define ENABLE (1u << 31)
#define ACTIVE (1u << 30)
#define PLL_LOCK (1u << 15)
#define PHY_D_WELL (3u << 24)
#define PHY_D_RESET (1u << 1)
#define PHY_D_CONFIGURATION (0x20000000u | (3u << 25) | (15u << 19) | (7u << 8) | PHY_D_RESET)
#define PHY_D_GOOD (1u << 30)

static volatile uint32_t *display;

static uint32_t read_config(unsigned offset)
{
    uint32_t data;
    uint32_t address = 0x80001000u | offset;
    __asm__ volatile("outl %0, %1" :: "a"(address), "Nd"((uint16_t)0xcf8));
    __asm__ volatile("inl %1, %0" : "=a"(data) : "Nd"((uint16_t)0xcfc));
    return data;
}

static uint32_t rd(unsigned reg) { return display[reg / 4]; }
static void wr(unsigned reg, uint32_t data)
{
    display[reg / 4] = data;
    (void)display[reg / 4];
}

static bool wait_bits(unsigned reg, uint32_t mask, uint32_t value, unsigned us)
{
    for (unsigned elapsed = 0; elapsed < us; elapsed += 10) {
        if ((rd(reg) & mask) == value) return true;
        wyse_udelay(10);
    }
    return false;
}

static bool sb(unsigned port, unsigned opcode, unsigned address, uint32_t *value)
{
    if (!wait_bits(SB_COMMAND, 1, 0, 5000)) return false;
    wr(SB_ADDRESS, address);
    wr(SB_DATA, opcode & 1 ? *value : 0);
    wr(SB_COMMAND, (opcode << 16) | (port << 8) | 0xf1);
    if (!wait_bits(SB_COMMAND, 1, 0, 10000)) return false;
    if (!(opcode & 1)) *value = rd(SB_DATA);
    return true;
}

static bool phy_well(uint32_t state)
{
    uint32_t value;
    if (!sb(PUNIT, 6, 0x60, &value)) return false;
    value = (value & ~PHY_D_WELL) | state;
    if (!sb(PUNIT, 7, 0x60, &value)) return false;
    for (unsigned retry = 0; retry < 100; retry++) {
        if (!sb(PUNIT, 6, 0x61, &value)) return false;
        if ((value & PHY_D_WELL) == state) return true;
        wyse_udelay(20);
    }
    return false;
}

/* Only writable configuration registers from the driver modeset sequence. */
static const uint16_t pll_regs[] = {
    0x8170, 0x8178, /* common power controls */
    0x8114, 0x814c, /* clock channel and distribution */
    0x0220, 0x0420, /* PCS clock channel */
    0x8134, 0x8000, 0x8004, 0x8008, 0x800c, 0x8024, 0x8018, 0x8020, 0x8138
};
static uint32_t pll[sizeof(pll_regs) / sizeof(pll_regs[0])];
static const uint16_t lane_regs[] = {
    0x0224, 0x0424, 0x0228, 0x0428, 0x022c, 0x042c, 0x0230, 0x0430,
    0x0088, 0x008c, 0x0090, 0x00b8, 0x0288, 0x028c, 0x0290, 0x02b8,
    0x0488, 0x048c, 0x0490, 0x04b8, 0x0688, 0x068c, 0x0690, 0x06b8,
    0x0200, 0x0400, 0x0204, 0x0404
};
static uint32_t lanes[sizeof(lane_regs) / sizeof(lane_regs[0])];
static const unsigned pipe_regs[] = {
    0x63000, 0x63004, 0x63008, 0x6300c, 0x63010, 0x63014, 0x6301c,
    0x63100, 0x74030, 0x74184, 0x74188, 0x7418c, 0x74190, 0x741a4
};
static uint32_t pipe[sizeof(pipe_regs) / sizeof(pipe_regs[0])];
static uint32_t phy_snapshot, dpll_snapshot, pipe_configuration, plane, encoder;
static bool valid, active;
static uint32_t original_phy, original_dpll, original_power;

bool chv_mirror_active(void) { return active; }

void chv_mirror_disable(void)
{
    if (!active) return;
    wr(HDMI_D, rd(HDMI_D) & ~ENABLE);
    wr(0x74180, rd(0x74180) & ~ENABLE);
    wr(0x7419c, rd(0x7419c));
    wr(PIPE_C, rd(PIPE_C) & ~(ENABLE | ACTIVE));
    (void)wait_bits(PIPE_C, ACTIVE, 0, 50000);
    wr(DPLL_C, original_dpll & ~PLL_LOCK);
    wr(PHY_CONTROL, original_phy);
    (void)phy_well(original_power & PHY_D_WELL);
    active = false;
}

bool chv_mirror_capture(void)
{
    valid = false;
    if (read_config(0) != 0x22b08086u) return false;
    uint64_t bar = ((uint64_t)read_config(0x14) << 32 | read_config(0x10)) & ~0xfull;
    if (!bar) return false;
    display = (volatile uint32_t *)(bar + DISPLAY);
    encoder = rd(HDMI_D);
    pipe_configuration = rd(PIPE_C);
    if (!(encoder & ENABLE) || (encoder & (3u << 24)) != (2u << 24) ||
        !(pipe_configuration & ACTIVE) || !(rd(PHY_STATUS) & PHY_D_GOOD)) return false;
    dpll_snapshot = rd(DPLL_C);
    phy_snapshot = rd(PHY_CONTROL);
    plane = rd(0x74180);
    for (unsigned i = 0; i < sizeof(pipe_regs) / sizeof(pipe_regs[0]); i++)
        pipe[i] = rd(pipe_regs[i]);
    for (unsigned i = 0; i < sizeof(pll_regs) / sizeof(pll_regs[0]); i++)
        if (!sb(DPIO_D, 0, pll_regs[i], &pll[i]) || pll[i] == 0xffffffffu) {
            printf("mirror: DPIO PLL read %x failed\n", pll_regs[i]);
            return false;
        }
    for (unsigned i = 0; i < sizeof(lane_regs) / sizeof(lane_regs[0]); i++)
        if (!sb(DPIO_D, 0, lane_regs[i], &lanes[i]) || lanes[i] == 0xffffffffu) {
            printf("mirror: DPIO lane read %x failed\n", lane_regs[i]);
            return false;
        }
    printf("mirror: captured D htotal %08x vtotal %08x source %08x pll %08x phy %08x plane %08x %08x\n",
           pipe[0], pipe[3], pipe[6], dpll_snapshot, phy_snapshot, plane, rd(0x7419c));
    valid = true;
    return true;
}

bool chv_mirror_enable(void)
{
    /* This version handles the first output's progressive 1080p60 mode.
     * The caller has checked that the second EDID advertises that mode. */
    if (!valid || (rd(HDMI_B) & (ENABLE | (3u << 24))) != (ENABLE | (1u << 24)) ||
        (rd(PIPE_C) & ACTIVE) || !(rd(PIPE_B) & ACTIVE) || !(plane & ENABLE) ||
        rd(0x61000) != 0x0897077fu || rd(0x6100c) != 0x04640437u ||
        rd(0x6101c) != 0x077f0437u || (rd(0x61230) & ENABLE) ||
        rd(0x71188) != 1920 * 4) return false;

    /* Supply pipe C with the primary's exact pixel clock and timings.
     * Keep port D's captured lane/clock-routing controls. */
    static const uint32_t masks[] = {
        0x00e0fff0, 0xff, 0xf07, 0x003fffff, 0x1010f, 0xf, 0x71f0f, 0x3ff,
    };
    for (unsigned i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
        uint32_t input;
        unsigned entry = 6 + i;
        unsigned offset = i == 0 ? 0x8080 : pll_regs[entry] + 0x180;
        if (!sb(DPIO_B, 0, offset, &input) || input == 0xffffffffu) return false;
        pll[entry] = (pll[entry] & ~masks[i]) | (input & masks[i]);
    }
    pll[14] |= 0x6000u; /* re-calibrate and supply the 10-bit display clock */
    for (unsigned i = 0; i < 8; i++) pipe[i] = rd(pipe_regs[i] - 0x2000);
    pipe[9] = rd(0x71184);
    pipe[10] = rd(0x71188);
    pipe[11] = 0;
    pipe[12] = ((pipe[6] & 0xffff) << 16) | (pipe[6] >> 16);
    pipe[13] = rd(0x711a4);
    printf("mirror: programmed C htotal %08x vtotal %08x source %08x stride %u\n",
           pipe[0], pipe[3], pipe[6], pipe[10]);

    original_phy = rd(PHY_CONTROL);
    original_dpll = rd(DPLL_C);
    if (!sb(PUNIT, 6, 0x60, &original_power)) return false;
    const char *failed = "PHY power";
    wr(DPLL_C, dpll_snapshot & ~(ENABLE | PLL_LOCK));
    wyse_udelay(1);
    if (!phy_well(0) || !wait_bits(PHY_STATUS, PHY_D_GOOD, PHY_D_GOOD, 10000)) goto rollback;
    failed = "common configuration";
    for (unsigned i = 0; i < 2; i++)
        if (!sb(DPIO_D, 1, pll_regs[i], &pll[i])) goto rollback;
    wr(PHY_CONTROL, (original_phy & ~PHY_D_CONFIGURATION) | (phy_snapshot & PHY_D_CONFIGURATION));
    failed = "PLL configuration";
    for (unsigned i = 2; i < sizeof(pll_regs) / sizeof(pll_regs[0]); i++)
        if (!sb(DPIO_D, 1, pll_regs[i], &pll[i])) goto rollback;
    wyse_udelay(1);
    wr(DPLL_C, dpll_snapshot & ~PLL_LOCK);
    failed = "PLL lock";
    if (!wait_bits(DPLL_C, PLL_LOCK, PLL_LOCK, 10000)) goto rollback;
    failed = "lane configuration";
    for (unsigned i = 0; i < sizeof(lane_regs) / sizeof(lane_regs[0]); i++)
        if (!sb(DPIO_D, 1, lane_regs[i], &lanes[i])) goto rollback;

    for (unsigned i = 0; i < sizeof(pipe_regs) / sizeof(pipe_regs[0]); i++) wr(pipe_regs[i], pipe[i]);
    wr(PIPE_C, pipe_configuration & ~ACTIVE);
    failed = "pipe C enable";
    if (!wait_bits(PIPE_C, ACTIVE, ACTIVE, 50000)) goto rollback;
    wr(0x74180, plane);
    wr(0x7419c, rd(0x7119c)); /* The two planes read the same GTT address. */
    wr(HDMI_D, encoder & ~(1u << 6)); /* Keep the primary digital audio. */
    failed = "second scanout";
    if (!wait_bits(HDMI_D, ENABLE, ENABLE, 20000)) goto rollback;
    active = true;
    printf("mirror: B/C pipes %08x %08x, HDMI B/D %08x %08x, shared surface %08x\n",
           rd(PIPE_B), rd(PIPE_C), rd(HDMI_B), rd(HDMI_D), rd(0x7419c));
    return true;

rollback:
    printf("mirror: stopped at %s, PHY %08x DPLL %08x\n", failed, rd(PHY_STATUS), rd(DPLL_C));
    active = true;
    chv_mirror_disable();
    return false;
}
