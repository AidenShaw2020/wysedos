/*
 * The settings behind the on-screen menus.  See config_save.h.
 */

#include "config_save.h"
#include "pc.h"
#include "disk.h"
#include "ff.h"
#include "image_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The same file the kernel reads at boot, on the same mount. */
#define CONFIG_PATH "SD:/386/config.ini"

static PC *bound_pc;
static bool changed;
static bool restart_needed;

/* Defaults match what pc_new() sets up, so a machine with no [wyseDOS]
 * section in its ini behaves exactly as it did before there was a menu. */
static int cfg_volume       = 16;
static int cfg_mem_mb       = 8;
static int cfg_vga_kb       = 4096;
static int cfg_cpu_gen      = 4;
static int cfg_fpu          = 0;
static int cfg_pcspeaker    = 1;
static int cfg_adlib        = 1;
static int cfg_soundblaster = 1;
static int cfg_mpu401       = 1;
static int cfg_tandy        = 0;
static int cfg_covox        = 1;
static int cfg_dss          = 0;
static int cfg_mouse        = 1;
static int cfg_mouse_inv_y  = 0;
static int cfg_mouse_speed  = 100;   /* percent */
static int cfg_joystick     = 0;
static int cfg_power_restore = POWER_RESTORE_ON;
static int cfg_power_button  = 0;
static int cfg_picture       = PICTURE_4_3;
static int cfg_voodoo2       = 1;
static int cfg_voodoo2_mem   = 12;
static int cfg_wgpu_mb       = 64;
static int cfg_wgpu_enabled  = 1;
static int cfg_voodoo2_turbo = 1;
static int cfg_graphics_turbo = 1;

/* The mixer: a level for each sound source and for the headphone output,
 * 0..16 with 16 as unity (for the headphones, the codec's full +12 dB). */
static const char *const mix_keys[MIX_COUNT] = {
    "vol_pcspeaker", "vol_adlib", "vol_soundblaster", "vol_midi", "vol_tandy", "vol_covox", "vol_headphones"
};
static int cfg_mix[MIX_COUNT] = { 16, 16, 16, 16, 16, 16, 16 };

/* Read by the audio mixer and the mouse handler on the host side. */
int g_audio_volume = 16;
int g_mix_level[MIX_COUNT] = { 16, 16, 16, 16, 16, 16, 16 };
int g_mouse_invert_y = 0;
int g_picture = PICTURE_4_3;
int g_cpu_speed_mhz, g_cpu_speed_gen;   /* read by the host: 0 MHz = no limit */
int g_mouse_speed = 100;
int g_voodoo2_turbo = 1;
int g_graphics_turbo = 1;

void config_bind_pc(PC *pc) { bound_pc = pc; }

static void apply_live(void)
{
    g_audio_volume = cfg_volume;
    for (int i = 0; i < MIX_COUNT; i++) g_mix_level[i] = cfg_mix[i];
    g_mouse_invert_y = cfg_mouse_inv_y;
    g_picture = cfg_picture;
    g_cpu_speed_mhz = config_get_speed_mhz();
    g_cpu_speed_gen = cfg_cpu_gen;
    g_mouse_speed = cfg_mouse_speed;
    g_voodoo2_turbo = cfg_voodoo2_turbo;
    g_graphics_turbo = cfg_graphics_turbo;
    if (!bound_pc) return;
    bound_pc->pcspk_enabled    = cfg_pcspeaker;
    bound_pc->adlib_enabled    = cfg_adlib;
    bound_pc->sb16_enabled     = cfg_soundblaster;
    bound_pc->mpu401_enabled   = cfg_mpu401;
    bound_pc->tandy_enabled    = cfg_tandy;
    bound_pc->covox_enabled    = cfg_covox;
    bound_pc->dss_enabled      = cfg_dss;
    bound_pc->mouse_enabled    = cfg_mouse;
    bound_pc->joystick_enabled = cfg_joystick;
}

void config_apply_all(void) { apply_live(); }

bool config_has_changes(void)   { return changed; }
bool config_needs_restart(void) { return restart_needed; }
void config_clear_changes(void) { changed = false; restart_needed = false; }

#define LIVE_SETTING(name, var)                       \
    int config_get_##name(void) { return var; }       \
    void config_set_##name(int v)                     \
    {                                                 \
        if (var == v) return;                         \
        var = v; changed = true; apply_live();        \
    }

LIVE_SETTING(volume,       cfg_volume)
LIVE_SETTING(mix_pcspeaker,    cfg_mix[MIX_PCSPEAKER])
LIVE_SETTING(mix_adlib,        cfg_mix[MIX_ADLIB])
LIVE_SETTING(mix_soundblaster, cfg_mix[MIX_SOUNDBLASTER])
LIVE_SETTING(mix_midi,         cfg_mix[MIX_MIDI])
LIVE_SETTING(mix_tandy,        cfg_mix[MIX_TANDY])
LIVE_SETTING(mix_covox,        cfg_mix[MIX_COVOX])
LIVE_SETTING(mix_headphones,   cfg_mix[MIX_HEADPHONES])
LIVE_SETTING(pcspeaker,    cfg_pcspeaker)
LIVE_SETTING(adlib,        cfg_adlib)
LIVE_SETTING(soundblaster, cfg_soundblaster)
LIVE_SETTING(mpu401,       cfg_mpu401)
LIVE_SETTING(tandy,        cfg_tandy)
LIVE_SETTING(covox,        cfg_covox)
LIVE_SETTING(dss,          cfg_dss)
LIVE_SETTING(mouse,        cfg_mouse)
LIVE_SETTING(mouse_invert_y, cfg_mouse_inv_y)
LIVE_SETTING(mouse_speed,  cfg_mouse_speed)
LIVE_SETTING(joystick,     cfg_joystick)
LIVE_SETTING(power_restore, cfg_power_restore)
LIVE_SETTING(power_button, cfg_power_button)
LIVE_SETTING(picture,      cfg_picture)
LIVE_SETTING(voodoo2_turbo, cfg_voodoo2_turbo)
LIVE_SETTING(graphics_turbo, cfg_graphics_turbo)

/* The board is part of the machine: fitting it or changing its memory means
 * building the machine again. */
static const int voodoo2_mem_options[] = { 8, 12 };
static const int wgpu_mem_options[] = { 32, 64, 128 };

int config_get_wgpu(void) { return cfg_wgpu_enabled; }
void config_set_wgpu(int enabled)
{
    enabled = enabled != 0;
    if (cfg_wgpu_enabled == enabled) return;
    cfg_wgpu_enabled = enabled; changed = true; restart_needed = true;
}

const int *config_wgpu_mem_options(int *count)
{
    *count = (int)(sizeof wgpu_mem_options / sizeof *wgpu_mem_options);
    return wgpu_mem_options;
}

int config_get_wgpu_mem(void) { return cfg_wgpu_mb; }
void config_set_wgpu_mem(int mb)
{
    if (mb != 32 && mb != 64 && mb != 128) return;
    if (cfg_wgpu_mb == mb) return;
    cfg_wgpu_mb = mb; changed = true; restart_needed = true;
}

void config_note_wgpu(int mb, int enabled)
{
    int valid = mb == 32 || mb == 64 || mb == 128;
    cfg_wgpu_mb = valid ? mb : 64;
    cfg_wgpu_enabled = valid && enabled;
}

const int *config_voodoo2_mem_options(int *count)
{
    *count = (int)(sizeof voodoo2_mem_options / sizeof *voodoo2_mem_options);
    return voodoo2_mem_options;
}

int config_get_voodoo2(void) { return cfg_voodoo2; }
void config_set_voodoo2(int fitted)
{
    if (cfg_voodoo2 == fitted) return;
    cfg_voodoo2 = fitted; changed = true; restart_needed = true;
}

int config_get_voodoo2_mem(void) { return cfg_voodoo2_mem; }
void config_set_voodoo2_mem(int mb)
{
    if (cfg_voodoo2_mem == mb) return;
    cfg_voodoo2_mem = mb; changed = true; restart_needed = true;
}

void config_note_voodoo2(int fitted, int mem_mb)
{
    cfg_voodoo2 = fitted != 0;
    if (mem_mb == 8 || mem_mb == 12) cfg_voodoo2_mem = mem_mb;
}

/* These two describe the machine rather than its devices, so they cannot
 * take effect until it is built again. */
static const int mem_386[]  = { 1, 2, 4, 8, 16, 32, 64 };
static const int mem_486[]  = { 4, 8, 16, 32, 64, 128 };
static const int mem_586[]  = { 8, 16, 32, 64, 128, 256 };

const int *config_mem_options(int *count)
{
    switch (cfg_cpu_gen) {
    case 3:  *count = (int)(sizeof mem_386 / sizeof *mem_386); return mem_386;
    case 4:  *count = (int)(sizeof mem_486 / sizeof *mem_486); return mem_486;
    default: *count = (int)(sizeof mem_586 / sizeof *mem_586); return mem_586;
    }
}

/* Bring the size into what the new generation's boards could hold, keeping as
 * much of it as that allows. */
static void clamp_mem_to_generation(void)
{
    int n = 0;
    const int *opt = config_mem_options(&n);
    if (cfg_mem_mb < opt[0]) { cfg_mem_mb = opt[0]; return; }
    if (cfg_mem_mb > opt[n - 1]) { cfg_mem_mb = opt[n - 1]; return; }
    for (int i = 0; i < n; i++) if (opt[i] == cfg_mem_mb) return;
    for (int i = n - 1; i >= 0; i--) if (opt[i] < cfg_mem_mb) { cfg_mem_mb = opt[i]; return; }
}

int config_get_mem_size_mb(void) { return cfg_mem_mb; }
void config_set_mem_size_mb(int mb)
{
    if (cfg_mem_mb == mb) return;
    cfg_mem_mb = mb; changed = true; restart_needed = true;
}

/*
 * The video card's memory, in kilobytes.
 *
 * Powers of two only: the VESA bank register is masked with the size in
 * 64 KB banks less one, so anything else would send a bank's worth of writes
 * somewhere the card did not intend.  256 KB is what a real VGA had and is
 * enough for the standard modes; the larger sizes are what the VESA modes and
 * a second display page need, and Red Alert wanted more than a megabyte
 * before it would show anything at all.
 */
static const int vga_kb_options[] = { 256, 512, 1024, 2048, 4096, 8192 };

const int *config_vga_mem_options(int *count)
{
    *count = (int)(sizeof vga_kb_options / sizeof *vga_kb_options);
    return vga_kb_options;
}

int config_get_vga_mem_kb(void) { return cfg_vga_kb; }
void config_set_vga_mem_kb(int kb)
{
    if (cfg_vga_kb == kb) return;
    cfg_vga_kb = kb; changed = true; restart_needed = true;
}

/*
 * The processor's speed, in MHz of the chosen generation; 0 is as fast as
 * the host runs it. The Wyse runs guest code natively and far faster than
 * any of these parts, which some programs cannot take - Turbo Pascal's CRT
 * unit stops with "Runtime error 200" above about a Pentium 200 (Draci
 * historie) - and games written against a 386 or 486 run too fast. The host
 * holds the guest to the speed by letting it run only that share of the time
 * (see wyse_cpu_set_speed()). Not set in the ini: the generation's default.
 */
static int cfg_speed = -1;
static const int speed_386[] = { 16, 20, 25, 33, 40, 0 };
static const int speed_486[] = { 25, 33, 50, 66, 75, 100, 120, 0 };
static const int speed_586[] = { 60, 75, 90, 100, 120, 133, 166, 200, 233, 0 };

const int *config_speed_options(int *count)
{
    switch (cfg_cpu_gen) {
    case 3:  *count = (int)(sizeof speed_386 / sizeof *speed_386); return speed_386;
    case 4:  *count = (int)(sizeof speed_486 / sizeof *speed_486); return speed_486;
    default: *count = (int)(sizeof speed_586 / sizeof *speed_586); return speed_586;
    }
}

static int default_speed(void) { return cfg_cpu_gen == 3 ? 40 : cfg_cpu_gen == 4 ? 66 : 133; }

int config_get_speed_mhz(void) { return cfg_speed < 0 ? default_speed() : cfg_speed; }
void config_set_speed_mhz(int mhz)
{
    if (cfg_speed == mhz) return;
    cfg_speed = mhz; changed = true; apply_live();
}

int config_get_fpu(void) { return cfg_fpu; }
void config_set_fpu(int enabled)
{
    if (cfg_fpu == enabled) return;
    cfg_fpu = enabled; changed = true; restart_needed = true;
}

int config_get_cpu_gen(void) { return cfg_cpu_gen; }
void config_set_cpu_gen(int gen)
{
    if (cfg_cpu_gen == gen) return;
    cfg_cpu_gen = gen; changed = true; restart_needed = true;
    /*
     * Every Pentium has a floating-point unit.  There is no part to
     * correspond to one without, and CPUID would then report a family of 5
     * with its FPU bit clear - which software is entitled to treat as
     * impossible, and would answer by using x87 instructions that this
     * machine would quietly ignore.  A 486 is different: the SX really did
     * ship without one, so that choice stays open.
     */
    if (gen >= 5) cfg_fpu = 1;
    clamp_mem_to_generation();
    /* a speed the new generation never ran at goes back to its default */
    int n = 0;
    const int *opt = config_speed_options(&n);
    bool known = false;
    for (int i = 0; i < n; i++) known |= opt[i] == cfg_speed;
    if (!known) cfg_speed = -1;
}

void config_set_from_ini(const char *name, const char *value)
{
    const int v = atoi(value);
    if      (!strcmp(name, "volume"))       cfg_volume = v < 0 ? 0 : v > 16 ? 16 : v;
    else if (!strcmp(name, "pcspeaker"))    cfg_pcspeaker = v != 0;
    else if (!strcmp(name, "adlib"))        cfg_adlib = v != 0;
    else if (!strcmp(name, "soundblaster")) cfg_soundblaster = v != 0;
    else if (!strcmp(name, "mpu401"))       cfg_mpu401 = v != 0;
    else if (!strcmp(name, "tandy"))        cfg_tandy = v != 0;
    else if (!strcmp(name, "covox"))        cfg_covox = v != 0;
    else if (!strcmp(name, "dss"))          cfg_dss = v != 0;
    else if (!strcmp(name, "mouse"))        cfg_mouse = v != 0;
    else if (!strcmp(name, "mouse_invert_y")) cfg_mouse_inv_y = v != 0;
    else if (!strcmp(name, "mouse_speed"))  cfg_mouse_speed = v < 25 ? 25 : v > 400 ? 400 : v;
    else if (!strcmp(name, "joystick"))     cfg_joystick = v != 0;
    else if (!strcmp(name, "power_restore")) cfg_power_restore = v < 0 || v > 2 ? POWER_RESTORE_ON : v;
    else if (!strcmp(name, "power_button")) cfg_power_button = v != 0;
    else if (!strcmp(name, "speed"))        cfg_speed = v < 0 ? -1 : v;
    else if (!strcmp(name, "picture"))      cfg_picture = v < 0 || v > 2 ? PICTURE_4_3 : v;
    else if (!strcmp(name, "voodoo2_turbo")) cfg_voodoo2_turbo = v != 0;
    else if (!strcmp(name, "graphics_turbo")) cfg_graphics_turbo = v != 0;
    else
        for (int i = 0; i < MIX_COUNT; i++)
            if (!strcmp(name, mix_keys[i])) cfg_mix[i] = v < 0 ? 0 : v > 16 ? 16 : v;
}

/* The memory size and processor come from the [pc] section the kernel already
 * parses, so the menu starts from what the machine was actually built with
 * rather than from the defaults above. */
void config_note_machine(int mem_mb, int cpu_gen)
{
    if (mem_mb > 0) cfg_mem_mb = mem_mb;
    if (cpu_gen > 0) cfg_cpu_gen = cpu_gen;
}

void config_note_vga_mem(int kb)
{
    int n = 0;
    const int *opt = config_vga_mem_options(&n);
    /* Only a size the menu can also express, so that opening the menu and
     * leaving it without touching anything cannot quietly change the
     * machine. */
    for (int i = 0; i < n; i++)
        if (opt[i] == kb) { cfg_vga_kb = kb; return; }
}

void config_note_fpu(int fpu) { cfg_fpu = fpu != 0; }

/* ------------------------------------------------------------------------ */
/* Writing it back                                                           */
/* ------------------------------------------------------------------------ */

static bool put(FIL *fp, const char *line)
{
    UINT bw = 0;
    const UINT n = (UINT)strlen(line);
    return f_write(fp, line, n, &bw) == FR_OK && bw == n;
}

static bool putf(FIL *fp, const char *key, int value)
{
    char line[64];
    snprintf(line, sizeof line, "%s=%d\n", key, value);
    return put(fp, line);
}

/*
 * The file is rewritten whole rather than edited, because there is no reader
 * here that could preserve what it does not understand.  Everything the
 * kernel parses is written back, so a save never loses the machine's
 * definition - that was the failure mode worth designing against.
 */
__attribute__((weak)) const char *config_platform_lines(void) { return ""; }

static bool write_config(void)
{
    FIL fp;
    char line[128];

    if (f_open(&fp, CONFIG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;

    bool ok = true;
    ok &= put(&fp, "; Written by the on-screen settings menu.\n\n[pc]\n");
    snprintf(line, sizeof line, "mem=%dM\n", cfg_mem_mb);
    ok &= put(&fp, line);
    snprintf(line, sizeof line, "vga_mem=%dK\n", cfg_vga_kb);
    ok &= put(&fp, line);
    ok &= putf(&fp, "cpu", cfg_cpu_gen);
    ok &= putf(&fp, "fpu", cfg_fpu);
    ok &= putf(&fp, "voodoo2", cfg_voodoo2);
    ok &= putf(&fp, "voodoo2_mem", cfg_voodoo2_mem);
    ok &= putf(&fp, "wgpu", cfg_wgpu_mb);
    ok &= putf(&fp, "wgpu_enabled", cfg_wgpu_enabled);
    ok &= put(&fp, "bios=bios.bin\n");
    ok &= put(&fp, "vga_bios=vgabios.bin\n");

    ok &= put(&fp, "\n; Disk images\n");
    for (int i = 0; i < 2; i++) {
        const char *f = fdd_get_filename(i);
        if (f && f[0] && image_media_path_source(f)<0) {
            snprintf(line, sizeof line, "fd%c=%s\n", 'a' + i, f);
            ok &= put(&fp, line);
        }
    }
    for (int i = 0; i < 4; i++) {
        const char *f = ata_get_filename(i);
        if (f && f[0] && image_media_path_source(f)<0) {
            snprintf(line, sizeof line, "%s%c=%s\n",
                     ata_is_cdrom(i) ? "cd" : "hd", 'a' + i, f);
            ok &= put(&fp, line);
        }
    }

    /* Keys only the platform knows (wyseDOS: timezone, devctl), so a save
     * from the menu keeps them. */
    ok &= put(&fp, config_platform_lines());
    ok &= put(&fp, "\n[wyseDOS]\n");
    ok &= putf(&fp, "volume", cfg_volume);
    for (int i = 0; i < MIX_COUNT; i++) ok &= putf(&fp, mix_keys[i], cfg_mix[i]);
    ok &= putf(&fp, "pcspeaker", cfg_pcspeaker);
    ok &= putf(&fp, "adlib", cfg_adlib);
    ok &= putf(&fp, "soundblaster", cfg_soundblaster);
    ok &= putf(&fp, "mpu401", cfg_mpu401);
    ok &= putf(&fp, "tandy", cfg_tandy);
    ok &= putf(&fp, "covox", cfg_covox);
    ok &= putf(&fp, "dss", cfg_dss);
    ok &= putf(&fp, "mouse", cfg_mouse);
    ok &= putf(&fp, "mouse_invert_y", cfg_mouse_inv_y);
    ok &= putf(&fp, "mouse_speed", cfg_mouse_speed);
    ok &= putf(&fp, "joystick", cfg_joystick);
    ok &= putf(&fp, "power_restore", cfg_power_restore);
    ok &= putf(&fp, "power_button", cfg_power_button);
    ok &= putf(&fp, "picture", cfg_picture);
    ok &= putf(&fp, "voodoo2_turbo", cfg_voodoo2_turbo);
    ok &= putf(&fp, "graphics_turbo", cfg_graphics_turbo);
    if (cfg_speed >= 0) ok &= putf(&fp, "speed", cfg_speed);

    return f_close(&fp) == FR_OK && ok;
}

bool config_save_all(void)
{
    const bool ok = write_config();
    if (ok) changed = false;
    return ok;
}

bool config_save_disks(void) { return write_config(); }
