/*
 * The settings menu, on Win+F11.
 *
 * Every row is one entry in the table below, including the tabs and the
 * headings, so the layout is data rather than a sequence of draw calls and
 * the cursor only has to know how to skip what it cannot select.  A tab row
 * starts a page (CPU, GPU, SOUND, ...); Tab / PgDn and PgUp step through them.
 */

#include "settingsui.h"
#include "uikeys.h"
#include "config_save.h"
#include "osd.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    STATE_CLOSED,
    STATE_MAIN,
    STATE_SAVED,           /* a short confirmation over the menu */
    STATE_CONFIRM_RESTART, /* saved, and something needs the machine rebuilt */
    STATE_CONFIRM_BIGMEM   /* more memory than DOS will map */
} State;

/*
 * When a change takes hold, which is not the same question for every row.
 *
 * EFFECT_RESTART is a real requirement rather than caution: CR0's ET bit is
 * set from whether a coprocessor exists, and only at reset, and the BIOS
 * reads the memory size and the equipment byte out of the CMOS when it posts.
 * The processor generation is softer - CPUID is the only thing that reads it
 * - but software asks once, at startup, so changing it under a running DOS
 * would only make the machine disagree with what is already installed.
 *
 * EFFECT_DEVICE is immediate and complete in one direction only.  The enable
 * flags gate both the port responses and the mixer, so switching a device off
 * silences it at once; switching one on cannot make a program that has
 * already chosen its sound card go and look again.
 */
typedef enum {
    EFFECT_NOW,       /* host side: nothing in the guest has to agree */
    EFFECT_DEVICE,    /* the hardware changes under whatever is running */
    EFFECT_RESTART
} RowEffect;

typedef enum {
    ROW_TAB,              /* starts a page; its label is the tab's name */
    ROW_HEADING,
    ROW_TOGGLE,
    ROW_CHOICE,           /* one of a list of numbers */
    ROW_VOLUME
} RowKind;

typedef struct {
    RowKind kind;
    /* Null when the label depends on the rest of the configuration; see
     * row_label(). */
    const char *label;
    int (*get)(void);
    void (*set)(int);
    const int *options;
    int option_count;
    RowEffect effect;
} Row;

static const int cpu_options[] = { 3, 4, 5 };
static const int mouse_speed_options[] = { 25, 50, 75, 100, 125, 150, 200, 300, 400 };
static const int power_restore_options[] = { POWER_RESTORE_OFF, POWER_RESTORE_ON, POWER_RESTORE_LAST };
static const int power_button_options[] = { 0, 1 };
static const int picture_options[] = { PICTURE_4_3, PICTURE_FILL, PICTURE_SHARP_2X };

static const Row rows[] = {
    { ROW_TAB,     "CPU",                  0, 0, 0, 0, EFFECT_NOW },
    /* Null options: the sizes depend on the processor, because it is the
     * board rather than the chip that limits them.  Fetched when the row is
     * used; see row_options(). */
    { ROW_CHOICE,  "Memory",               config_get_mem_size_mb, config_set_mem_size_mb,
                                           0, 0, EFFECT_RESTART },
    { ROW_CHOICE,  "Processor",            config_get_cpu_gen, config_set_cpu_gen,
                                           cpu_options, 3, EFFECT_RESTART },
    { ROW_TOGGLE,  0,                      config_get_fpu, config_set_fpu, 0, 0, EFFECT_RESTART },
    /* Null options: the clocks depend on the processor; see row_options(). */
    { ROW_CHOICE,  "Speed",                config_get_speed_mhz, config_set_speed_mhz, 0, 0, EFFECT_NOW },

    { ROW_TAB,     "GPU",                  0, 0, 0, 0, EFFECT_NOW },
    { ROW_HEADING, "VGA",                  0, 0, 0, 0, EFFECT_NOW },
    { ROW_CHOICE,  "Video memory",         config_get_vga_mem_kb, config_set_vga_mem_kb,
                                           0, 0, EFFECT_RESTART },
    { ROW_CHOICE,  "Picture",              config_get_picture, config_set_picture,
                                           picture_options, 3, EFFECT_NOW },
    { ROW_HEADING, "Voodoo2 3D card",      0, 0, 0, 0, EFFECT_NOW },
    { ROW_TOGGLE,  "Voodoo2",              config_get_voodoo2, config_set_voodoo2, 0, 0, EFFECT_RESTART },
    /* Null options: see row_options(). */
    { ROW_CHOICE,  "Voodoo2 memory",       config_get_voodoo2_mem, config_set_voodoo2_mem,
                                           0, 0, EFFECT_RESTART },
    { ROW_TOGGLE,  "CPU Boost DOS: Glide", config_get_voodoo2_turbo, config_set_voodoo2_turbo,
                                           0, 0, EFFECT_NOW },
    { ROW_HEADING, "Windows 3D",            0, 0, 0, 0, EFFECT_NOW },
    { ROW_TOGGLE,  "Windows 3D accelerator", config_get_wgpu, config_set_wgpu,
                                           0, 0, EFFECT_RESTART },
    { ROW_CHOICE,  "Windows 3D memory", config_get_wgpu_mem, config_set_wgpu_mem,
                                           0, 0, EFFECT_RESTART },
    { ROW_TOGGLE,  "CPU Boost Windows: DirectX/Glide/OpenGL", config_get_graphics_turbo, config_set_graphics_turbo,
                                           0, 0, EFFECT_NOW },

    { ROW_TAB,     "SOUND",                0, 0, 0, 0, EFFECT_NOW },
    { ROW_VOLUME,  "Volume",               config_get_volume, config_set_volume, 0, 0, EFFECT_NOW },
    { ROW_VOLUME,  "Headphones",           config_get_mix_headphones, config_set_mix_headphones, 0, 0, EFFECT_NOW },
    { ROW_HEADING, "Devices",              0, 0, 0, 0, EFFECT_NOW },
    { ROW_TOGGLE,  "PC speaker",           config_get_pcspeaker, config_set_pcspeaker, 0, 0, EFFECT_DEVICE },
    { ROW_TOGGLE,  "AdLib",                config_get_adlib, config_set_adlib, 0, 0, EFFECT_DEVICE },
    { ROW_TOGGLE,  "Sound Blaster",        config_get_soundblaster, config_set_soundblaster, 0, 0, EFFECT_DEVICE },
    { ROW_TOGGLE,  "Roland MPU-401",       config_get_mpu401, config_set_mpu401, 0, 0, EFFECT_DEVICE },
    { ROW_TOGGLE,  "Tandy 3-voice",        config_get_tandy, config_set_tandy, 0, 0, EFFECT_DEVICE },
    { ROW_TOGGLE,  "Covox on LPT2",        config_get_covox, config_set_covox, 0, 0, EFFECT_DEVICE },
    { ROW_TOGGLE,  "Disney Sound Source",  config_get_dss, config_set_dss, 0, 0, EFFECT_DEVICE },
    { ROW_HEADING, "Mixer",                0, 0, 0, 0, EFFECT_NOW },
    { ROW_VOLUME,  "PC speaker",           config_get_mix_pcspeaker, config_set_mix_pcspeaker, 0, 0, EFFECT_NOW },
    { ROW_VOLUME,  "AdLib / OPL3",         config_get_mix_adlib, config_set_mix_adlib, 0, 0, EFFECT_NOW },
    { ROW_VOLUME,  "Sound Blaster",        config_get_mix_soundblaster, config_set_mix_soundblaster, 0, 0, EFFECT_NOW },
    { ROW_VOLUME,  "MIDI",                 config_get_mix_midi, config_set_mix_midi, 0, 0, EFFECT_NOW },
    { ROW_VOLUME,  "Tandy",                config_get_mix_tandy, config_set_mix_tandy, 0, 0, EFFECT_NOW },
    { ROW_VOLUME,  "Covox / Sound Source", config_get_mix_covox, config_set_mix_covox, 0, 0, EFFECT_NOW },

    { ROW_TAB,     "INPUT",                0, 0, 0, 0, EFFECT_NOW },
    { ROW_TOGGLE,  "Mouse",                config_get_mouse, config_set_mouse, 0, 0, EFFECT_DEVICE },
    { ROW_CHOICE,  "Mouse speed",          config_get_mouse_speed, config_set_mouse_speed,
                                           mouse_speed_options, 9, EFFECT_NOW },
    { ROW_TOGGLE,  "Invert mouse Y",       config_get_mouse_invert_y, config_set_mouse_invert_y, 0, 0, EFFECT_NOW },
    { ROW_TOGGLE,  "Joystick",             config_get_joystick, config_set_joystick, 0, 0, EFFECT_DEVICE },

    { ROW_TAB,     "POWER",                0, 0, 0, 0, EFFECT_NOW },
    { ROW_CHOICE,  "When power returns",   config_get_power_restore, config_set_power_restore,
                                           power_restore_options, 3, EFFECT_NOW },
    { ROW_CHOICE,  "Power button",         config_get_power_button, config_set_power_button,
                                           power_button_options, 2, EFFECT_NOW },
};
#define ROW_COUNT ((int)(sizeof rows / sizeof rows[0]))

#define PANEL_X  13
#define PANEL_Y  1
#define PANEL_W  54
#define PANEL_H  24
/* The tab bar under the title, then the page's rows down to the two note
 * lines above the footer; a page scrolls when it has more. */
#define TABS_Y     (PANEL_Y + 2)
#define LIST_TOP   (PANEL_Y + 4)
#define LIST_ROWS  (PANEL_Y + PANEL_H - 4 - LIST_TOP)

static State state;
static int cursor = 1;                 /* row 0 is the first tab */
static int scroll = 1;                 /* the first row on screen */

/* The page a row is on: its tab row, and the row after its last. */
static int page_start(int row)
{
    while (row > 0 && rows[row].kind != ROW_TAB) row--;
    return row;
}

static int page_end(int start)
{
    int row = start + 1;
    while (row < ROW_COUNT && rows[row].kind != ROW_TAB) row++;
    return row;
}

static bool selectable(int row)
{
    return rows[row].kind != ROW_TAB && rows[row].kind != ROW_HEADING;
}
static bool restart_requested;
static int mem_before_confirm;         /* to put back if the answer is no */

bool settingsui_restart_requested(void) { return restart_requested; }
void settingsui_clear_restart(void) { restart_requested = false; }

static void draw(void);

void settingsui_init(void) { state = STATE_CLOSED; cursor = 1; scroll = 1; }

bool settingsui_is_open(void) { return state != STATE_CLOSED; }

void settingsui_open(void)
{
    if (state != STATE_CLOSED) return;
    state = STATE_MAIN;
    osd_show();
    draw();
}

void settingsui_close(void)
{
    state = STATE_CLOSED;
    osd_hide();
}

/* ------------------------------------------------------------------------ */

/*
 * What the floating-point row is called depends on the processor, because the
 * part it describes does: a separate chip beside a 386, the difference
 * between a 486SX and a 486DX, and not a choice at all on a Pentium.
 */
static const char *row_label(const Row *r)
{
    if (r->label) return r->label;
    switch (config_get_cpu_gen()) {
    case 3:  return "387 coprocessor";
    case 4:  return "Integrated FPU (486DX)";
    default: return "Integrated FPU";
    }
}

static const int *row_options(const Row *r, int *count)
{
    if (r->options) { *count = r->option_count; return r->options; }
    if (r->set == config_set_vga_mem_kb) return config_vga_mem_options(count);
    if (r->set == config_set_speed_mhz) return config_speed_options(count);
    if (r->set == config_set_voodoo2_mem) return config_voodoo2_mem_options(count);
    if (r->set == config_set_wgpu_mem) return config_wgpu_mem_options(count);
    return config_mem_options(count);
}

static int fpu_is_fixed(const Row *r)
{
    return r->get == config_get_fpu && config_get_cpu_gen() >= 5;
}

static const char *cpu_name(int gen)
{
    switch (gen) {
    case 3:  return "386";
    case 4:  return "486";
    case 5:  return "Pentium";
    default: return "?";
    }
}

static void format_value(const Row *r, char *out, size_t cap)
{
    const int v = r->get ? r->get() : 0;
    switch (r->kind) {
    case ROW_TOGGLE:
        if (fpu_is_fixed(r)) snprintf(out, cap, "always");
        else                 snprintf(out, cap, "%s", v ? "on" : "off");
        break;
    case ROW_VOLUME: {
        /* A bar reads faster than a number for something you adjust by ear. */
        char bar[20];
        const int filled = (v * 12 + 8) / 16;
        for (int i = 0; i < 12; i++) bar[i] = i < filled ? '\xDB' : '\xB0';
        bar[12] = 0;
        snprintf(out, cap, "%s %2d", bar, v);
        break;
    }
    case ROW_CHOICE:
        if (r->set == config_set_cpu_gen)
            snprintf(out, cap, "%s", cpu_name(v));
        else if (r->set == config_set_vga_mem_kb) {
            /* Kilobytes below a megabyte, because 256 KB is the size this
             * card is remembered by and "0 MB" would be nonsense. */
            if (v < 1024) snprintf(out, cap, "%d KB", v);
            else          snprintf(out, cap, "%d MB", v / 1024);
        }
        else if (r->set == config_set_mouse_speed)
            snprintf(out, cap, "%d %%", v);
        else if (r->set == config_set_power_restore)
            snprintf(out, cap, "%s", v == POWER_RESTORE_OFF ? "stay off" :
                                     v == POWER_RESTORE_ON  ? "power on" : "last state");
        else if (r->set == config_set_speed_mhz) {
            if (v) snprintf(out, cap, "%d MHz", v);
            else   snprintf(out, cap, "full speed");
        }
        else if (r->set == config_set_picture)
            snprintf(out, cap, "%s", v == PICTURE_FILL ? "fill the screen" :
                                     v == PICTURE_SHARP_2X ? "sharp, 2x pixels" : "4:3");
        else if (r->set == config_set_voodoo2_mem)
            snprintf(out, cap, "%d MB", v);
        else if (r->set == config_set_wgpu_mem) {
            snprintf(out, cap, "%d MB", v);
        }
        else if (r->set == config_set_power_button)
            snprintf(out, cap, "%s", v ? "hold 4 s to switch off" : "press to switch off");
        else
            snprintf(out, cap, "%d MB", v);
        break;
    default:
        out[0] = 0;
        break;
    }
}

static void draw(void)
{
    osd_clear();
    osd_draw_box_titled(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, "Settings",
                        OSD_ATTR(OSD_WHITE, OSD_LIGHTBLUE));

    const int x = PANEL_X + 2;

    /* The tab bar, centred: every page's name, the current one highlighted. */
    const int first = page_start(cursor) + 1, end = page_end(first - 1);
    {
        int width = -1;
        for (int i = 0; i < ROW_COUNT; i++)
            if (rows[i].kind == ROW_TAB) width += (int)strlen(rows[i].label) + 3;
        int tx = PANEL_X + (PANEL_W - width) / 2;
        for (int i = 0; i < ROW_COUNT; i++) {
            if (rows[i].kind != ROW_TAB) continue;
            char name[16];
            snprintf(name, sizeof name, " %s ", rows[i].label);
            osd_print(tx, TABS_Y, name, i == first - 1 ? OSD_ATTR_SELECTED : OSD_ATTR_NORMAL);
            tx += (int)strlen(name) + 1;
        }
    }

    /* Keep the cursor on screen, and the heading above it with it. */
    const int want = rows[cursor - 1].kind == ROW_HEADING ? cursor - 1 : cursor;
    if (scroll < first || scroll >= end) scroll = first;
    if (want < scroll) scroll = want;
    if (cursor >= scroll + LIST_ROWS) scroll = cursor - LIST_ROWS + 1;
    if (scroll > first)
        osd_putchar(PANEL_X + PANEL_W - 2, LIST_TOP, '\x1E', OSD_ATTR_HINT);
    if (scroll + LIST_ROWS < end)
        osd_putchar(PANEL_X + PANEL_W - 2, LIST_TOP + LIST_ROWS - 1, '\x1F', OSD_ATTR_HINT);

    for (int i = scroll; i < end && i < scroll + LIST_ROWS; i++) {
        const Row *r = &rows[i];
        const int y = LIST_TOP + i - scroll;

        if (r->kind == ROW_HEADING) {
            osd_print(x, y, row_label(r), OSD_ATTR(OSD_YELLOW, OSD_BLUE));
            continue;
        }

        const bool selected = (i == cursor);
        const uint8_t label_attr = selected ? OSD_ATTR_SELECTED : OSD_ATTR_NORMAL;
        const uint8_t value_attr = selected ? OSD_ATTR_SELECTED : OSD_ATTR_VALUE;

        if (selected) {
            /* The bar runs the width of the list so the rounded ends line up
             * with the panel rather than with the longest label. */
            for (int c = x - 1; c < PANEL_X + PANEL_W - 2; c++)
                osd_putchar(c, y, ' ', OSD_ATTR_SELECTED);
        }

        osd_print(x + 1, y, row_label(r), label_attr);

        char value[40];
        format_value(r, value, sizeof value);
        int vx = PANEL_X + PANEL_W - 4 - (int)strlen(value);
        osd_print(vx, y, value, value_attr);

        if (selected) {
            osd_putchar(vx - 2, y, '\x11', value_attr);   /* left arrow  */
            osd_putchar(PANEL_X + PANEL_W - 3, y, '\x10', value_attr);
        }
        if (r->effect == EFFECT_RESTART)
            osd_putchar(x, y, '\x07', selected ? OSD_ATTR_SELECTED
                                               : OSD_ATTR(OSD_BROWN, OSD_BLUE));
    }

    const int footer = PANEL_Y + PANEL_H - 2;
    osd_print(x, footer,
              "Tab page  \x18\x19 choose  \x1B\x1A change  Enter save  Esc",
              OSD_ATTR_HINT);

    /*
     * Only the rows that need the machine restarted say anything.  The
     * others were labelled too - "applies now", and a note that a running
     * program will not re-detect a device - and at a glance that reads as
     * part of the list rather than as a remark about the row under the
     * cursor.  A line that is always there stops being read.
     */
    if (rows[cursor].effect == EFFECT_RESTART)
        osd_print(x, footer - 2, "\x07 takes effect when the machine restarts",
                  OSD_ATTR(OSD_BROWN, OSD_BLUE));
    else if (rows[cursor].set == config_set_voodoo2_turbo ||
             rows[cursor].set == config_set_graphics_turbo)
        osd_print(x, footer - 2, "Full CPU speed during 3D rendering", OSD_ATTR_HINT);

    if (config_has_changes())
        osd_print(x, footer - 1, "changed, not yet saved", OSD_ATTR_HINT);

    if (state == STATE_SAVED) {
        const int w = 34, h = 3;
        const int dx = (OSD_COLS - w) / 2, dy = 10;
        osd_draw_box(dx, dy, w, h, OSD_ATTR(OSD_WHITE, OSD_GREEN));
        osd_print_center(dy + 1, "Saved to 386/config.ini",
                         OSD_ATTR(OSD_WHITE, OSD_BLUE));
    } else if (state == STATE_CONFIRM_RESTART) {
        const int w = 50, h = 4;
        const int dx = (OSD_COLS - w) / 2, dy = 9;
        osd_draw_box(dx, dy, w, h, OSD_ATTR(OSD_WHITE, OSD_BROWN));
        osd_print_center(dy + 1, "Saved. Restart the machine now?",
                         OSD_ATTR(OSD_WHITE, OSD_BLUE));
        osd_print_center(dy + 2, "Y restart    N keep running",
                         OSD_ATTR_HINT);
    } else if (state == STATE_CONFIRM_BIGMEM) {
        /*
         * Measured, not guessed: MEM reports the same 66,057,376 bytes of
         * extended memory whether the machine was built with 64 MB or with
         * 128.  The limit is HIMEM.SYS's own, and a Pentium board of the
         * period with 128 MB fitted behaved in exactly this way under DOS.
         * Worth asking about rather than leaving to look like a fault.
         */
        const int w = 52, h = 5;
        const int dx = (OSD_COLS - w) / 2, dy = 9;
        osd_draw_box(dx, dy, w, h, OSD_ATTR(OSD_WHITE, OSD_RED));
        osd_print_center(dy + 1, "DOS reaches only the first 64 MB",
                         OSD_ATTR(OSD_WHITE, OSD_BLUE));
        osd_print_center(dy + 2, "The rest is fitted and unreachable",
                         OSD_ATTR_HINT);
        osd_print_center(dy + 3, "Y keep it    N go back to 64 MB",
                         OSD_ATTR_HINT);
    }
}

/* ------------------------------------------------------------------------ */

/* Up and down stay on the page, wrapping round within it. */
static void move_cursor(int delta)
{
    const int start = page_start(cursor), end = page_end(start);
    int i = cursor;
    for (int guard = 0; guard < ROW_COUNT; guard++) {
        i += delta;
        if (i <= start) i = end - 1;
        if (i >= end) i = start + 1;
        if (selectable(i)) { cursor = i; return; }
    }
}

/* The first row that can be chosen on the next or the previous page. */
static void change_page(int delta)
{
    int start = page_start(cursor);
    for (int guard = 0; guard < ROW_COUNT; guard++) {
        if (delta > 0) {
            start = page_end(start);
            if (start >= ROW_COUNT) start = 0;
        } else {
            start = start > 0 ? page_start(start - 1) : page_start(ROW_COUNT - 1);
        }
        for (int i = start + 1; i < page_end(start); i++)
            if (selectable(i)) { cursor = i; scroll = start + 1; return; }
    }
}

static void change(int delta)
{
    const Row *r = &rows[cursor];
    if (!r->get || !r->set) return;
    if (fpu_is_fixed(r)) return;
    const int v = r->get();

    switch (r->kind) {
    case ROW_TOGGLE:
        r->set(!v);
        break;
    case ROW_VOLUME: {
        int n = v + delta;
        if (n < 0) n = 0;
        if (n > 16) n = 16;
        r->set(n);
        break;
    }
    case ROW_CHOICE: {
        if (r->get == config_get_mem_size_mb) mem_before_confirm = v;
        int n = 0;
        const int *opt = row_options(r, &n);
        int idx = 0;
        for (int i = 0; i < n; i++)
            if (opt[i] == v) { idx = i; break; }
        idx += delta;
        if (idx < 0) idx = n - 1;
        if (idx >= n) idx = 0;
        r->set(opt[idx]);
        /* Ask on the way past 64 MB, not on every press above it, so that
         * stepping from 128 to 256 does not repeat the question. */
        if (r->get == config_get_mem_size_mb && opt[idx] > 64 && v <= 64)
            state = STATE_CONFIRM_BIGMEM;
        break;
    }
    default:
        break;
    }
}

bool settingsui_handle_key(int keycode, bool is_down)
{
    if (state == STATE_CLOSED) return false;
    if (!is_down) return true;

    if (state == STATE_SAVED) {           /* any key dismisses it */
        state = STATE_MAIN;
        draw();
        return true;
    }

    if (state == STATE_CONFIRM_RESTART) {
        if (keycode == KEY_Y || keycode == KEY_ENTER) {
            restart_requested = true;
            config_clear_changes();
            settingsui_close();
        } else if (keycode == KEY_N || keycode == KEY_ESC) {
            state = STATE_MAIN;
            config_clear_changes();
            draw();
        }
        return true;
    }

    if (state == STATE_CONFIRM_BIGMEM) {
        if (keycode == KEY_N || keycode == KEY_ESC)
            config_set_mem_size_mb(mem_before_confirm > 64 ? 64 : mem_before_confirm);
        if (keycode == KEY_Y || keycode == KEY_N ||
            keycode == KEY_ENTER || keycode == KEY_ESC) {
            state = STATE_MAIN;
            draw();
        }
        return true;
    }

    switch (keycode) {
    case KEY_TAB:
    case KEY_PAGEDOWN: change_page(+1); draw(); break;
    case KEY_PAGEUP:   change_page(-1); draw(); break;
    case KEY_UP:    move_cursor(-1); draw(); break;
    case KEY_DOWN:  move_cursor(+1); draw(); break;
    case KEY_LEFT:  change(-1); draw(); break;
    case KEY_RIGHT: change(+1); draw(); break;
    case KEY_SPACE: change(+1); draw(); break;
    case KEY_ENTER:
        if (config_save_all()) {
            state = config_needs_restart() ? STATE_CONFIRM_RESTART : STATE_SAVED;
            if (state == STATE_SAVED) config_clear_changes();
        }
        draw();
        break;
    case KEY_ESC:
        settingsui_close();
        break;
    default:
        break;
    }
    return true;
}
