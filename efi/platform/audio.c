// The sound core (core 1): OPL3 synthesis and the mixer, as on X86Pi.
//
// Every device's output is summed into the LPE audio buffers (HDMI or DP),
// a period at a time as the hardware frees them. The mix follows X86Pi's
// CircleAudio::Pump(); the analog output will take the same frames.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "pc.h"
#include "adlib.h"
#include "sb16.h"
#include "pcspk.h"
#include "sn76489.h"
#include "dss.h"

int lpe_pump(void (*fill)(int16_t *dst, int frames));
bool lpe_active(void);
void analog_put(const int16_t *frames, int n);
void analog_feed(void);
uint32_t analog_level(void);
bool analog_owns_core(void);
extern int g_audio_volume;
#include "config_save.h"
#include "gmsynth.h"

// a source at its mixer level (16 is as it comes)
static inline int level(int s, int which) {
    int l = g_mix_level[which];
    return l == 16 ? s : s * l >> 4;
}

static PC *apc;
static int covox_dc, dss_value;
static uint32_t dss_phase;
uint32_t g_audio_peak, g_audio_frames;
volatile uint32_t g_audio_tone_frames;     // a 1 kHz test sine for this many frames
static uint32_t tone_phase;
int g_audio_tone_shift;

// sin() of a 0..47 phase step, for 1 kHz at 48 kHz, -6 dBFS
static int tone_sample(void) {
    static int16_t table[48];
    if (!table[12]) {
        for (int i = 0; i < 48; i++) {
            // sine by a recurrence, no libm on this core needed
            double x = i * (2 * 3.14159265358979 / 48), s = 0, t = x;
            for (int k = 1; k < 20; k += 2) { s += t; t *= -x * x / ((k + 1) * (k + 2)); }
            table[i] = (int16_t)(s * 16384);
        }
    }
    int v = table[tone_phase] >> g_audio_tone_shift;
    tone_phase = (tone_phase + 1) % 48;
    return v;
}

static inline int clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

volatile int g_audio_pause;    // the monitor's menu is open: silence

static void mix(int16_t *dst, int frames) {
    PC *pc = apc;
    if (g_audio_pause) {
        memset(dst, 0, (size_t)frames * 4);
        analog_put(dst, frames);
        return;
    }
    // the OPL renders ahead of the mixer; make sure this period is there
    if (pc->adlib_enabled)
        for (int i = 0; i < 64 && adlib_ready(pc->adlib) < (uint32_t)frames; i++) adlib_synth_step();

    for (int i = 0; i < frames; i++) {
        int left = 0, right = 0;
        if (pc->pcspk_enabled) {
            int s = level(pcspk_sample(pc->pcspk), MIX_PCSPEAKER);
            left += s;
            right += s;
        }
        if (pc->covox_enabled) {
            // a latch with a DC blocker, like the capacitor on a real one
            int latch = ((int)pc->covox_sample - 127) << 8;
            covox_dc += ((latch << 8) - covox_dc) >> 9;
            int s = level(latch - (covox_dc >> 8), MIX_COVOX);
            left += s;
            right += s;
        }
        if (pc->tandy_enabled) {
            int s = level(sn76489_sample(), MIX_TANDY);
            left += s;
            right += s;
        }
        if (pc->mpu401_enabled) {
            if (gmsynth_active()) {
                // the SoundFont, rendered ahead on the synthesis core
                int l, r;
                gmsynth_getframe(&l, &r);
                left += level(l, MIX_MIDI);
                right += level(r, MIX_MIDI);
            } else {
                int s = level(midi_sample(), MIX_MIDI);
                left += s;
                right += s;
            }
        }
        if (pc->adlib_enabled) {
            int l, r;
            adlib_getframe(pc->adlib, &l, &r);
            left += level(l, MIX_ADLIB);
            right += level(r, MIX_ADLIB);
        }
        if (pc->sb16_enabled) {
            int l = 0, r = 0;
            sb16_getsample(pc->sb16, &r, &l);
            left += level(l, MIX_SOUNDBLASTER);
            right += level(r, MIX_SOUNDBLASTER);
        }
        if (pc->dss_enabled) {
            dss_phase += 7000u;
            if (dss_phase >= SOUND_FREQUENCY) {
                dss_phase -= SOUND_FREQUENCY;
                dss_value = dss_sample();
            }
            left += level(dss_value, MIX_COVOX);
            right += level(dss_value, MIX_COVOX);
        }
        if (g_audio_tone_frames) {
            int s = tone_sample();
            left += s;
            right += s;
            g_audio_tone_frames--;
        }
        if (g_audio_volume != 16) {
            left = (left * g_audio_volume) >> 4;
            right = (right * g_audio_volume) >> 4;
        }
        int l = clamp16(left), r = clamp16(right);
        dst[2 * i] = (int16_t)l;
        dst[2 * i + 1] = (int16_t)r;
        int m = l < 0 ? -l : l;
        if ((uint32_t)m > g_audio_peak) g_audio_peak = m;
    }
    g_audio_frames += frames;
    analog_put(dst, frames);    // the same frames to the headphone jack
}

static void audio_core(void) {
    static int16_t buf[512 * 2];
    for (;;) {
        extern volatile uint64_t g_core_heartbeat[4];
        void mp_hang_watch_core3(void);
        g_core_heartbeat[1]++;
        mp_hang_watch_core3();
        int work = adlib_synth_step();
        int n = lpe_pump(mix);
        // without HDMI/DP audio the analog output sets the pace
        if (!lpe_active() && analog_level() < 2048) {
            mix(buf, 512);
            n++;
        }
        if (!analog_owns_core()) {
            analog_feed();
            if (gmsynth_step()) work = 1;     // General MIDI here when core 3 is not running
        }
        if (!work && !n) __asm__ volatile("pause");
    }
}

bool mp_start(int core, void (*fn)(void));

bool audio_start(PC *pc) {
    apc = pc;
    return mp_start(1, audio_core);
}
