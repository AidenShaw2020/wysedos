#pragma once
#include <stdint.h>

/* Accepted driver-tagged 3D work, never plain VBE/GDI. An idle renderer
 * returns to base after three seconds; every text transition revokes it. */
#define CPU_WINDOWS_3D_IDLE_US 3000000u
typedef struct {
    uint32_t renewed, epoch, last_count;
    int active;
} CpuWindows3DBoost;

static inline int cpu_windows_3d_boost_step(CpuWindows3DBoost *s, uint32_t now,
                                           uint32_t epoch, uint32_t count,
                                           int enabled, int graphics)
{
    if (!enabled || !graphics || s->epoch != epoch) {
        s->last_count = count;
        s->epoch = epoch;
        s->active = 0;
        return 0;
    }
    if (s->last_count != count) {
        s->last_count = count;
        s->renewed = now;
        s->active = 1;
    }
    if (s->active && now - s->renewed >= CPU_WINDOWS_3D_IDLE_US) {
        s->active = 0;
    }
    return s->active;
}

/* Keep the configured base speed for boot/text DOS. Each optional display
 * override is independent; leaving one must not cancel the other. */
static inline int cpu_speed_effective(int base_mhz, int graphics_enabled,
                                     int graphics_active, int glide_enabled,
                                     int glide_active)
{
    return ((graphics_enabled && graphics_active) ||
            (glide_enabled && glide_active)) ? 0 : base_mhz;
}
