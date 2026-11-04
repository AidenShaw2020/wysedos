// DisplayPort audio through the Cherry Trail display's LPE audio block.
//
// The register sequence follows Linux's sound/x86/intel_hdmi_audio.c and
// i915's intel_lpe_audio.c (which notes this exact machine needs the audio
// chicken bit). The GOP has already trained the DP link and lit the pipe;
// we only add audio to it: audio output enable in the port, amp unmute,
// N/Maud for the link rate, the audio info frame, and a ring of four DMA
// buffers that lpe_pump() refills as the hardware empties them.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../include/efi.h"
#include "lpe_clock.h"

extern EFI_BOOT_SERVICES *gBS;

#define VLV_DISPLAY_BASE    0x180000
#define LPE_AUDIO_BASE      (VLV_DISPLAY_BASE + 0x65000)
#define AUD_CHICKEN         (VLV_DISPLAY_BASE + 0x62F38)

// LPE registers, relative to the pipe's config block
#define AUD_CONFIG          0x00
#define AUD_CH_STATUS_0     0x08
#define AUD_CH_STATUS_1     0x0C
#define AUD_HDMI_CTS        0x10
#define AUD_N_ENABLE        0x14
#define AUD_BUF_CONFIG      0x20
#define AUD_BUF_CH_SWAP     0x24
#define AUD_BUF_ADDR(i)     (0x40 + (i) * 8)
#define AUD_BUF_LEN(i)      (0x44 + (i) * 8)
#define AUD_CNTL_ST         0x60
#define AUD_HDMI_STATUS     0x64
#define AUD_HDMIW_INFOFR    0x68

#define NBUF        4
#define PERIOD      512     // frames per buffer: 11.6 ms at 44.1 kHz

static volatile uint8_t *mmio;
static volatile uint32_t *aud;      // LPE config block of our pipe
static int16_t *bufs[NBUF];
static int head;                    // next buffer the hardware will finish
static uint32_t rate = 44100;
static bool running;
uint32_t g_lpe_underruns, g_lpe_periods;

static inline uint32_t pci_read(int bus, int dev, int fn, int reg) {
    uint32_t addr = 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xfc);
    __asm__ volatile("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xcf8));
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xcfc));
    return v;
}

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }
static inline void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio + off) = v; }
static inline void awr(uint32_t reg, uint32_t v) { aud[reg / 4] = v; }
static inline uint32_t ard(uint32_t reg) { return aud[reg / 4]; }

static uint32_t clock_read(void *ctx, uint32_t reg) {
    return rd(VLV_DISPLAY_BASE+reg);
}
static uint64_t clock_time(void *ctx) {
    extern uint64_t wyse_ticks_us(void);
    return wyse_ticks_us();
}

static void flush(const void *p, size_t n) {
    for (size_t i = 0; i < n; i += 64) __asm__ volatile("clflush (%0)" :: "r"((const uint8_t *)p + i) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static void prog_bd(int i) {
    flush(bufs[i], PERIOD * 4);
    awr(AUD_BUF_ADDR(i), (uint32_t)(uint64_t)bufs[i] | 1);    // valid, no interrupt: we poll
    awr(AUD_BUF_LEN(i), PERIOD * 4);
}

static bool alloc_bufs(void) {
    for (int i = 0; i < NBUF; i++) {
        EFI_PHYSICAL_ADDRESS a = 0xffffffff;
        if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &a))) return false;
        bufs[i] = (int16_t *)a;
        memset(bufs[i], 0, PERIOD * 4);
    }
    return true;
}

// HDMI: CTS/N from the TMDS clock, the HDMI audio info frame, and the port
// switched to HDMI mode (the GOP may drive DVI, which has no data islands).
static bool lpe_init_hdmi(int port, int pipe, uint32_t port_reg, uint32_t en_dbg,
                          uint32_t pixel_khz) {
    static const uint32_t cfg_off[3] = {0x000, 0x800, 0x900};
    uint32_t pv = rd(VLV_DISPLAY_BASE + port_reg);
    printf("lpe: HDMI%c on pipe %c, %s mode\n", 'B' + port, 'A' + pipe, (pv & (1u << 9)) ? "HDMI" : "DVI");
    wr(VLV_DISPLAY_BASE + port_reg, pv | (1u << 9) | (1u << 6));    // HDMI mode, audio enable
    wr(AUD_CHICKEN, rd(AUD_CHICKEN) | 1);
    wr(VLV_DISPLAY_BASE + en_dbg, rd(VLV_DISPLAY_BASE + en_dbg) & ~2u);
    aud = (volatile uint32_t *)(mmio + LPE_AUDIO_BASE + cfg_off[pipe]);
    if (!alloc_bufs()) return false;

    uint32_t n = rate == 48000 ? 6144 : 6272;
    uint32_t cts = lpe_hdmi_cts(pixel_khz,rate,n);
    awr(AUD_HDMI_STATUS, 1);
    awr(AUD_HDMI_STATUS, 0);
    awr(AUD_N_ENABLE, (1u << 24) | n);
    awr(AUD_HDMI_CTS, (1u << 24) | cts);
    uint32_t w1 = 0x000a0184, w2 = 1u << 8, w3 = 0;     // 2 channels, allocation 0
    uint8_t sum = 0;
    for (int i = 0; i < 4; i++) sum += (uint8_t)(w1 >> (8 * i)) + (uint8_t)(w2 >> (8 * i)) + (uint8_t)(w3 >> (8 * i));
    w2 |= (uint8_t)-sum;
    awr(AUD_CNTL_ST, 0);
    awr(AUD_HDMIW_INFOFR, w1);
    awr(AUD_HDMIW_INFOFR, w2);
    awr(AUD_HDMIW_INFOFR, w3);
    for (int i = 0; i < 13; i++) awr(AUD_HDMIW_INFOFR, 0);
    awr(AUD_CNTL_ST, (1u << 16) | (1u << 21));
    awr(AUD_CH_STATUS_0, (uint32_t)(rate == 48000 ? 2 : 0) << 24);
    awr(AUD_CH_STATUS_1, 1u << 1);
    awr(AUD_BUF_CONFIG, 0xfe | (7u << 8));
    uint32_t cfg = (1u << 9) | (1u << 12);      // valid, 16-bit packets, layout 0, 2 ch
    awr(AUD_CONFIG, cfg);
    for (int i = 0; i < NBUF; i++) prog_bd(i);
    awr(AUD_BUF_CH_SWAP, 0x00fac4c8);
    head = 0;
    awr(AUD_HDMI_STATUS, ard(AUD_HDMI_STATUS) | (1u << 29) | (1u << 31));
    awr(AUD_CONFIG, cfg | 1);
    running = true;
    printf("lpe: HDMI audio running, %u Hz, N %u CTS %u, port %08x, status %08x\n", rate, n, cts,
           rd(VLV_DISPLAY_BASE + port_reg), ard(AUD_HDMI_STATUS));
    return true;
}

bool lpe_init(uint32_t sample_rate) {
    rate = sample_rate;
    uint32_t id = pci_read(0, 2, 0, 0);
    uint64_t bar = pci_read(0, 2, 0, 0x10) & ~0xfULL;
    bar |= (uint64_t)pci_read(0, 2, 0, 0x14) << 32;
    mmio = (volatile uint8_t *)bar;
    printf("lpe: igpu %08x, mmio %llx\n", id, (unsigned long long)bar);
    if ((id & 0xffff) != 0x8086 || !bar) return false;

    // which port is lit, and from which pipe. A DP connector with a passive
    // DP->HDMI adapter is driven in HDMI (TMDS) mode through the HDMI port
    // registers, so look at both.
    static const uint32_t ports[3] = {0x64100, 0x64200, 0x64300};
    static const uint32_t hdmi_ports[3] = {0x61140, 0x61160, 0x6116C};
    static const uint32_t port_en_dbg[3] = {0x62F20, 0x62F30, 0x62F34};
    int port = -1, pipe = 0;
    bool dp = true;
    for (int i = 0; i < 3; i++) {
        uint32_t v = rd(VLV_DISPLAY_BASE + ports[i]), h = rd(VLV_DISPLAY_BASE + hdmi_ports[i]);
        printf("lpe: port %c: DP %08x HDMI %08x\n", 'B' + i, v, h);
        if ((v & (1u << 31)) && port < 0) {
            port = i;
            pipe = (v >> 16) & 3;
        } else if ((h & (1u << 31)) && port < 0) {
            port = i;
            pipe = (h >> 24) & 3;
            dp = false;
        }
    }
    if (port < 0) {
        printf("lpe: no display port enabled\n");
        return false;
    }
    uint32_t pixel_khz=lpe_measure_pixel_clock(clock_read,clock_time,NULL,pipe);
    printf("lpe: pipe %c measured pixel clock %u kHz\n",'A'+pipe,pixel_khz);
    if (!pixel_khz) {
        printf("lpe: no valid scanout clock; display audio stays disabled\n");
        return false;
    }
    if (!dp) return lpe_init_hdmi(port, pipe, hdmi_ports[port], port_en_dbg[port],pixel_khz);
    // link rate from the link M/N: M/N = pixel clock / link symbol clock
    static const uint32_t trans[3] = {0x60000, 0x61000, 0x63000};
    uint32_t lm = rd(VLV_DISPLAY_BASE + trans[pipe] + 0x40) & 0xffffff;
    uint32_t ln = rd(VLV_DISPLAY_BASE + trans[pipe] + 0x44);
    static const uint32_t pipes[3] = {0x70000,0x71000,0x74000};
    uint32_t gm = rd(VLV_DISPLAY_BASE + pipes[pipe] + 0x60) & 0xffffff;
    uint32_t gn = rd(VLV_DISPLAY_BASE + pipes[pipe] + 0x64);
    printf("lpe: DP_%c on pipe %c, link M/N %u/%u, g4x %u/%u\n", 'B' + port, 'A' + pipe, lm, ln, gm, gn);
    uint32_t mm = lm ? lm : gm, nn = lm ? ln : gn;
    uint64_t ls = nn && mm ? (uint64_t)pixel_khz * nn / mm : 0;
    uint32_t link=0;
    const uint32_t rates[]={162000,270000,540000};
    for (unsigned i=0;i<3;i++)
        if (ls>rates[i]*95ULL/100 && ls<rates[i]*105ULL/100) link=rates[i];
    if (!link) { printf("lpe: invalid DP link clock %llu kHz\n",(unsigned long long)ls);return false; }
    printf("lpe: measured DP link ~%llu kHz -> %u kHz\n",(unsigned long long)ls,link);

    // audio on: port audio enable, chicken bit (Wyse 3040), amp unmute
    uint32_t pv = rd(VLV_DISPLAY_BASE + ports[port]);
    wr(VLV_DISPLAY_BASE + ports[port], pv | (1u << 6));
    wr(AUD_CHICKEN, rd(AUD_CHICKEN) | 1);
    wr(VLV_DISPLAY_BASE + port_en_dbg[port], rd(VLV_DISPLAY_BASE + port_en_dbg[port]) & ~2u);

    static const uint32_t cfg_off[3] = {0x000, 0x800, 0x900};
    aud = (volatile uint32_t *)(mmio + LPE_AUDIO_BASE + cfg_off[pipe]);

    for (int i = 0; i < NBUF; i++) {
        EFI_PHYSICAL_ADDRESS a = 0xffffffff;
        if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &a))) return false;
        bufs[i] = (int16_t *)a;
        memset(bufs[i], 0, PERIOD * 4);
    }

    // reset, N/Maud, info frame, channel status, config, buffers
    awr(AUD_HDMI_STATUS, 1);
    awr(AUD_HDMI_STATUS, 0);
    uint32_t maud = ((uint64_t)rate*512*32768+link*500ULL)/(link*1000ULL);
    awr(AUD_N_ENABLE, (1u << 24) | 32768);
    awr(AUD_HDMI_CTS, (1u << 24) | maud);
    awr(AUD_CNTL_ST, 0);
    awr(AUD_HDMIW_INFOFR, 0x00441b84);      // DP audio info frame header
    awr(AUD_HDMIW_INFOFR, 1);               // 2 channels, allocation 0
    for (int i = 0; i < 14; i++) awr(AUD_HDMIW_INFOFR, 0);
    awr(AUD_CNTL_ST, (1u << 16) | (1u << 21));   // dip_freq 1, dip enable
    awr(AUD_CH_STATUS_0, (uint32_t)(rate == 48000 ? 2 : 0) << 24);
    awr(AUD_CH_STATUS_1, 1u << 1);          // 16-bit words
    awr(AUD_BUF_CONFIG, 0xfe | (7u << 8));
    uint32_t cfg = (1u << 7) | (1u << 9) | (1u << 12) | (1u << 15);  // set, valid, 16-bit packets, DP
    awr(AUD_CONFIG, cfg);
    for (int i = 0; i < NBUF; i++) prog_bd(i);
    awr(AUD_BUF_CH_SWAP, 0x00fac4c8);
    head = 0;
    awr(AUD_HDMI_STATUS, ard(AUD_HDMI_STATUS) | (1u << 29) | (1u << 31));
    awr(AUD_CONFIG, cfg | 1);               // go
    running = true;
    printf("lpe: running, %u Hz, maud %u, status %08x\n", rate, maud, ard(AUD_HDMI_STATUS));
    return true;
}

// Refill every buffer the hardware has finished. fill(dst, frames) writes
// interleaved stereo int16. Returns the number of buffers refilled.
int lpe_pump(void (*fill)(int16_t *dst, int frames)) {
    if (!running) return 0;
    int n = 0;
    for (int k = 0; k < NBUF; k++) {
        uint32_t left = ard(AUD_BUF_LEN(head)) & 0xfffff;
        if (left) break;
        fill(bufs[head], PERIOD);
        prog_bd(head);
        head = (head + 1) % NBUF;
        g_lpe_periods++;
        n++;
    }
    if (n == NBUF) g_lpe_underruns++;
    uint32_t st = ard(AUD_HDMI_STATUS);
    if (st & (1u << 31)) awr(AUD_HDMI_STATUS, st | (1u << 31));
    return n;
}

bool lpe_active(void) { return running; }

// Frames still queued for the hardware (for pacing the mixer).
int lpe_queued(void) {
    if (!running) return 0;
    int frames = 0;
    for (int k = 0; k < NBUF; k++) frames += (ard(AUD_BUF_LEN(k)) & 0xfffff) / 4;
    return frames;
}

// A test tone, to tell the output path from the mixer.
static void tone(int16_t *dst, int frames) {
    static uint32_t phase;
    for (int i = 0; i < frames; i++) {
        // 1 kHz triangle at -12 dB
        uint32_t p = (phase += 1000 * 65536 / rate) & 0xffff;
        int v = p < 32768 ? (int)p - 16384 : 49152 - (int)p;
        dst[2 * i] = dst[2 * i + 1] = (int16_t)(v / 2);
    }
}

void lpe_test_tone(uint32_t ms, void (*delay_us)(uint32_t)) {
    for (uint32_t t = 0; t < ms; t += 2) {
        lpe_pump(tone);
        delay_us(2000);
    }
    printf("lpe: tone done, %u periods, %u underruns, status %08x\n", g_lpe_periods, g_lpe_underruns,
           ard(AUD_HDMI_STATUS));
}
