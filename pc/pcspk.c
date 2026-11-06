/*
 * QEMU PC speaker emulation
 *
 * Copyright (c) 2006 Joachim Henke
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "pcspk.h"
#include <stdlib.h>
#include <string.h>
#include <pico.h>
#ifdef WYSE_BUILD
#include "audioclock.h"
#define PCSPK_EDGE_SLOTS 4096u
#endif

#if defined(BUILD_ESP32)
void *pcmalloc(long size);
#else
#define pcmalloc malloc
#endif

/*
 * The looping square-wave buffer behind pcspk_callback().
 *
 * That callback is the QEMU mixer path and nothing on this port calls it:
 * the RP2350 audio side asks pcspk_sample() for one sample at a time and
 * derives it from the PIT directly.  The buffer was still 4 KB, taken from
 * a malloc heap that ends up with about 4 KB left in it - so pcspk_init()
 * was the allocation that failed, and the whole board came up as a black
 * screen with "Out of memory" on a console nobody watches.  Keeping it
 * small leaves that headroom for the devices that actually need it.
 */
#define PCSPK_BUF_LEN 256
//#define PCSPK_SAMPLE_RATE 32000
/* The PC speaker is sampled by the same one-frame-per-call timer as
 * everything else, so its rate is the output rate, not a nominal 44100. */
#define PCSPK_SAMPLE_RATE SOUND_FREQUENCY
#define PCSPK_MAX_FREQ (PCSPK_SAMPLE_RATE >> 1)
#define PCSPK_MIN_COUNT ((PIT_FREQ + PCSPK_MAX_FREQ - 1) / PCSPK_MAX_FREQ)

struct PCSpkState {
    uint8_t sample_buf[PCSPK_BUF_LEN];
//    QEMUSoundCard card;
//    SWVoiceOut *voice;
    PITState *pit;
    unsigned int pit_count;
    unsigned int samples;
    unsigned int play_pos;
    int data_on;
    int dummy_refresh_clock;
    int active_out;
#ifdef WYSE_BUILD
    AudioClock clock;
    int64_t consumed;
    uint32_t edge_write, edge_read;
    struct { uint32_t frame, bits; } edges[PCSPK_EDGE_SLOTS];
    uint32_t render_bits, queued_bits, overflow_bits;
    uint32_t overflow_write, overflow_read;
    int32_t direct_dc;
#endif
};

static inline void generate_samples(PCSpkState *s)
{
    unsigned int i;

    if (s->pit_count) {
        const uint32_t m = PCSPK_SAMPLE_RATE * s->pit_count;
        const uint32_t n = ((uint64_t)PIT_FREQ << 32) / m;

        /* multiple of wavelength for gapless looping */
        s->samples = ((uint64_t)PCSPK_BUF_LEN * PIT_FREQ / m * m / (PIT_FREQ >> 1) + 1) >> 1;
        if (s->samples == 0) s->samples = 1;
        for (i = 0; i < s->samples; ++i)
            s->sample_buf[i] = (64 & (n * i >> 25)) - 32;
    } else {
        s->samples = PCSPK_BUF_LEN;
        for (i = 0; i < PCSPK_BUF_LEN; ++i)
            s->sample_buf[i] = 128; /* silence */
    }
}

#include <stdio.h>

void pcspk_callback(void *opaque, uint8_t *stream, int free)
{
    PCSpkState *s = opaque;
    unsigned int n;

    if (pit_get_mode(s->pit, 2) != 3)
        return;
    if (!s->data_on)
        return;

    n = pit_get_initial_count(s->pit, 2);
    /* avoid frequencies that are not reproducible with sample rate */
    if (n < PCSPK_MIN_COUNT)
        n = 0;

    if (s->pit_count != n) {
        s->pit_count = n;
        s->play_pos = 0;
        generate_samples(s);
    }

    int k = 0;
    while (free > 0) {
        n = s->samples - s->play_pos;
        if (n > free)
            n = free;
        memcpy(stream + k, &s->sample_buf[s->play_pos], n);
//        n = AUD_write(s->voice, &s->sample_buf[s->play_pos], n);
        if (!n)
            break;
        s->play_pos = (s->play_pos + n) % s->samples;
        free -= n;
        k += n;
    }
}

uint32_t pcspk_ioport_read(void *opaque)
{
    PCSpkState *s = opaque;
    int out;

    s->dummy_refresh_clock ^= (1 << 4);
    out = pit_get_out(s->pit, 2) << 5;

    return pit_get_gate(s->pit, 2) | (s->data_on << 1) | s->dummy_refresh_clock | out;
}

static void AUD_set_active_out (PCSpkState *s, int i)
{
    s->active_out = i;
}

void pcspk_ioport_write(void *opaque, uint32_t val)
{
    PCSpkState *s = opaque;
    const int gate = val & 1;
    int oldout = gate & s->data_on;

    s->data_on = (val >> 1) & 1;
    pit_set_gate(s->pit, 2, gate);
    if (gate) /* restart */
        s->play_pos = 0;

    int newout = gate & s->data_on;
    if (oldout != newout) {
        AUD_set_active_out(s, gate & s->data_on);
    }
#ifdef WYSE_BUILD
    const uint32_t bits = val & 3;
    if (bits != s->queued_bits) {
        const uint32_t written = __atomic_load_n(&s->edge_write, __ATOMIC_RELAXED);
        const uint32_t read = __atomic_load_n(&s->edge_read, __ATOMIC_ACQUIRE);
        if (written - read < PCSPK_EDGE_SLOTS) {
            uint32_t frame;
            if (!audioclock_read(&s->clock, &frame)) frame = 0;
            s->edges[written & (PCSPK_EDGE_SLOTS - 1u)].frame = frame + AUDIOCLOCK_STAMP_LEAD(0);
            s->edges[written & (PCSPK_EDGE_SLOTS - 1u)].bits = bits;
            __atomic_store_n(&s->edge_write, written + 1u, __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&s->overflow_bits, bits, __ATOMIC_RELAXED);
            __atomic_fetch_add(&s->overflow_write, 1u, __ATOMIC_RELEASE);
        }
        s->queued_bits = bits;
    }
#endif
}

int pcspk_get_active_out(PCSpkState *s)
{
    return s->active_out;
}

PCSpkState *pcspk_init(PITState *pit)
{
    PCSpkState *s = pcmalloc(sizeof(PCSpkState));
    memset(s, 0, sizeof(PCSpkState));

    s->pit = pit;
//    register_ioport_read(0x61, 1, 1, pcspk_ioport_read, s);
//    register_ioport_write(0x61, 1, 1, pcspk_ioport_write, s);
    return s;
}

/*
 * A band-limited square, for hosts that mix at the output rate.
 *
 * pcspk_sample() below builds its wave by taking the top bit of a phase
 * accumulator.  That is a perfect square in continuous time and the wrong
 * thing to sample directly: a square's harmonics run to infinity, and every
 * one above half the sample rate folds back down into the audible band at a
 * frequency that has nothing to do with the note.  The folded partials move
 * the wrong way as the pitch rises, so some notes sound clean and their
 * neighbours rasp - which is exactly the complaint, and it is inherent to
 * the naive generator rather than a bug anywhere downstream.
 *
 * PolyBLEP fixes it where it happens.  A jump sampled bluntly is what
 * creates the aliases, so each transition gets a short polynomial that
 * approximates a band-limited step across the two samples straddling it.
 * It costs a handful of operations per sample and needs no oversampling.
 *
 * The pitch is also derived differently.  The old path divides PIT_FREQ by
 * the count to get an integer frequency and then divides again to get the
 * phase step, quantising twice; this folds both into one division, so the
 * note is as exact as the accumulator can represent.
 *
 * This replaced a full-scale square generator that survived only because
 * the RP2350 audio driver consumed its return value directly.  That
 * driver is gone, and so is the other generator.
 */
#define PCSPK_BL_PEAK 8192.0f

static inline float pcspk_poly_blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

int16_t pcspk_sample(PCSpkState *s)
{
    if (!s) return 0;
    int gate = pit_get_gate(s->pit, 2), data_on = s->data_on;
#ifdef WYSE_BUILD
    if (!(s->consumed & 63)) audioclock_steer(&s->clock, s->consumed);
    const uint32_t frame = (uint32_t)s->consumed++;
    uint32_t read = __atomic_load_n(&s->edge_read, __ATOMIC_RELAXED);
    const uint32_t written = __atomic_load_n(&s->edge_write, __ATOMIC_ACQUIRE);
    const uint32_t overflow = __atomic_load_n(&s->overflow_write, __ATOMIC_ACQUIRE);
    if (overflow != s->overflow_read) {
        s->render_bits = __atomic_load_n(&s->overflow_bits, __ATOMIC_RELAXED);
        s->overflow_read = overflow;
        read = written;
    } else {
        while (read != written) {
            const uint32_t slot = read & (PCSPK_EDGE_SLOTS - 1u);
            if ((int32_t)(s->edges[slot].frame - frame) > 0) break;
            s->render_bits = s->edges[slot].bits;
            read++;
        }
    }
    __atomic_store_n(&s->edge_read, read, __ATOMIC_RELEASE);
    gate = s->render_bits & 1;
    data_on = (s->render_bits >> 1) & 1;
#endif
    if (!gate && pit_get_mode(s->pit, 2) == 3) {
        int sample = data_on ? (int)PCSPK_BL_PEAK : 0;
#ifdef WYSE_BUILD
        s->direct_dc += ((sample << 8) - s->direct_dc) >> 9;
        sample -= s->direct_dc >> 8;
#endif
        return (int16_t)sample;
    }
    if (!data_on) return 0;
    if (pit_get_mode(s->pit, 2) != 3) return 0;
    if (!gate) return 0;

    uint32_t count = pit_get_initial_count(s->pit, 2);
    if (count < PCSPK_MIN_COUNT) return 0;

    static uint32_t phase;
    const uint32_t inc = (uint32_t)(((uint64_t)PIT_FREQ << 32) /
                                    ((uint64_t)count * PCSPK_SAMPLE_RATE));
    if (!inc) return 0;
    phase += inc;

    const float scale = 1.0f / 4294967296.0f;
    const float dt = (float)inc * scale;
    const float t = (float)phase * scale;
    float v = t < 0.5f ? 1.0f : -1.0f;
    v += pcspk_poly_blep(t, dt);
    float t2 = t + 0.5f;
    if (t2 >= 1.0f) t2 -= 1.0f;
    v -= pcspk_poly_blep(t2, dt);

    /* The correction overshoots a little at each edge, which is what a real
     * band-limited step does; clamp only to keep the result in range. */
    if (v > 1.4f) v = 1.4f;
    else if (v < -1.4f) v = -1.4f;
    return (int16_t)(v * PCSPK_BL_PEAK);
}
