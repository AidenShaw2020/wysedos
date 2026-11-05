// wyseDOS platform: runs the X86Pi PC model on the Wyse 3040 under UEFI.
//
// HAL for the model (time, memory, ROM loading, log), configuration from
// 386/config.ini (via netfs), the display (the model's 640x480 surface scaled
// into the GOP framebuffer) and keyboard input, and the main loop.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "pc.h"
#include "edid.h"
#include "ini.h"
#include "config_save.h"
#include "ff.h"
#include "blockdev.h"
#include "osd.h"
#include "settingsui.h"
#include "diskui.h"
#include "storage.h"
#include "installer.h"
#include "delltoken.h"
#include "blkcache.h"
#include "hostblk.h"
#include "ide.h"
#include "gmsynth.h"
#include "../include/efi.h"
#include "../src/net.h"

extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;
const char *wyse_cpu_init(PC *pc);
int usbhid_init(void);
int usbhid_scan(void);
int usbhid_poll(PC *pc);
bool wc_map(uint64_t base, uint64_t size);
int mp_init(void);
bool lpe_init(uint32_t sample_rate);
void lpe_test_tone(uint32_t ms, void (*delay_us)(uint32_t));
bool audio_start(PC *pc);
bool analog_init(void);
void analog_start(void);
bool mp_start(int core, void (*fn)(void));
volatile int g_wyse_vga_core;
uint32_t g_vga_flips;       // page flips at the retrace (vga.c), per report
extern uint64_t g_vmx_exits[64], g_vmx_bursts, g_vmx_burst_insns, g_vmx_irqs, g_vmx_guest_tsc;
extern uint32_t g_netfs_retries, g_netfs_requests;

//--------------------------------------------------------------------+
// time
//--------------------------------------------------------------------+

static uint64_t tsc_khz;
uint64_t g_uticks_scale;

static inline uint64_t rdtsc_(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void wyse_time_init(void) {
    uint64_t t0 = rdtsc_();
    gBS->Stall(100000);
    tsc_khz = (rdtsc_() - t0) / 100;
    g_uticks_scale = ((uint64_t)1000 << 32) / tsc_khz;   // us per tick, 32.32
}

uint64_t wyse_tsc_khz(void) { return tsc_khz; }
uint64_t wyse_ticks_us(void) { return rdtsc_() * 1000 / tsc_khz; }
uint64_t time_us_64(void) { return wyse_ticks_us(); }
uint32_t time_us_32(void) { return (uint32_t)wyse_ticks_us(); }
uint32_t get_uticks(void) { return (uint32_t)wyse_ticks_us(); }
void wyse_udelay(uint32_t us) { uint64_t t = wyse_ticks_us() + us; while (wyse_ticks_us() < t) __asm__ volatile("pause"); }
void sleep_us(uint32_t us) { wyse_udelay(us); }
void sleep_ms(uint32_t ms) { wyse_udelay(ms * 1000u); }
uint64_t make_timeout_time_ms(uint32_t ms) { return time_us_64() + (uint64_t)ms * 1000u; }
bool time_reached(uint64_t t) { return time_us_64() >= t; }

int32_t __fast_mul(int32_t a, int32_t b) { return (int32_t)((int64_t)a * b); }

uint64_t wyse_epoch_seconds(void) {
    EFI_TIME t;
    if (EFI_ERROR(gST->RuntimeServices->GetTime(&t, NULL))) return 0;
    struct tm tm = {t.Second, t.Minute, t.Hour, t.Day, t.Month - 1, t.Year - 1900, 0, 0, 0};
    return (uint64_t)mktime(&tm);
}

//--------------------------------------------------------------------+
// memory, log, ROMs
//--------------------------------------------------------------------+

void *wyse_pool_alloc(size_t n) {
    void *p;
    return EFI_ERROR(gBS->AllocatePool(EfiLoaderData, n, &p)) ? NULL : p;
}
void wyse_pool_free(void *p) { gBS->FreePool(p); }

// Large, page-aligned allocations (guest RAM wants 2 MB alignment for EPT)
void *pcmalloc(long size) {
    EFI_PHYSICAL_ADDRESS a;
    uint64_t pages = ((uint64_t)size + 0x1fffff) / 4096 + 512;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &a))) return NULL;
    void *p = (void *)((a + 0x1fffff) & ~0x1fffffULL);
    memset(p, 0, (size_t)size);
    return p;
}

static bool console_log = false;   // the screen shows the log only for a fatal error
void wyse_console_log(bool on) { console_log = on; }
void wyse_log_write(const char *s, size_t len) {
    // the UDP log takes whole lines; collect until newline
    static char line[1400];
    static size_t ll;
    for (size_t i = 0; i < len; i++) {
        if (ll < sizeof(line) - 1) line[ll++] = s[i];
        // a line that ends in \r (the installer's progress) is shown at once too
        if (s[i] == '\n' || (s[i] == '\r' && console_log) || ll == sizeof(line) - 1) {
            net_broadcast(5555, line, ll);
            if (console_log) {
                CHAR16 w[1410];
                size_t k = 0;
                for (size_t j = 0; j < ll; j++) {
                    if (line[j] == '\n') w[k++] = '\r';
                    w[k++] = (uint8_t)line[j];
                }
                w[k] = 0;
                gST->ConOut->OutputString(gST->ConOut, w);
            }
            ll = 0;
        }
    }
}

__attribute__((noreturn)) void wyse_fatal(const char *msg) {
    console_log = true;
    printf("wyseDOS fatal: %s\n", msg);
    for (;;) __asm__ volatile("hlt");
}

// SeaBIOS's clock_setup() as built for X86Pi turns the CMOS time into BIOS ticks
// as ms * 3579545 >> 16 / 3000, keeping the middle result in 32 bits; from 21:51
// on that overflows and DOS starts the day hours off. The same ticks come out of
// ms * 1193182 >> 16 / 1000, which stays in range all day: put those constants in.
static void patch_bios_clock(uint8_t *rom, size_t size) {
    static const uint8_t code[] = {
        0x69, 0xc0, 0xe8, 0x03, 0x00, 0x00,         // imul eax, eax, 1000
        0xba, 0x99, 0x9e, 0x36, 0x00,               // mov edx, 3579545
        0xf7, 0xe2,                                 // mul edx
        0x05, 0xff, 0xff, 0x00, 0x00,               // add eax, 0xffff
        0x83, 0xd2, 0x00,                           // adc edx, 0
        0x0f, 0xac, 0xd0, 0x10,                     // shrd eax, edx, 16
        0xc1, 0xea, 0x10,                           // shr edx, 16
        0x05, 0xb7, 0x0b, 0x00, 0x00,               // add eax, 2999
        0xb9, 0xb8, 0x0b, 0x00, 0x00,               // mov ecx, 3000
        0x31, 0xd2,                                 // xor edx, edx
        0xf7, 0xf1,                                 // div ecx
        0xa3, 0x6c, 0x04, 0x00, 0x00,               // mov [0x46c], eax
    };
    for (size_t i = 0; i + sizeof(code) <= size; i++) {
        if (memcmp(rom + i, code, sizeof(code))) continue;
        uint8_t *p = rom + i;
        p[7] = 0xde; p[8] = 0x34; p[9] = 0x12;      // 1193182
        p[29] = 0xe7; p[30] = 0x03;                 // 999
        p[34] = 0xe8; p[35] = 0x03;                 // 1000
        printf("rom: clock set-up patched at %05x\n", (unsigned)(0x100000 - size + i));
        return;
    }
}

uint32_t g_wyse_vga_rom_size;
int load_rom(void *phys_mem, const char *file, uword addr, int backward) {
    if (!phys_mem || !file || !file[0]) return -1;
    char path[256];
    snprintf(path, sizeof(path), "386/%s", file);
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK) return -1;
    FSIZE_t fs = f_size(&fp);
    if (fs == 0 || fs > 0x40000u) { f_close(&fp); return -1; }
    uint8_t *dest = (uint8_t *)phys_mem + (backward ? addr - (uword)fs : addr);
    UINT got = 0;
    FRESULT fr = f_read(&fp, dest, (UINT)fs, &got);
    f_close(&fp);
    printf("rom %s: %u bytes at %x\n", file, (unsigned)got, backward ? addr - (uword)fs : addr);
    if (backward && fr == FR_OK) patch_bios_clock(dest, got);
    if (!backward && addr==0xc0000 && fr==FR_OK) g_wyse_vga_rom_size=got;
    return (fr == FR_OK && got == fs) ? (int)fs : -1;
}

// USB floppy and CD drives: hostblk.c. The guest's NE2000 is bridged to the
// onboard NIC, whose MAC it takes (net.c keeps the monitor's own UDP apart).
void hostblk_list(void);
void hostblk_usb_start(void (*change)(int port, BlockDev *dev, unsigned serial));
static int usb_storage_enabled = 1;
static uint32_t firmware_timer_us = 5000; /* 0: retain firmware's hardware timer period */
static void guest_usb_change(int port, BlockDev *dev, unsigned serial) {
    extern PC *g_wyse_pc;
    if (g_wyse_pc) uhci_attach(g_wyse_pc->uhci, port, dev, serial);
}
static int guest_send(void *ctx, const uint8_t *buf, int len) { return net_guest_send(buf, len); }
static int guest_poll(void *ctx, uint8_t *buf, int cap) { return net_guest_poll(buf, cap); }

int net_bind_host(NE2000State *card) {
    uint8_t mac[6];
    if (!card || !net_guest_bind(mac)) return 0;
    ne2000_set_mac(card, mac);
    NE2000Host host = {guest_send, guest_poll, NULL};
    ne2000_set_host(&host);
    printf("ne2000: bridged to the onboard NIC, %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
    return 1;
}

//--------------------------------------------------------------------+
// time zone: the Wyse's RTC keeps UTC (Linux left it so), DOS wants local time.
// "timezone=cet" (the default) is Central European time with the EU's summer
// time, "timezone=utc" hands the RTC through.
//--------------------------------------------------------------------+

static bool tz_cet = true;

static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - (int)era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_seconds(int64_t t, CmosDateTime *dt) {
    int64_t z = (t >= 0 ? t : t - 86399) / 86400 + 719468;
    int secs = (int)(t - (z - 719468) * 86400);
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int d = doy - (153 * mp + 2) / 5 + 1;
    int m = mp + (mp < 10 ? 3 : -9);
    dt->year = (int)(yoe + era * 400) + (m <= 2);
    dt->month = m;
    dt->day = d;
    dt->hour = secs / 3600;
    dt->min = secs / 60 % 60;
    dt->sec = secs % 60;
}

// EU summer time: from 01:00 UTC on the last Sunday of March to 01:00 UTC on the last Sunday of October
static int64_t last_sunday_1am(int year, int month) {
    int64_t day = days_from_civil(year, month, 31);
    int wday = (int)((day + 4) % 7);            // 1970-01-01 was a Thursday; 0 = Sunday
    return (day - wday) * 86400 + 3600;
}

static int tz_offset(int64_t utc, int year) {
    if (!tz_cet) return 0;
    bool summer = utc >= last_sunday_1am(year, 3) && utc < last_sunday_1am(year, 10);
    return summer ? 7200 : 3600;
}

static void rtc_to_local(const EFI_TIME *t, CmosDateTime *dt) {
    int64_t utc = days_from_civil(t->Year, t->Month, t->Day) * 86400 + t->Hour * 3600 + t->Minute * 60 + t->Second;
    civil_from_seconds(utc + tz_offset(utc, t->Year), dt);
}

static void local_to_rtc(const CmosDateTime *dt, EFI_TIME *t) {
    int64_t local = days_from_civil(dt->year, dt->month, dt->day) * 86400 + dt->hour * 3600 + dt->min * 60 + dt->sec;
    int64_t utc = local - tz_offset(local - 3600, dt->year);
    CmosDateTime u;
    civil_from_seconds(utc, &u);
    t->Year = (uint16_t)u.year; t->Month = (uint8_t)u.month; t->Day = (uint8_t)u.day;
    t->Hour = (uint8_t)u.hour; t->Minute = (uint8_t)u.min; t->Second = (uint8_t)u.sec;
}

// The [pc] keys only wyseDOS reads, for config_save.c to write back
const char *config_platform_lines(void) {
    static char lines[128];
    snprintf(lines, sizeof(lines), "timezone=%s\nusb_storage=%d\nfirmware_timer_us=%u\n%s", tz_cet ? "cet" : "utc", usb_storage_enabled,
             firmware_timer_us,
             g_net_accept_broadcast ? "devctl=1\n" : "");
    return lines;
}

// FatFs time stamps: local time, packed the FAT way
uint32_t wyse_fat_time(void) {
    EFI_TIME t;
    CmosDateTime d = {2026, 1, 1, 0, 0, 0};
    if (!EFI_ERROR(gST->RuntimeServices->GetTime(&t, NULL))) rtc_to_local(&t, &d);
    if (d.year < 1980) d.year = 1980;
    return (uint32_t)(d.year - 1980) << 25 | (uint32_t)d.month << 21 | (uint32_t)d.day << 16 |
           (uint32_t)d.hour << 11 | (uint32_t)d.min << 5 | (uint32_t)d.sec / 2;
}

//--------------------------------------------------------------------+
// configuration ([pc] and [wyseDOS], with legacy [X86Pi] input support)
//--------------------------------------------------------------------+

typedef struct {
    char bios[128], vga_bios[128], fdd0[128], fdd1[128], ata0[128], ata1[128], ata2[128], ata3[128];
    int iscd[4];
    char mem[24], vga_mem[24];
    int cpu, fpu;
} cfg_t;

static long parse_size(const char *s) {
    char *end;
    long v = strtol(s, &end, 0);
    if (end == s || v <= 0) return 0;
    if (*end == 'K' || *end == 'k') v *= 1024L;
    else if (*end == 'M' || *end == 'm') v *= 1024L * 1024L;
    return v;
}

static void copy_value(char *dst, size_t cap, const char *v) {
    while (*v && isspace((unsigned char)*v)) v++;
    size_t n = strlen(v);
    while (n && isspace((unsigned char)v[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, v, n);
    dst[n] = 0;
}

static int cfg_handler(void *user, const char *section, const char *name, const char *value) {
    cfg_t *c = user;
    if (!strcmp(name, "devctl")) g_net_accept_broadcast = atoi(value) != 0;
    else if (!strcmp(name, "usb_storage")) usb_storage_enabled = atoi(value) != 0;
    else if (!strcmp(name, "firmware_timer_us")) {
        char *end;
        unsigned long us=strtoul(value,&end,10);
        if (end!=value && !*end && us<=1000000) firmware_timer_us=(uint32_t)us;
    }
    else if (!strcmp(name, "hwvga")) {
        extern int g_hwvga_wanted;
        g_hwvga_wanted = atoi(value) != 0;
    }
    else if (!strcmp(name, "timezone")) tz_cet = strcmp(value, "utc") && strcmp(value, "UTC");
    else if (!strcmp(name, "bios")) copy_value(c->bios, sizeof(c->bios), value);
    else if (!strcmp(name, "vga_bios")) copy_value(c->vga_bios, sizeof(c->vga_bios), value);
    else if (!strcmp(name, "fda")) copy_value(c->fdd0, sizeof(c->fdd0), value);
    else if (!strcmp(name, "fdb")) copy_value(c->fdd1, sizeof(c->fdd1), value);
    else if (!strcmp(name, "hda")) { copy_value(c->ata0, sizeof(c->ata0), value); c->iscd[0] = 0; }
    else if (!strcmp(name, "hdb")) { copy_value(c->ata1, sizeof(c->ata1), value); c->iscd[1] = 0; }
    else if (!strcmp(name, "hdc")) { copy_value(c->ata2, sizeof(c->ata2), value); c->iscd[2] = 0; }
    else if (!strcmp(name, "hdd")) { copy_value(c->ata3, sizeof(c->ata3), value); c->iscd[3] = 0; }
    else if (!strcmp(name, "cda")) { copy_value(c->ata0, sizeof(c->ata0), value); c->iscd[0] = 1; }
    else if (!strcmp(name, "cdb")) { copy_value(c->ata1, sizeof(c->ata1), value); c->iscd[1] = 1; }
    else if (!strcmp(name, "cdc")) { copy_value(c->ata2, sizeof(c->ata2), value); c->iscd[2] = 1; }
    else if (!strcmp(name, "cdd")) { copy_value(c->ata3, sizeof(c->ata3), value); c->iscd[3] = 1; }
    else if (!strcmp(name, "mem")) copy_value(c->mem, sizeof(c->mem), value);
    else if (!strcmp(name, "vga_mem")) copy_value(c->vga_mem, sizeof(c->vga_mem), value);
    else if (!strcmp(name, "cpu") || !strcmp(name, "gen")) c->cpu = atoi(value);
    else if (!strcmp(name, "fpu")) c->fpu = atoi(value) != 0;
    else if (!strcmp(section, "X86Pi") || !strcmp(section, "wyseDOS")) config_set_from_ini(name, value);
    return 1;
}

static bool load_config(PCConfig *conf) {
    static cfg_t c;
    static char text[16385];
    FIL f;
    if (f_open(&f, "386/config.ini", FA_READ) != FR_OK) return false;
    UINT got = 0;
    f_read(&f, text, sizeof(text) - 1, &got);
    f_close(&f);
    text[got] = 0;
    memset(&c, 0, sizeof(c));
    c.cpu = 5;
    c.fpu = 1;
    if (ini_parse_string(text, cfg_handler, &c) != 0 || !c.bios[0]) return false;
    memset(conf, 0, sizeof(*conf));
    conf->bios = c.bios;
    conf->vga_bios = c.vga_bios;
    conf->fdd[0] = c.fdd0;
    conf->fdd[1] = c.fdd1;
    conf->ata[0] = c.ata0;
    conf->ata[1] = c.ata1;
    conf->ata[2] = c.ata2;
    conf->ata[3] = c.ata3;
    for (int i = 0; i < 4; i++) conf->iscd[i] = c.iscd[i];
    conf->cpu_gen = c.cpu;
    conf->fpu = c.fpu;
    conf->usb_storage = usb_storage_enabled;
    conf->mem_size = parse_size(c.mem);
    if (!conf->mem_size) conf->mem_size = 16L << 20;
    conf->vga_mem_size = parse_size(c.vga_mem);
    if (!conf->vga_mem_size) conf->vga_mem_size = 4 << 20;
    conf->width = 640;
    conf->height = 480;
    conf->vga_force_8dm = 1;
    // a USB floppy drive is drive A, a USB CD drive the secondary master, and
    // USB flash / external disks attach to the free slave ATA slots by default
    if (!conf->fdd[0][0] && blk_host_available("ufd1")) conf->fdd[0] = "usb:ufd1";
    if (!conf->fdd[1][0] && blk_host_available("ufd2")) conf->fdd[1] = "usb:ufd2";
    if (!conf->ata[2][0] && blk_host_available("ucd1")) {
        conf->ata[2] = "usb:ucd1";
        conf->iscd[2] = 1;
    }
    if (!conf->usb_storage && blk_host_available("uhd1")) {
        if (!conf->ata[1][0]) { conf->ata[1] = "usb:uhd1"; conf->iscd[1] = 0; }
        else if (!conf->ata[3][0] && strcmp(conf->ata[1], "usb:uhd1") != 0) {
            conf->ata[3] = "usb:uhd1";
            conf->iscd[3] = 0;
        }
    }
    if (!conf->usb_storage && blk_host_available("uhd2") && !conf->ata[3][0]) {
        conf->ata[3] = "usb:uhd2";
        conf->iscd[3] = 0;
    }
    return true;
}

//--------------------------------------------------------------------+
// display: the model's 640x480 surface, scaled to the full height of the GOP
// framebuffer at 4:3 (1440x1080 on a 1080p screen), nearest neighbour
//--------------------------------------------------------------------+

/* Capacity is fixed; the display core selects the active stride and size.
 * Legacy DOS keeps its original 640x480 canvas and scaling. */
#define SURF_MAX_W 1920
#define SURF_MAX_H 1200
#define MAX_FB_W   4096
static uint32_t surf_w = 640, surf_h = 480;
#define SURF_W surf_w
#define SURF_H surf_h
static uint32_t *surface, *shown;
static uint32_t *gop_fb;
static uint32_t gop_w, gop_h, gop_stride;
static uint32_t dst_w, dst_h, off_x, off_y;
static uint16_t *col_src;       // destination column -> source column

static uint64_t t_redraw, n_redraw, px_redraw, t_step, t_hid, t_ctl, n_loops;
static void redraw_body(int x, int y, int w, int h);
static void redraw(void *opaque, int x, int y, int w, int h) {
    (void)opaque;
    uint64_t t0 = rdtsc_();
    redraw_body(x, y, w, h);
    t_redraw += rdtsc_() - t0;
    n_redraw++;
    px_redraw += (uint64_t)(w > 0 ? w : 0) * (h > 0 ? h : 0);
}
static void redraw_body(int x, int y, int w, int h) {
    if (!gop_fb) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > SURF_W) w = SURF_W - x;
    if (y + h > SURF_H) h = SURF_H - y;
    if (w <= 0 || h <= 0) return;
    // destination span covering the source rectangle
    uint32_t dx0 = (uint32_t)x * dst_w / SURF_W, dx1 = ((uint32_t)(x + w) * dst_w + SURF_W - 1) / SURF_W;
    uint32_t dy0 = (uint32_t)y * dst_h / SURF_H, dy1 = ((uint32_t)(y + h) * dst_h + SURF_H - 1) / SURF_H;
    if (dx1 > dst_w) dx1 = dst_w;
    if (dx1 > MAX_FB_W) dx1 = MAX_FB_W;
    if (dx0 >= dx1) return;
    if (dy1 > dst_h) dy1 = dst_h;
    (void)dy0;
    (void)dy1;
    static uint32_t line[MAX_FB_W];
    for (int sy = y; sy < y + h; sy++) {
        // the renderer hands over whole frames; only rows that changed go out
        const uint32_t *src = surface + sy * SURF_W;
        uint32_t *seen = shown + sy * SURF_W;
        if (!memcmp(src + x, seen + x, (size_t)w * 4)) continue;
        memcpy(seen + x, src + x, (size_t)w * 4);
        for (uint32_t dx = dx0; dx < dx1; dx++) line[dx] = src[col_src[dx]];
        // destination rows whose source row is sy
        uint32_t ya = ((uint32_t)sy * dst_h + SURF_H - 1) / SURF_H;
        uint32_t yb = ((uint32_t)(sy + 1) * dst_h + SURF_H - 1) / SURF_H;
        for (uint32_t dy = ya; dy < yb && dy < dst_h; dy++) {
            // dword stores: memcpy's rep movsb would be byte-wise on device memory
            void *d = gop_fb + (off_y + dy) * gop_stride + off_x + dx0;
            const void *sp = line + dx0;
            uint64_t n = dx1 - dx0;
            __asm__ volatile("rep movsl" : "+D"(d), "+S"(sp), "+c"(n) :: "memory");
        }
    }
}

/* Prefer the EDID attached to this GOP, so a second output cannot supply
 * the identity of the wrong monitor. Firmware may expose only Discovered. */
static void display_edid(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
    static const EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    static const EFI_GUID guids[2] = {
        {0xbd8c1056,0x9f36,0x44ec,{0x92,0xa8,0xa6,0x33,0x7f,0x81,0x79,0x86}},
        {0x1c0c34f6,0xd380,0x41fa,{0xa0,0x49,0x8a,0xd0,0x6c,0x1a,0x66,0xaa}}
    };
    typedef struct { uint32_t size; uint8_t *data; } EdidProtocol;
    EFI_HANDLE *handles=NULL;uint64_t count=0;
    if (!EFI_ERROR(gBS->LocateHandleBuffer(2,&gop_guid,NULL,&count,&handles))) {
        for (uint64_t i=0;i<count;i++) {
            EFI_GRAPHICS_OUTPUT_PROTOCOL *candidate=NULL;
            if (EFI_ERROR(gBS->HandleProtocol(handles[i],&gop_guid,(void **)&candidate)) || candidate!=gop) continue;
            for (unsigned j=0;j<2;j++) {
                EdidProtocol *e=NULL;
                if (!EFI_ERROR(gBS->HandleProtocol(handles[i],&guids[j],(void **)&e)) &&
                    e && guest_edid_set(e->data,e->size)) {
                    char name[14]={0};const uint8_t *p=guest_edid_page();
                    for (unsigned off=54;off<=108;off+=18)
                        if (!p[off] && !p[off+1] && !p[off+2] && p[off+3]==0xfc) {
                            for (unsigned k=0;k<13;k++) {
                                uint8_t c=p[off+5+k];if (c<32 || c>126) break;name[k]=c;
                            }
                        }
                    printf("edid: %s, product %02x%02x, %u blocks (%s)\n",name[0]?name:"unnamed monitor",p[11],p[10],p[126]+1,j?"discovered":"active");
                    gBS->FreePool(handles);return;
                }
            }
        }
        gBS->FreePool(handles);
    }
    printf("edid: no valid EDID on the active GOP; DDC unavailable\n");
}

static void display_init(void) {
    static const EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    console_log = false;    // the screen belongs to the guest from here on
    surface = calloc(SURF_MAX_W * SURF_MAX_H, 4);
    shown = malloc(SURF_MAX_W * SURF_MAX_H * 4);
    memset(shown, 0xff, SURF_W * SURF_H * 4);   // nothing shown yet: every row differs
    if (EFI_ERROR(gBS->LocateProtocol(&gop_guid, NULL, (void **)&gop))) return;
    display_edid(gop);
    gop_fb = (uint32_t *)gop->Mode->FrameBufferBase;
    gop_w = gop->Mode->Info->HorizontalResolution;
    gop_h = gop->Mode->Info->VerticalResolution;
    gop_stride = gop->Mode->Info->PixelsPerScanLine;
    col_src = calloc(MAX_FB_W, sizeof(uint16_t));
    wc_map(gop->Mode->FrameBufferBase, gop->Mode->FrameBufferSize);
    printf("display: fb %llx size %llx, %ux%u stride %u\n", (unsigned long long)gop->Mode->FrameBufferBase,
           (unsigned long long)gop->Mode->FrameBufferSize, gop_w, gop_h, gop_stride);
}

// Where the 640x480 picture goes on the screen (the menu's "Picture"):
// 4:3 at the screen's full height, the whole screen (stretched), or sharp
// whole-number pixels. The firmware gives 1920x1080 at most, so on a bigger
// or wider monitor it is the monitor's scaler that takes it from there.
static int layout_mode = -1;

// The wyseDOS logo while the machine is put together (386/logo.bin, made by
// tools/logo/mklogo.py: "WDLG", width, height, background, a 256-colour
// palette and the pixels). The firmware's own logo cannot be replaced - the
// flash is locked against writes (BLE, SMM_BWP) - so this is the first
// picture of ours, until the PC's BIOS takes the screen.
static uint64_t splash_at;

static void splash_show(void) {
    FIL f;
    if (!gop_fb || f_open(&f, "386/logo.bin", FA_READ) != FR_OK) return;
    struct __attribute__((packed)) {
        char magic[4];
        uint16_t w, h;
        uint32_t bg;
    } hdr;
    static uint32_t pal[256];
    UINT got = 0;
    f_read(&f, &hdr, sizeof(hdr), &got);
    if (got != sizeof(hdr) || memcmp(hdr.magic, "WDLG", 4) || !hdr.w || hdr.w > gop_w || hdr.h > gop_h) {
        f_close(&f);
        return;
    }
    f_read(&f, pal, sizeof(pal), &got);
    uint8_t *px = malloc((size_t)hdr.w * hdr.h);
    if (!px) { f_close(&f); return; }
    UINT want = (UINT)hdr.w * hdr.h;
    f_read(&f, px, want, &got);
    f_close(&f);
    if (got == want) {
        uint32_t line_w = gop_w < MAX_FB_W ? gop_w : MAX_FB_W;
        uint32_t x0 = (line_w > hdr.w) ? (line_w - hdr.w) / 2 : 0, y0 = (gop_h - hdr.h) / 2;
        static uint32_t line[MAX_FB_W];
        for (uint32_t y = 0; y < gop_h; y++) {
            for (uint32_t x = 0; x < line_w; x++) line[x] = hdr.bg;
            if (y >= y0 && y < y0 + hdr.h)
                for (uint32_t x = 0; x < hdr.w && x0 + x < line_w; x++) line[x0 + x] = pal[px[(y - y0) * hdr.w + x]];
            void *d = gop_fb + y * gop_stride;
            const void *s = line;
            uint64_t n = line_w;
            __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
        }
        splash_at = wyse_ticks_us();
        printf("display: splash %ux%u\n", hdr.w, hdr.h);
    }
    free(px);
}

// Installer uses the same logo and framebuffer as startup.
void installer_ui_begin(void) { display_init(); splash_show(); }
static void install_rect(int x,int y,int w,int h,uint32_t color) {
    if (!gop_fb) return;
    for (int j=0;j<h;j++) for (int i=0;i<w;i++)
        if (x+i>=0 && y+j>=0 && (uint32_t)(x+i)<gop_w && (uint32_t)(y+j)<gop_h)
            gop_fb[(y+j)*gop_stride+x+i]=color;
}
void installer_ui_status(const char *step,const char *detail,unsigned percent) {
    extern const uint8_t font_8x16[4096];
    if (!gop_fb) return;
    int scale=gop_w>=1280?2:1;
    int w=gop_w>1280?1200:(int)gop_w-40, x=((int)gop_w-w)/2, y=(int)gop_h-112*scale/2;
    install_rect(x-12,y-8,w+24,112*scale/2,0x08121c);
    const char *lines[2]={step,detail};
    int max_chars=w/(8*scale);
    for (int row=0;row<2;row++) {
        if (!lines[row]) continue;
        int len=(int)strlen(lines[row]);
        if (len>max_chars) len=max_chars;
        int lx=x+(w-len*8*scale)/2;
        for (int k=0;k<len;k++) {
            unsigned ch=(unsigned char)lines[row][k];
            for (int j=0;j<16;j++) for (int i=0;i<8;i++) if (font_8x16[ch*16+j] & (1u<<i))
                install_rect(lx+k*8*scale+i*scale,y+row*19*scale+j*scale,scale,scale,row?0xb3c7d8:0xffffff);
        }
    }
    int by=y+40*scale, bh=8*scale;
    install_rect(x,by,w,bh,0x294154);
    install_rect(x,by,(int)((uint64_t)w*(percent>100?100:percent)/100),bh,0x48c9ed);
    __asm__ volatile("sfence" ::: "memory");
}

// The logo stays up at least two seconds, then the picture is the PC's
static void splash_end(void) {
    if (!splash_at) return;
    while (wyse_ticks_us() - splash_at < 2000000) __asm__ volatile("pause");
    splash_at = 0;
}

static void display_layout(int mode) {
    if (!gop_fb || mode == layout_mode) return;
    layout_mode = mode;
    uint32_t maxw = gop_w < MAX_FB_W ? gop_w : MAX_FB_W;
    if (mode == PICTURE_FILL) {
        dst_w = maxw;
        dst_h = gop_h;
    } else if (mode == PICTURE_SHARP_2X) {
        uint32_t k = maxw / SURF_W < gop_h / SURF_H ? maxw / SURF_W : gop_h / SURF_H;
        if (k) {
            dst_w = SURF_W * k;
            dst_h = SURF_H * k;
        } else {
            dst_w=maxw;dst_h=(uint64_t)maxw*SURF_H/SURF_W;
            if (dst_h>gop_h) { dst_h=gop_h;dst_w=(uint64_t)gop_h*SURF_W/SURF_H; }
        }
    } else {
        dst_h = gop_h;
        dst_w = gop_h * 4 / 3;
        if (dst_w > maxw) {
            dst_w = maxw;
            dst_h = dst_w * 3 / 4;
        }
    }
    off_x = (gop_w - dst_w) / 2;
    off_y = (gop_h - dst_h) / 2;
    for (uint32_t dx = 0; dx < dst_w && dx < MAX_FB_W; dx++) col_src[dx] = (uint16_t)(dx * SURF_W / dst_w);
    // black around it, and every row drawn again
    for (uint32_t y = 0; y < gop_h; y++) {
        void *d = gop_fb + y * gop_stride;
        uint64_t n = gop_w;
        uint32_t zero = 0;
        __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(zero) : "memory");
    }
    memset(shown, 0xff, SURF_W * SURF_H * 4);
    printf("display: picture %ux%u at %u,%u (%s)\n", dst_w, dst_h, off_x, off_y,
           mode == PICTURE_FILL ? "fill" : mode == PICTURE_SHARP_2X ? "sharp" : "4:3");
}

/* Called only by the renderer (or with the renderer held by the main loop).
 * All storage is allocated before starting it; no UEFI calls on an AP. */
void wyse_vga_surface(VGAState *v) {
    uint32_t w=640,h=480;
    if (vga_vbe_on(v)) {
        uint32_t vw=v->vbe_regs[VBE_DISPI_INDEX_XRES], vh=v->vbe_regs[VBE_DISPI_INDEX_YRES];
        if (vw>=640 && vh>=480 && vw<=SURF_MAX_W && vh<=SURF_MAX_H) { w=vw;h=vh; }
    }
    if (w==surf_w && h==surf_h) return;
    surf_w=w;surf_h=h;
    v->fb_dev->width=w;v->fb_dev->height=h;v->fb_dev->stride=w*4;
    memset(surface,0,(size_t)w*h*4);
    int mode=layout_mode;layout_mode=-1;display_layout(mode<0?g_picture:mode);
    printf("display: guest surface %ux%u\n",w,h);
}

//--------------------------------------------------------------------+
// keyboard: UEFI console input -> PS/2 (press and release per keystroke)
//--------------------------------------------------------------------+

// Linux/AT key codes, as X86Pi's ps2_put_keycode() takes them
static int ascii_keycode(int c, bool *shift) {
    static const char *row1 = "1234567890-=", *row2 = "qwertyuiop[]", *row3 = "asdfghjkl;'`", *row4 = "\\zxcvbnm,./";
    static const char *srow1 = "!@#$%^&*()_+", *srow2 = "QWERTYUIOP{}", *srow3 = "ASDFGHJKL:\"~", *srow4 = "|ZXCVBNM<>?";
    const char *p;
    *shift = false;
    if (c == ' ') return 57;
    if (c == '\r' || c == '\n') return 28;
    if (c == '\t') return 15;
    if (c == 8) return 14;
    if (c == 27) return 1;
    if ((p = strchr(row1, c)) && c) return 2 + (int)(p - row1);
    if ((p = strchr(row2, c)) && c) return 16 + (int)(p - row2);
    if ((p = strchr(row3, c)) && c) return 30 + (int)(p - row3);
    if ((p = strchr(row4, c)) && c) return 43 + (int)(p - row4);
    *shift = true;
    if ((p = strchr(srow1, c)) && c) return 2 + (int)(p - srow1);
    if ((p = strchr(srow2, c)) && c) return 16 + (int)(p - srow2);
    if ((p = strchr(srow3, c)) && c) return 30 + (int)(p - srow3);
    if ((p = strchr(srow4, c)) && c) return 43 + (int)(p - srow4);
    *shift = false;
    return 0;
}

static int scan_keycode(int scan) {
    switch (scan) {
    case 1: return 103;   // up
    case 2: return 108;   // down
    case 3: return 106;   // right
    case 4: return 105;   // left
    case 5: return 102;   // home
    case 6: return 107;   // end
    case 7: return 110;   // insert
    case 8: return 111;   // delete
    case 9: return 104;   // page up
    case 10: return 109;  // page down
    case 0x17: return 1;  // escape
    default:
        if (scan >= 0x0b && scan <= 0x14) return 59 + (scan - 0x0b);   // F1..F10
        if (scan == 0x15) return 87;
        if (scan == 0x16) return 88;
        return 0;
    }
}

static void poll_keyboard(PC *pc) {
    EFI_INPUT_KEY key;
    // the firmware's keyboard driver fills its buffer from the WaitForKey check
    if (EFI_ERROR(gBS->CheckEvent(gST->ConIn->WaitForKey))) return;
    while (!EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) {
        printf("key: scan %x char %x\n", key.ScanCode, key.UnicodeChar);
        bool shift = false;
        int code = key.ScanCode ? scan_keycode(key.ScanCode) : 0;
        if (!code && key.UnicodeChar) {
            int c = key.UnicodeChar;
            if (c >= 1 && c <= 26 && c != 8 && c != 9 && c != 13) {
                // Ctrl+letter
                ps2_put_keycode(pc->kbd, 1, 29);
                ps2_put_keycode(pc->kbd, 1, ascii_keycode('a' + c - 1, &shift));
                ps2_put_keycode(pc->kbd, 0, ascii_keycode('a' + c - 1, &shift));
                ps2_put_keycode(pc->kbd, 0, 29);
                continue;
            }
            code = ascii_keycode(c, &shift);
        }
        if (!code) continue;
        if (shift) ps2_put_keycode(pc->kbd, 1, 42);
        ps2_put_keycode(pc->kbd, 1, code);
        ps2_put_keycode(pc->kbd, 0, code);
        if (shift) ps2_put_keycode(pc->kbd, 0, 42);
    }
}

static const EFI_GUID global_var = EFI_GLOBAL_VARIABLE_GUID;
enum { REBOOT_SAME, REBOOT_PXE, REBOOT_ORDER };

static void set_boot_next(uint16_t num) {
    gST->RuntimeServices->SetVariable((const CHAR16 *)L"BootNext", &global_var,
                                      EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                          EFI_VARIABLE_RUNTIME_ACCESS,
                                      sizeof(num), &num);
}

// The boot option that netboots over IPv4 (a MAC and an IPv4 node in its path)
static int pxe_boot_option(void) {
    static uint16_t order[64];
    uint64_t size = sizeof(order);
    if (EFI_ERROR(gST->RuntimeServices->GetVariable((const CHAR16 *)L"BootOrder", &global_var, NULL, &size, order)))
        return -1;
    for (unsigned i = 0; i < size / 2; i++) {
        static const char hex[] = "0123456789ABCDEF";
        CHAR16 name[9] = {'B', 'o', 'o', 't', hex[order[i] >> 12 & 15], hex[order[i] >> 8 & 15],
                          hex[order[i] >> 4 & 15], hex[order[i] & 15], 0};
        static uint8_t opt[1024];
        uint64_t n = sizeof(opt);
        if (EFI_ERROR(gST->RuntimeServices->GetVariable(name, &global_var, NULL, &n, opt)) || n < 8) continue;
        // attributes, path length, the description (UTF-16, NUL-ended), then the path
        uint64_t at = 6;
        while (at + 1 < n && (opt[at] || opt[at + 1])) at += 2;
        EFI_DEVICE_PATH *dp = (EFI_DEVICE_PATH *)(opt + at + 2);
        bool mac = false, ipv4 = false;
        for (; (uint8_t *)dp + 4 <= opt + n && !dp_end(dp) && dp->Length >= 4; dp = dp_next(dp)) {
            if (dp->Type == DP_MESSAGING && dp->SubType == DP_MSG_MAC) mac = true;
            if (dp->Type == DP_MESSAGING && dp->SubType == DP_MSG_IPV4) ipv4 = true;
        }
        if (mac && ipv4) return order[i];
    }
    return -1;
}

// What the guest wrote, out to the disk; then a cold reset of the Wyse. A
// netbooted monitor asks to be netbooted again (BootNext = BootCurrent), so
// the development loop keeps working when wyseDOS is first in the boot order;
// REBOOT_PXE asks for the network boot whatever started this one,
// REBOOT_ORDER for the firmware's boot order.
static void wyse_reboot_to(int how) {
    ide_sync_idle(1);
    blkcache_flush(NULL);
    if (how == REBOOT_PXE) {
        int n = pxe_boot_option();
        if (n >= 0) set_boot_next((uint16_t)n);
        printf("reboot: network boot option %d\n", n);
    } else if (how == REBOOT_SAME && fs_default_backend() == FS_NETFS) {
        uint16_t current;
        uint64_t size = sizeof(current);
        if (!EFI_ERROR(gST->RuntimeServices->GetVariable((const CHAR16 *)L"BootCurrent", &global_var, NULL, &size,
                                                         &current)))
            set_boot_next(current);
    }
    gST->RuntimeServices->ResetSystem(EfiResetCold, 0, 0, NULL);
}

static void wyse_reboot(void) { wyse_reboot_to(REBOOT_SAME); }

static void wyse_poweroff(void) {
    extern volatile int g_audio_pause;
    printf("power: guest shutdown complete, synchronizing disks\n");
    ide_sync_idle(1);
    blkcache_flush(NULL);
    g_audio_pause=1;
    if (g_wyse_vga_core) pc_vga_hold(1);
    gBS->SetWatchdogTimer(0,0,0,NULL);
    printf("power: guest power-off -> UEFI ResetShutdown\n");
    gST->RuntimeServices->ResetSystem(EfiResetShutdown,0,0,NULL);
    printf("power: firmware returned from ResetShutdown\n");
    for (;;) __asm__ volatile("hlt");
}

//--------------------------------------------------------------------+
// control port: commands from the development PC (tools/rig/wysectl.py)
//--------------------------------------------------------------------+

static void report(PC *pc, uint64_t elapsed_us);

static void poll_control(PC *pc) {
    size_t len;
    const uint8_t *p = net_poll(5557, &len);
    if (!p) return;
    char cmd[64];
    if (len >= sizeof(cmd)) len = sizeof(cmd) - 1;
    memcpy(cmd, p, len);
    cmd[len] = 0;
    // wysectl.py sends each command several times (unicast and broadcast)
    // with a " #nonce" on the end: one with the nonce just seen is a repeat
    static char last_nonce[16];
    char *hash = strrchr(cmd, '#');
    if (hash) {
        if (!strcmp(hash + 1, last_nonce)) return;
        snprintf(last_nonce, sizeof(last_nonce), "%s", hash + 1);
        while (hash > cmd && hash[-1] == ' ') hash--;
        *hash = 0;
    }
    printf("control: %s\n", cmd);
    if (!strncmp(cmd, "reboot", 6)) {
        // "reboot pxe": netboot next; "reboot disk": the boot order (wyseDOS on the eMMC)
        wyse_reboot_to(strstr(cmd, "pxe") ? REBOOT_PXE : strstr(cmd, "disk") ? REBOOT_ORDER : REBOOT_SAME);
    } else if (!strncmp(cmd, "reset", 5)) {
        pc->reset_request = 1;
    } else if (!strncmp(cmd, "stats", 5)) {
        report(pc, 10000000);
    } else if (!strncmp(cmd, "clock", 5)) {
        CmosDateTime dt;
        EFI_TIME t;
        cmos_get_clock(pc->cmos, &dt);
        gST->RuntimeServices->GetTime(&t, NULL);
        uint32_t ticks = *(uint32_t *)(pc->phys_mem + 0x46c);
        uint32_t ts = (uint32_t)((uint64_t)ticks * 10000 / 182065);
        printf("clock: cmos %04d-%02d-%02d %02d:%02d:%02d, rtc %04u-%02u-%02u %02u:%02u:%02u, "
               "bios ticks %u (%02u:%02u:%02u)\n", dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec, t.Year,
               t.Month, t.Day, t.Hour, t.Minute, t.Second, ticks, ts / 3600, ts / 60 % 60, ts % 60);
    } else if (!strncmp(cmd, "peek", 4)) {
        // "peek c8000": a map of the 64 KB from there, one character per 1 KB:
        // F all FF, 0 all zero, R a ROM signature at the start, x anything else
        uint32_t a = (uint32_t)strtoul(cmd + 4, NULL, 16) & ~0x3ffu;
        char map[65];
        for (int k = 0; k < 64; k++) {
            const uint8_t *p = (const uint8_t *)pc->phys_mem + a + k * 1024;
            int ff = 1, zero = 1;
            for (int i = 0; i < 1024; i++) ff &= p[i] == 0xff, zero &= p[i] == 0;
            map[k] = p[0] == 0x55 && p[1] == 0xaa ? 'R' : ff ? 'F' : zero ? '0' : 'x';
        }
        map[64] = 0;
        printf("peek %05x: %s\n", a, map);
    } else if (!strncmp(cmd, "hwvga", 5)) {
        void hwvga_report(void);
        void hwvga_peek(uint32_t dw);
        void hwvga_lag(void);
        void hwvga_test(void);
        void hwvga_unaligned(void);
        void wyse_cpu_vga_safety(int enabled);
        if (!strncmp(cmd+5," safe ",6)) wyse_cpu_vga_safety(atoi(cmd+11));
        else if (!strcmp(cmd+5," unaligned")) hwvga_unaligned();
        else if (!strncmp(cmd + 5, " lag", 4)) hwvga_lag();
        else if (!strncmp(cmd + 5, " test", 5)) hwvga_test();
        else if (cmd[5] == ' ') hwvga_peek((uint32_t)strtoul(cmd + 6, NULL, 16));
        else hwvga_report();
    } else if (!strncmp(cmd, "redeliver", 9)) {
        extern int g_redeliver;
        g_redeliver = atoi(cmd + 9);
    } else if (!strncmp(cmd, "iotrace", 7)) {
        void wyse_cpu_iotrace(const char *args);
        wyse_cpu_iotrace(cmd + 7);
    } else if (!strncmp(cmd, "mtf", 3)) {
        void wyse_cpu_mtf(uint32_t eip, uint32_t nth);
        char *end;
        uint32_t eip = (uint32_t)strtoul(cmd + 3, &end, 16);
        wyse_cpu_mtf(eip, (uint32_t)strtoul(end, NULL, 10));
    } else if (!strncmp(cmd, "gwatch", 6)) {
        extern uint32_t g_gwatch, g_gwatch_left;
        g_gwatch = (uint32_t)strtoul(cmd + 6, NULL, 16);
        g_gwatch_left = 2;
    } else if (!strncmp(cmd, "gmem", 4)) {
        void wyse_cpu_gmem(uint32_t lin, uint32_t n);
        char *end;
        uint32_t lin = (uint32_t)strtoul(cmd + 4, &end, 16);
        wyse_cpu_gmem(lin, (uint32_t)strtoul(end, NULL, 10));
    } else if (!strncmp(cmd, "trapexc", 7)) {
        void wyse_cpu_trap_exceptions(int on);
        wyse_cpu_trap_exceptions(atoi(cmd + 7));
    } else if (!strncmp(cmd, "iopass", 6)) {
        extern int g_io_pass;
        g_io_pass = atoi(cmd + 6);
        printf("iopass %d\n", g_io_pass);
    } else if (!strcmp(cmd,"rom")) {
        void wyse_cpu_rom_report(void);wyse_cpu_rom_report();
    } else if (!strcmp(cmd,"edid")) {
        const uint8_t *e=guest_edid_page();unsigned sum=0;
        for (unsigned i=0;i<128;i++) sum+=e[i];
        printf("edid: BAR %08x header %02x%02x product %02x%02x checksum %s\n",pc->pci_vga_edid_addr,e[0],e[1],e[11],e[10],!(sum&255)&&e[1]==255?"PASS":"unavailable");
        if (e[1]==255) {
            unsigned found=0;
            for (unsigned a=0xc0000;a+128<=0xd0000;a++)
                if (!memcmp(pc->phys_mem+a,e,128)) { printf("edid: identical VGA BIOS DDC copy at %05x\n",a);found++; }
            if (!found) printf("edid: VGA BIOS has no matching DDC copy\n");
        }
    } else if (!strcmp(cmd,"acpi")) {
        void acpi_report(void);acpi_report();
    } else if (!strncmp(cmd, "hostirq", 7)) {
        void firmware_events_report(void);
        void firmware_events_recover(void);
        if (strstr(cmd,"recover")) firmware_events_recover();
        firmware_events_report();
        // the host's interrupts: whether the firmware's timer still runs
        uint64_t fl;
        __asm__ volatile("pushfq; popq %0" : "=r"(fl));
        uint8_t imr0, imr1;
        __asm__ volatile("inb $0x21, %0" : "=a"(imr0));
        __asm__ volatile("inb $0xa1, %0" : "=a"(imr1));
        volatile uint32_t *lapic = (volatile uint32_t *)0xfee00000;
        EFI_EVENT ev;
        int fired = -1;
        if (!EFI_ERROR(gBS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &ev))) {
            gBS->SetTimer(ev, TimerRelative, 100000);       // 10 ms
            uint64_t t0 = wyse_ticks_us();
            fired = 0;
            while (wyse_ticks_us() - t0 < 100000)
                if (gBS->CheckEvent(ev) == EFI_SUCCESS) { fired = (int)(wyse_ticks_us() - t0); break; }
            gBS->CloseEvent(ev);
        }
        printf("hostirq: IF %d, 8259 imr %02x %02x, lapic lvt timer %08x count %08x tpr %02x, 10 ms timer event %s %d us\n",
               (int)(fl >> 9 & 1), imr0, imr1, lapic[0x320 / 4], lapic[0x390 / 4], lapic[0x80 / 4],
               fired > 0 ? "fired after" : "did NOT fire", fired);
    } else if (!strncmp(cmd, "hiddebug", 8)) {
        extern int hid_debug;
        hid_debug = atoi(cmd + 8);
    } else if (!strncmp(cmd,"fddhash ",8)) {
        int drive=atoi(cmd+8);
        BlockDev *dev=drive>=0 && drive<2?fdd_get_dev(drive):NULL;
        if (!diskui_is_open() || !dev || !blk_size(dev)) {
            printf("fddhash: pause in Disk Manager and attach a floppy first\n");
        } else {
            static uint8_t data[8192];
            uint64_t saved=dev->pos,size=blk_size(dev),hash=14695981039346656037ULL;
            int ok=1;
            for (uint64_t off=0;off<size;) {
                unsigned n=size-off>sizeof(data)?sizeof(data):(unsigned)(size-off);
                if (blk_pread(dev,off,data,n)!=(int)n) { ok=0; break; }
                for (unsigned i=0;i<n;i++) hash=(hash^data[i])*1099511628211ULL;
                off+=n;
            }
            if (blk_seek(dev,saved)<0) ok=0;
            printf("fddhash: %d size %llu FNV64 %016llx %s\n",drive,
                   (unsigned long long)size,(unsigned long long)hash,ok?"OK":"FAILED");
        }
    } else if (!strcmp(cmd, "usb")) {
        uhci_report(pc->uhci);
    } else if (!strncmp(cmd, "usb eject ", 10)) {
        printf("usb: eject %s\n", hostblk_usb_eject(atoi(cmd + 10)) ? "OK" : "refused");
    } else if (!strncmp(cmd, "usb insert ", 11)) {
        char *which;
        long port = strtol(cmd + 11, &which, 10);
        if (which != cmd + 11 && *which == ' ' && port >= 0 && port < 2)
            printf("usb: insert %s\n", hostblk_usb_insert((int)port, which + 1) ? "OK" : "refused");
        else
            printf("usb: usage: usb insert <0|1> uhdN\n");
    } else if (!strncmp(cmd, "hid", 3)) {
        void usbhid_report(void);
        usbhid_report();
        ps2_mouse_report(pc->mouse);
    } else if (!strncmp(cmd, "gtt", 3)) {
        void hwvga_gtt(uint32_t first);
        hwvga_gtt((uint32_t)strtoul(cmd + 3, NULL, 10));
    } else if (!strncmp(cmd, "ddr ", 4)) {
        void display_ddr(const char *how);
        display_ddr(cmd + 4);
    } else if (!strncmp(cmd, "fbpeek", 6)) {
        // "fbpeek 1000": 16 pixels across that row of the framebuffer, and the picture's place
        uint32_t y = (uint32_t)strtoul(cmd + 6, NULL, 10);
        if (gop_fb && y < gop_h) {
            char buf[256];
            int len = snprintf(buf, sizeof(buf), "fbpeek %u:", y);
            for (int i = 0; i < 16; i++)
                len += snprintf(buf + len, sizeof(buf) - len, " %06x",
                                gop_fb[y * gop_stride + (uint32_t)i * gop_w / 16 + 4] & 0xffffff);
            printf("%s (picture %ux%u at %u,%u)\n", buf, dst_w, dst_h, off_x, off_y);
        }
    } else if (!strncmp(cmd, "vgamem", 6)) {
        // "vgamem 3cc0": the four plane bytes of 16 addresses there, a line at a time
        uint32_t a = (uint32_t)strtoul(cmd + 6, NULL, 16);
        VGAState *v = pc->vga;
        for (int row = 0; row < 4; row++) {
            char buf[200];
            int len = snprintf(buf, sizeof(buf), "vgamem %04x:", a + row * 16);
            for (int i = 0; i < 16; i++) {
                uint32_t o = ((a + row * 16 + i) * 4u) % (uint32_t)v->vga_ram_size;
                len += snprintf(buf + len, sizeof(buf) - len, " %02x%02x%02x%02x", v->vga_ram[o],
                                v->vga_ram[o + 1], v->vga_ram[o + 2], v->vga_ram[o + 3]);
            }
            printf("%s\n", buf);
        }
    } else if (!strncmp(cmd, "vga", 3)) {
        // the VGA registers and the last frames' start/split/pan, line by line
        static char dump[8192];
        vga_dump_state(pc->vga, dump, sizeof(dump));
        for (char *line = dump, *nl; *line; line = nl ? nl + 1 : line + strlen(line)) {
            nl = strchr(line, '\n');
            if (nl) *nl = 0;
            printf("vga| %s\n", line);
        }
        for (int i = 40; i >= 0; i--) {
            char buf[80];
            if (vga_reg_ring_line(i, buf, sizeof(buf))) printf("vgareg| %s\n", buf);
        }
    } else if (!strncmp(cmd, "fswr", 4)) {
        storage_debug(cmd + 4, true);
    } else if (!strncmp(cmd, "fs ", 3)) {
        storage_debug(cmd + 3, false);
    } else if (!strncmp(cmd, "token ", 6)) {
        // development: turn a Dell token on ("token 00ea")
        uint16_t id = (uint16_t)strtoul(cmd + 6, NULL, 16);
        bool ok = dell_token_activate(id);
        printf("dell: token %04x %s, now %s\n", id, ok ? "written" : "NOT written", dell_token_active(id) ? "on" : "off");
    } else if (!strncmp(cmd, "tokens", 6)) {
        void dell_token_report(void);
        dell_token_report();
    } else if (!strncmp(cmd, "dmi", 3)) {
        void installer_dmi_report(void);
        installer_dmi_report();
    } else if (!strcmp(cmd, "nativecheck")) {
        void native_disk_test(void); native_disk_test();
    } else if (!strcmp(cmd, "emmcbackup")) {
        void installer_backup_emmc(void); installer_backup_emmc();
    } else if (!strncmp(cmd, "emmcupd", 7)) {
        installer_update_emmc(cmd[7] == ' ' ? cmd + 8 : NULL);
    } else if (!strncmp(cmd, "nfbench", 7)) {
        storage_file_bench();
    } else if (!strncmp(cmd, "bench", 5)) {
        storage_bench(cmd + 5);
    } else if (!strncmp(cmd, "blk", 3)) {
        storage_list();
        hostblk_list();
    } else if (!strncmp(cmd, "sspmode", 7)) {
        void analog_set_ssp_mode(int mode);
        analog_set_ssp_mode(atoi(cmd + 7));
    } else if (!strncmp(cmd, "hpvol", 5)) {
        void analog_set_volume(int step);
        analog_set_volume(atoi(cmd + 5));
    } else if (!strncmp(cmd, "tone", 4)) {
        extern volatile uint32_t g_audio_tone_frames;
        extern int g_audio_tone_shift;
        char *end;
        int sec = (int)strtol(cmd + 4, &end, 10);
        g_audio_tone_shift = (int)strtol(end, NULL, 10) & 15;   // "tone 5 4": 5 s, 24 dB quieter
        g_audio_tone_frames = (uint32_t)(sec > 0 ? sec : 5) * SOUND_FREQUENCY;
    }
}

//--------------------------------------------------------------------+
// hang detector: a firmware timer event checks that the main loop moves
//--------------------------------------------------------------------+

extern volatile int g_where;
volatile uint64_t g_main_heartbeat;   // also watched from a helper core (mp_hang_watch)

static void hang_check(EFI_EVENT ev, void *ctx) {
    static uint64_t last;
    static int stuck;
    (void)ev;
    (void)ctx;
    if (g_main_heartbeat != last) {
        last = g_main_heartbeat;
        stuck = 0;
        return;
    }
    if (++stuck == 3 || stuck % 10 == 0) {
        uint64_t cs, ip;
        __asm__ volatile("vmread %1, %0" : "=r"(cs) : "r"((uint64_t)0x802));
        __asm__ volatile("vmread %1, %0" : "=r"(ip) : "r"((uint64_t)0x681E));
        printf("HANG: main loop stuck %d s in %d, guest %04llx:%08llx\n", stuck, g_where,
               (unsigned long long)cs, (unsigned long long)ip);
    }
}

static void hang_detector_start(void) {
    EFI_EVENT ev;
    // TPL_CALLBACK (8), periodic every second
    if (EFI_ERROR(gBS->CreateEvent(EVT_TIMER | 0x200, 8, (void *)hang_check, NULL, &ev))) return;
    gBS->SetTimer(ev, TimerPeriodic, 10000000);
}

//--------------------------------------------------------------------+
// main loop
//--------------------------------------------------------------------+

static void report(PC *pc, uint64_t elapsed_us) {
    static uint64_t last_exits[64], last_bursts, last_insns, last_irqs, last_tsc;
    char buf[400];
    int n = snprintf(buf, sizeof(buf), "10s: guest %llu%%, irqs %llu, bursts %llu (%llu insns), exits:",
                     (unsigned long long)((g_vmx_guest_tsc - last_tsc) / tsc_khz * 100 / (elapsed_us / 1000 + 1)),
                     (unsigned long long)(g_vmx_irqs - last_irqs), (unsigned long long)(g_vmx_bursts - last_bursts),
                     (unsigned long long)(g_vmx_burst_insns - last_insns));
    for (int i = 0; i < 64; i++) {
        uint64_t d = g_vmx_exits[i] - last_exits[i];
        if (d && n < (int)sizeof(buf) - 24) n += snprintf(buf + n, sizeof(buf) - n, " %d:%llu", i, (unsigned long long)d);
        last_exits[i] = g_vmx_exits[i];
    }
    extern uint32_t g_cache_hits, g_cache_misses, g_cache_flushes;
    printf("%s; netfs %u req %u retries; cache %u hits %u misses %u writes\n", buf, g_netfs_requests,
           g_netfs_retries, g_cache_hits, g_cache_misses, g_cache_flushes);
    {
        extern uint64_t g_vmx_emul_tsc, g_vmx_state_tsc, g_vmx_io_tsc;
        // SMIs: every core stops while the firmware runs in SMM (MSR_SMI_COUNT)
        static uint64_t last_smi;
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x34));
        uint64_t smi = (uint64_t)hi << 32 | lo;
        extern uint64_t g_plane_faults, g_plane_switches, g_last_ept_gpa;
        printf("10s: last EPT exit at %llx\n", (unsigned long long)g_last_ept_gpa);
        printf("10s: interpreter %llu ms, its state %llu ms, port I/O %llu ms, SMIs %llu, plane pages %llu in %llu "
               "switches\n",
               (unsigned long long)(g_vmx_emul_tsc / tsc_khz), (unsigned long long)(g_vmx_state_tsc / tsc_khz),
               (unsigned long long)(g_vmx_io_tsc / tsc_khz), (unsigned long long)(smi - last_smi),
               (unsigned long long)g_plane_faults, (unsigned long long)g_plane_switches);
        g_plane_faults = g_plane_switches = 0;
        extern uint32_t g_vga_keys[8], g_vga_key_n[8];
        for (int i = 0; i < 8; i++) {
            if (!g_vga_key_n[i]) continue;
            printf("vga: interpreted %u times with sr2 %02x gr5 %02x gr8 %02x gr3 %02x gr4 %x chain4 %u\n",
                   g_vga_key_n[i], g_vga_keys[i] >> 24, g_vga_keys[i] >> 16 & 0xff, g_vga_keys[i] >> 8 & 0xff,
                   g_vga_keys[i] >> 3 & 0x1f, g_vga_keys[i] >> 1 & 3, g_vga_keys[i] & 1);
            g_vga_key_n[i] = 0;
        }
        last_smi = smi;
        g_vmx_emul_tsc = g_vmx_state_tsc = g_vmx_io_tsc = 0;
    }
    printf("10s: loops %llu, pc_step %llu ms, redraw %llu ms in %llu calls (%llu kpx), hid %llu ms, ctl %llu ms, "
           "page flips %u (%u.%u fps)\n",
           (unsigned long long)n_loops, (unsigned long long)(t_step / tsc_khz), (unsigned long long)(t_redraw / tsc_khz),
           (unsigned long long)n_redraw, (unsigned long long)(px_redraw / 1000), (unsigned long long)(t_hid / tsc_khz),
           (unsigned long long)(t_ctl / tsc_khz), g_vga_flips, g_vga_flips / 10, g_vga_flips % 10);
    n_loops = t_step = t_redraw = n_redraw = px_redraw = t_hid = t_ctl = 0;
    g_vga_flips = 0;
    {
        extern uint32_t g_audio_peak, g_audio_frames, g_lpe_underruns, g_lpe_periods;
        printf("audio: %u frames, peak %u, %u periods, %u underruns\n", g_audio_frames, g_audio_peak,
               g_lpe_periods, g_lpe_underruns);
        g_audio_peak = 0;
        if (gmsynth_active()) {
            extern uint32_t g_gm_voices_max, g_gm_peak, g_gm_clipped;
            void gmsynth_positions(uint32_t out[4]);
            uint32_t p[4];
            gmsynth_positions(p);
            printf("gm: peak %u, voices up to %u, clipped %u; ring w %u r %u, messages w %u r %u\n", g_gm_peak,
                   g_gm_voices_max, g_gm_clipped, p[0], p[1], p[2], p[3]);
            g_gm_peak = g_gm_voices_max = 0;
        }
        extern uint32_t g_analog_fed, g_analog_drop, g_analog_starve, g_analog_underrun;
        uint32_t analog_level(void);
        extern uint32_t g_analog_gap_max;
        printf("analog: fed %u frames, dropped %u, starved %u, fifo underruns %u, level %u, longest pause %llu us\n",
               g_analog_fed, g_analog_drop, g_analog_starve, g_analog_underrun, analog_level(),
               (unsigned long long)g_analog_gap_max * 1000 / tsc_khz);
        g_analog_gap_max = 0;
        {
            // the busiest I/O ports of the last period
            extern uint32_t g_io_count[2][0x400];
            char line[160] = "";
            int len = 0;
            for (int k = 0; k < 8; k++) {
                int bd = -1, bp = 0;
                for (int d = 0; d < 2; d++)
                    for (int p = 0; p < 0x400; p++)
                        if (g_io_count[d][p] && (bd < 0 || g_io_count[d][p] > g_io_count[bd][bp])) bd = d, bp = p;
                if (bd < 0) break;
                len += snprintf(line + len, sizeof(line) - len, " %s%03x:%u", bd ? "w" : "r", bp, g_io_count[bd][bp]);
                g_io_count[bd][bp] = 0;
            }
            memset(g_io_count, 0, sizeof(g_io_count));
            printf("io:%s\n", line);
        }
        extern uint32_t g_kbd_queued, g_kbd_reads, g_kbd_irq_on, g_kbd_mode, g_kbd_pending;
        extern uint64_t g_vmx_redelivered;
        uint32_t i8259_debug_master(PicState2 *s);
        uint32_t pic = i8259_debug_master(pc->pic);
        extern uint32_t g_kbd_qcount, g_mouse_qcount;
        printf("kbd: waiting %u/%u, ", g_kbd_qcount, g_mouse_qcount);
        printf("kbd: queued %u, irq raised %u, port 60 reads %u, mode %02x pending %x; pic irr %02x imr %02x isr %02x; "
               "events delivered again %llu\n", g_kbd_queued, g_kbd_irq_on, g_kbd_reads, g_kbd_mode, g_kbd_pending,
               pic >> 8 & 0xff, pic >> 16 & 0xff, pic >> 24, (unsigned long long)g_vmx_redelivered);
    }
    if (surface && gop_fb) {
        // what the picture holds: a few surface pixels and their framebuffer copies
        uint32_t sy = 240, fy = off_y + 540;
        printf("pixels: surface %06x %06x %06x fb %06x %06x %06x\n", surface[sy * SURF_W + 100] & 0xffffff,
               surface[sy * SURF_W + 320] & 0xffffff, surface[10 * SURF_W + 10] & 0xffffff,
               gop_fb[fy * gop_stride + off_x + 225] & 0xffffff, gop_fb[fy * gop_stride + off_x + 720] & 0xffffff,
               gop_fb[(off_y + 22) * gop_stride + off_x + 22] & 0xffffff);
    }
    last_bursts = g_vmx_bursts;
    last_insns = g_vmx_burst_insns;
    last_irqs = g_vmx_irqs;
    last_tsc = g_vmx_guest_tsc;
    (void)pc;
}

//--------------------------------------------------------------------+
// the menus (Win+F11 settings, Win+F12 disks): the PC stops while one is open
//--------------------------------------------------------------------+

// The General MIDI SoundFont, 386/gm.sf2, as on X86Pi: read whole and handed
// to TinySoundFont, which keeps its own copy. Without it the MPU-401 keeps
// its built-in sine-wave synthesiser.
static void load_soundfont(void) {
    FIL f;
    if (f_open(&f, "386/gm.sf2", FA_READ) != FR_OK) {
        printf("midi: no 386/gm.sf2, the MPU-401 keeps its built-in synthesiser\n");
        return;
    }
    uint64_t t0 = wyse_ticks_us();
    uint64_t size = f_size(&f);
    uint8_t *buf = size ? malloc((size_t)size) : NULL;
    uint64_t done = 0;
    while (buf && done < size) {
        UINT want = size - done > (1u << 20) ? (1u << 20) : (UINT)(size - done), got = 0;
        if (f_read(&f, buf + done, want, &got) != FR_OK || !got) break;
        done += got;
    }
    f_close(&f);
    if (!buf || done != size) {
        printf("midi: 386/gm.sf2: read %llu of %llu bytes\n", (unsigned long long)done, (unsigned long long)size);
        free(buf);
        return;
    }
    uint64_t t1 = wyse_ticks_us();
    int ok = gmsynth_load(buf, (uint32_t)size);
    free(buf);
    printf("midi: 386/gm.sf2, %llu KB, read in %llu ms, %s in %llu ms\n", (unsigned long long)(size >> 10),
           (unsigned long long)((t1 - t0) / 1000), ok ? "loaded" : "NOT a SoundFont",
           (unsigned long long)((wyse_ticks_us() - t1) / 1000));
}

extern volatile int g_audio_pause;
static bool menu_on;
static PC *g_wyse_pc;

// Flush any pending guest text/graphics changes to the screen before a
// blocking physical USB floppy/CD ReadBlocks/WriteBlocks call enters SMM.
void wyse_vga_flush_before_io(void) {
    if (!g_wyse_pc || !g_wyse_pc->vga || !gop_fb || menu_on) return;
    static uint64_t last_flush_us;
    uint64_t now = wyse_ticks_us();
    if (now - last_flush_us < 8000) return;
    last_flush_us = now;
    if (g_wyse_vga_core) pc_vga_hold(1);
    vga_refresh(g_wyse_pc->vga, redraw, g_wyse_pc, 0);
    if (g_wyse_vga_core) pc_vga_hold(0);
}

static void menu_frame(void) {
    if (!menu_on) {
        menu_on = true;
        if (g_wyse_vga_core) pc_vga_hold(1);    // the surface is ours now
        g_audio_pause = 1;
    }
    bool relaid = layout_mode != g_picture;
    if (relaid) display_layout(g_picture);      // the display core is held: safe to move the picture
    if (osd_take_dirty() || relaid) {
        osd_render((uint8_t *)surface, SURF_W * 4, SURF_W, SURF_H);
        redraw(NULL, 0, 0, SURF_W, SURF_H);
    }
}

static void menu_leave(PC *pc) {
    menu_on = false;
    g_audio_pause = 0;
    // the menu's backdrop covers the whole surface, more than a text mode redraws
    memset(surface, 0, (size_t)SURF_W * SURF_H * 4);
    redraw(NULL, 0, 0, SURF_W, SURF_H);
    pc->full_update = 2;                        // paint the whole picture over the menu
    if (g_wyse_vga_core) pc_vga_hold(0);
}

void wyse_main(void) {
    static PCConfig conf;
    wyse_time_init();
    printf("wyseDOS: tsc %llu kHz\n", (unsigned long long)tsc_khz);

    storage_list();
    hostblk_list();
    if (!storage_init()) wyse_fatal("no 386/config.ini on the boot disk");
    if (installer_wanted()) installer_run();
    if (!load_config(&conf)) wyse_fatal("cannot read 386/config.ini");
    printf("config: bios %s, vga bios %s, hda %s, %ld MB, cpu %d, fpu %d\n", conf.bios, conf.vga_bios,
           conf.ata[0], conf.mem_size >> 20, conf.cpu_gen, conf.fpu);

    void firmware_events_init(uint32_t max_period_us);
    void firmware_events_poll(uint64_t now);
    firmware_events_init(firmware_timer_us);
    int hid = usbhid_init();
    display_init();
    splash_show();
    bool have_audio = lpe_init(SOUND_FREQUENCY);
    bool have_analog = analog_init();
    PC *pc = pc_new(redraw, NULL, NULL, (uint8_t *)surface, &conf);
    if (!pc) wyse_fatal("pc_new failed");
    g_wyse_pc = pc;
    if (pc->uhci) hostblk_usb_start(guest_usb_change);
    net_bind_host(pc->ne2000);
    // what the menus show and change: the machine as read, and the live settings applied
    config_note_machine((int)(conf.mem_size >> 20), conf.cpu_gen);
    config_note_vga_mem((int)(conf.vga_mem_size >> 10));
    config_note_fpu(conf.fpu);
    config_bind_pc(pc);
    config_apply_all();
    osd_init();
    settingsui_init();
    diskui_init();
    load_soundfont();
    {
        // the CMOS clock starts from the Wyse's own RTC
        EFI_TIME t;
        if (!EFI_ERROR(gST->RuntimeServices->GetTime(&t, NULL))) {
            CmosDateTime dt;
            rtc_to_local(&t, &dt);
            cmos_set_clock(pc->cmos, &dt);
            printf("rtc: %04u-%02u-%02u %02u:%02u:%02u UTC, local %04d-%02d-%02d %02d:%02d:%02d\n", t.Year, t.Month,
                   t.Day, t.Hour, t.Minute, t.Second, dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
        }
    }
    const char *err = wyse_cpu_init(pc);
    if (err && strstr(err, "VMX disabled") && !dell_token_active(DELL_TOKEN_VT_ENABLE) &&
        dell_token_activate(DELL_TOKEN_VT_ENABLE)) {
        // virtualization was off in BIOS setup: turned on, effective after a restart
        console_log = true;
        printf("wyseDOS: virtualization was switched off in BIOS setup; it is on now, restarting...\n");
        wyse_udelay(3000000);
        gST->RuntimeServices->ResetSystem(EfiResetCold, 0, 0, NULL);
    }
    if (err) wyse_fatal(err);
    {
        // the Atom's own VGA for the guest's A0000 window, if it answers
        bool hwvga_init(void);
        void wyse_cpu_vga_changed(void);
        if (hwvga_init()) wyse_cpu_vga_changed();
        void display_watch_init(void);
        display_watch_init();
    }
    console_log = false;    // the screen belongs to the guest from here on
    splash_end();
    display_layout(g_picture);      // clears the screen for the PC's picture
    load_bios_and_reset(pc);

    // helper cores: the display on core 2
    int cores = mp_init();
    if (cores >= 3 && mp_start(2, pc_vga_core_run)) g_wyse_vga_core = 1;
    // the sound on core 1
    if (cores >= 2 && (have_audio || have_analog)) audio_start(pc);
    // the headphone FIFO on core 3
    if (cores >= 4 && have_analog) analog_start();

    uint64_t last_report = wyse_ticks_us(), last_input = 0, last_ctl = 0, last_watchdog = 0, last_sync = 0;
    hang_detector_start();
    for (;;) {
        g_main_heartbeat++;
        if (settingsui_is_open() || diskui_is_open()) {
            menu_frame();
        } else {
            if (menu_on) menu_leave(pc);
            uint64_t ts = rdtsc_();
            pc_step(pc);
            t_step += rdtsc_() - ts;
        }
        n_loops++;
        uint64_t now = wyse_ticks_us();
        firmware_events_poll(now);
        if (now - last_input > 2000) {
            uint64_t th = rdtsc_();
            // keyboards, mice and USB storage plugged in or pulled out while running
            static uint64_t last_scan;
            if (now - last_scan > 1000000) {
                void hostblk_poll(void);
                hid = usbhid_scan();
                hostblk_poll();
                last_scan = now;
            }
            if (hid) {
                int hk = usbhid_poll(pc);
                if (hk == 0x44) {                           // Win+F11: settings
                    if (settingsui_is_open()) settingsui_close();
                    else if (!diskui_is_open()) settingsui_open();
                } else if (hk == 0x45) {                    // Win+F12: disks
                    if (diskui_is_open()) diskui_close();
                    else if (!settingsui_is_open()) diskui_open();
                } else if (hk == 0x4c) pc->reset_request = 1;      // Win+Del: reset the PC
                else if (hk == 0x15) wyse_reboot();      // Win+R: reboot the Wyse
            } else {
                poll_keyboard(pc);
            }
            t_hid += rdtsc_() - th;
            last_input = now;
        }
        if (now - last_ctl > 20000) {
            uint64_t tc = rdtsc_();
            poll_control(pc);
            t_ctl += rdtsc_() - tc;
            last_ctl = now;
        }
        if (now - last_sync > 100000) {
            // the guest's writes, half a second after its last one; the cache
            // keeps nothing dirty for longer than two seconds
            ide_sync_idle(0);
            // the headphone level from the mixer menu: 16 is the codec's
            // full +12 dB, each step below 3 dB less, 0 mutes
            // the processor's speed from the menu (or config.ini)
            static int speed_applied = -2, gen_applied;
            if (g_cpu_speed_mhz != speed_applied || g_cpu_speed_gen != gen_applied) {
                void wyse_cpu_set_speed(int mhz, int gen);
                speed_applied = g_cpu_speed_mhz;
                gen_applied = g_cpu_speed_gen;
                wyse_cpu_set_speed(speed_applied, gen_applied);
            }
            static int hp_applied = -1;
            if (g_mix_level[MIX_HEADPHONES] != hp_applied) {
                void analog_set_volume(int step);
                hp_applied = g_mix_level[MIX_HEADPHONES];
                analog_set_volume(hp_applied ? (16 - hp_applied) * 2 : -1);
            }
            static uint64_t last_flush;
            if (now - last_flush > 2000000) {
                if (blkcache_dirty()) blkcache_flush(NULL);
                last_flush = now;
            }
            last_sync = now;
        }
        {
            // Nothing here draws outside the picture: a black border that is not
            // black is somebody else's doing (logged), and so is an underrun
            static uint64_t last_border, logged_border;
            if (gop_fb && off_x >= 8 && now - last_border > 1000) {
                last_border = now;
                void display_watch_poll(void);
                extern uint32_t g_display_underruns;
                static uint32_t underruns_logged;
                display_watch_poll();
                if (g_display_underruns != underruns_logged && now - logged_border > 1000000) {
                    logged_border = now;
                    printf("display: FIFO underrun on pipe B (%u so far)\n", g_display_underruns);
                    underruns_logged = g_display_underruns;
                }
                uint32_t y = gop_h - 8, x = off_x / 2;
                uint32_t a = gop_fb[y * gop_stride + x] & 0xffffff, b = gop_fb[(gop_h - 90) * gop_stride + 4] & 0xffffff;
                if ((a | b) && now - logged_border > 1000000) {
                    logged_border = now;
                    printf("display: the black border is not black: %06x at %u,%u, %06x at 4,%u\n", a, x, y, b,
                           gop_h - 90);
                }
            }
        }
        if (now - last_watchdog > 5000000) {
            // a time set in DOS goes back into the RTC
            CmosDateTime dt;
            if (cmos_clock_changed(pc->cmos, &dt)) {
                EFI_TIME t;
                if (!EFI_ERROR(gST->RuntimeServices->GetTime(&t, NULL))) {
                    local_to_rtc(&dt, &t);
                    gST->RuntimeServices->SetTime(&t);
                    printf("rtc: set by the guest to %04u-%02u-%02u %02u:%02u:%02u UTC\n", t.Year, t.Month, t.Day,
                           t.Hour, t.Minute, t.Second);
                }
            }
            // a monitor that stops looping gets the machine reset (and netbooted) by firmware
            gBS->SetWatchdogTimer(30, 0x10000, 0, NULL);
            last_watchdog = now;
        }
        if (settingsui_restart_requested()) {
            // a new machine (memory, processor): saved already, so start over from the netboot
            settingsui_clear_restart();
            wyse_reboot();
        }
        if (diskui_reset_requested_flag) {
            diskui_reset_requested_flag = false;
            pc->reset_request = 1;
        }
        if (pc->reset_request) {
            pc->reset_request = 0;
            load_bios_and_reset(pc);
        }
        {
            void acpi_step(void);acpi_step();
            if (pc->shutdown_state==8) wyse_poweroff();
        }
        {
            // diagnostic: the BIOS tick count's jumps in the first seconds
            static uint32_t last_ticks, n_jumps;
            uint32_t ticks = *(volatile uint32_t *)(pc->phys_mem + 0x46c);
            if (n_jumps < 40 && (ticks - last_ticks > 30 && last_ticks - ticks > 30)) {
                CmosDateTime d;
                cmos_get_clock(pc->cmos, &d);
                printf("ticks: %u -> %u at %llu ms (cmos %02d:%02d:%02d)\n", last_ticks, ticks,
                       (unsigned long long)(now / 1000), d.hour, d.min, d.sec);
                n_jumps++;
            }
            last_ticks = ticks;
        }
        if (now - last_report > 10000000) {
            report(pc, now - last_report);
            last_report = now;
        }
    }
}
