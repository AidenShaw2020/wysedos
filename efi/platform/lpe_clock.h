#ifndef WYSE_LPE_CLOCK_H
#define WYSE_LPE_CLOCK_H
#include <stdint.h>

/* GOP supplies framebuffer geometry, not the actual pixel clock. Measure
 * eight complete scanout frames using the running pipe's frame counter and
 * total timings (including blanking). A monitor's EDID preferred mode need
 * not be the mode firmware selected. Offsets are Cherry Trail / i915 regs. */
typedef uint32_t (*lpe_clock_read_fn)(void *, uint32_t);
typedef uint64_t (*lpe_clock_time_fn)(void *);
static uint32_t lpe_measure_pixel_clock(lpe_clock_read_fn read_reg,
                                      lpe_clock_time_fn time_us, void *ctx,
                                      unsigned pipe) {
    static const uint32_t trans[] = {0x60000,0x61000,0x63000};
    static const uint32_t counters[] = {0x70040,0x71040,0x74040};
    if (pipe > 2) return 0;
    uint32_t hr=read_reg(ctx,trans[pipe]), vr=read_reg(ctx,trans[pipe]+0x0c);
    uint32_t ht=(hr>>16)+1, vt=(vr>>16)+1;
    if (ht < (hr&0xffff)+1 || vt < (vr&0xffff)+1 || ht < 320 || vt < 200)
        return 0;
    uint32_t reg=counters[pipe]; /* CHV pipe C is at 74000, not 72000. */
    uint32_t first=read_reg(ctx,reg), frame;
    uint64_t deadline=time_us(ctx)+1000000, start;
    do {
        frame=read_reg(ctx,reg);
        if (time_us(ctx)>=deadline) return 0;
    } while (frame==first);
    first=frame; start=time_us(ctx);
    do {
        frame=read_reg(ctx,reg);
        if (time_us(ctx)>=deadline) return 0;
    } while ((uint32_t)(frame-first)<8);
    uint64_t elapsed=time_us(ctx)-start;
    uint32_t frames=frame-first;
    if (!elapsed || frames>120) return 0;
    uint64_t khz=((uint64_t)ht*vt*frames*1000+elapsed/2)/elapsed;
    return khz>=10000 && khz<=650000 ? (uint32_t)khz : 0;
}

static uint32_t lpe_hdmi_cts(uint32_t pixel_khz, uint32_t rate, uint32_t n) {
    uint64_t divisor=128ULL*rate;
    return divisor ? ((uint64_t)pixel_khz*n*1000+divisor/2)/divisor : 0;
}
#endif
