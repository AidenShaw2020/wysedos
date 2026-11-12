/*
 * The on-screen menus on the host: the settings menu's pages (CPU, GPU,
 * SOUND, INPUT, POWER) with the Voodoo2 rows, and the disk manager coming back
 * from its file browser with the main panel in front.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../../pc/settingsui.h"
#include "../../pc/diskui.h"
#include "../../pc/config_save.h"
#include "../../pc/osd.h"
#include "../../pc/uikeys.h"
#include "../../pc/disk.h"
#include "../../pc/blockdev.h"
#include "../../pc/image_media.h"
#include "ff.h"

/* ---- what the menus reach outside themselves ---- */

static const char *fdd_name[2];
const char *fdd_get_filename(int i) { return i >= 0 && i < 2 ? fdd_name[i] : NULL; }
const char *ata_get_filename(int i) { (void)i; return NULL; }
uint8_t ata_is_cdrom(uint8_t drive) { return drive == 2; }
uint8_t ejectdisk(uint8_t drive, bool is_fdd) { (void)drive; (void)is_fdd; return 1; }
uint8_t insertdisk(uint8_t drive, bool is_fdd, bool is_cd, const char *path)
{
    (void)is_cd;
    if (is_fdd && drive < 2) fdd_name[drive] = path;
    return 1;
}
int image_media_available(int source) { (void)source; return 0; }
int image_media_owned(int source) { (void)source; return 0; }
int image_media_begin(int source) { (void)source; return 0; }
int image_media_end(int volume) { (void)volume; return 1; }
int blk_host_available(const char *which) { (void)which; return 0; }
uint64_t blk_host_size_mb(const char *which) { (void)which; return 0; }

static const char *const images[] = { "a.img", "b.img", "c.img", "d.img", "e.img" };
static unsigned next_image;
FRESULT f_opendir(DIR *dp, const TCHAR *path) { (void)dp; (void)path; next_image = 0; return FR_OK; }
FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }
FRESULT f_readdir(DIR *dp, FILINFO *fno)
{
    (void)dp;
    memset(fno, 0, sizeof(*fno));
    if (next_image < sizeof images / sizeof images[0]) strcpy(fno->fname, images[next_image++]);
    return FR_OK;
}
static unsigned writes;
static char saved_config[8192];
static size_t saved_bytes;
FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode) { (void)fp; (void)path; (void)mode; return FR_OK; }
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
    (void)fp;
    assert(btw < sizeof saved_config - saved_bytes);
    memcpy(saved_config + saved_bytes, buff, btw);
    saved_bytes += btw;
    saved_config[saved_bytes] = 0;
    writes++;
    *bw = btw;
    return FR_OK;
}
FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }

/* ---- helpers ---- */

static void key(int code)
{
    if (settingsui_is_open()) settingsui_handle_key(code, true);
    else diskui_handle_key(code, true);
}

static bool row_has(int row, const char *text, uint8_t *attr)
{
    const uint8_t *cells = osd_get_buffer();
    char line[OSD_COLS + 1];
    for (int col = 0; col < OSD_COLS; col++) line[col] = (char)cells[(row * OSD_COLS + col) * 2];
    line[OSD_COLS] = 0;
    const char *at = strstr(line, text);
    if (!at) return false;
    if (attr) *attr = cells[(row * OSD_COLS + (int)(at - line)) * 2 + 1];
    return true;
}

/* The row of the screen a label is on, or -1. */
static int find_row(const char *text)
{
    for (int row = 0; row < OSD_ROWS; row++)
        if (row_has(row, text, NULL)) return row;
    return -1;
}

static bool tab_current(const char *name)
{
    char padded[16];
    snprintf(padded, sizeof padded, " %s ", name);
    uint8_t attr = 0;
    return row_has(3, padded, &attr) && attr == OSD_ATTR_SELECTED;
}

static bool selected_row(const char *label)
{
    const int row = find_row(label);
    uint8_t attr = 0;
    return row >= 0 && row_has(row, label, &attr) && attr == OSD_ATTR_SELECTED;
}

static void settings_tests(void)
{
    /* Fresh firmware and installer defaults must agree before any INI or
     * hardware discovery has supplied a value. Explicit OFF still wins. */
    assert(config_get_voodoo2() == 1 && config_get_voodoo2_mem() == 12);
    assert(config_get_wgpu() == 1 && config_get_wgpu_mem() == 64);
    assert(config_get_graphics_turbo() == 1 && g_graphics_turbo == 1);
    assert(config_get_voodoo2_turbo() == 1 && g_voodoo2_turbo == 1);
    config_set_from_ini("graphics_turbo", "0");
    config_apply_all();
    assert(config_get_graphics_turbo() == 0 && g_graphics_turbo == 0);
    config_note_machine(64, 5);
    config_note_voodoo2(1, 12);
    config_note_wgpu(64, 1);
    int option_count;
    const int *options = config_voodoo2_mem_options(&option_count);
    assert(option_count == 2 && options[0] == 8 && options[1] == 12);
    settingsui_init();
    settingsui_open();
    assert(tab_current("CPU") && selected_row("Memory"));
    assert(find_row("Sound Blaster") < 0 && find_row("Voodoo2") < 0);
    /* up and down stay on the page */
    key(KEY_UP);
    assert(selected_row("Speed") && tab_current("CPU"));
    key(KEY_DOWN);
    assert(selected_row("Memory"));

    key(KEY_TAB);
    assert(tab_current("GPU") && selected_row("Video memory"));
    assert(find_row("Voodoo2 3D card") >= 0 && find_row("Memory ") < 0);
    key(KEY_DOWN); key(KEY_DOWN); key(KEY_DOWN);
    assert(selected_row("Voodoo2 memory"));
    assert(row_has(find_row("Voodoo2 memory"), "12 MB", NULL));
    key(KEY_RIGHT);
    assert(config_get_voodoo2_mem() == 8 && config_needs_restart());
    assert(row_has(find_row("Voodoo2 memory"), "8 MB", NULL));
    key(KEY_DOWN);
    assert(selected_row("CPU Boost DOS: Glide") && g_voodoo2_turbo == 1);
    key(KEY_SPACE);
    assert(config_get_voodoo2_turbo() == 0 && g_voodoo2_turbo == 0);
    key(KEY_SPACE);
    assert(g_voodoo2_turbo == 1);
    key(KEY_DOWN);
    assert(selected_row("Windows 3D accelerator") && config_get_wgpu() == 1);
    key(KEY_SPACE);
    assert(config_get_wgpu() == 0 && config_get_wgpu_mem() == 64);
    key(KEY_SPACE);
    assert(config_get_wgpu() == 1 && config_get_wgpu_mem() == 64);
    key(KEY_DOWN);
    assert(selected_row("Windows 3D memory") && tab_current("GPU"));
    assert(row_has(find_row("Windows 3D memory"), "64 MB", NULL));
    key(KEY_RIGHT);
    assert(config_get_wgpu_mem() == 128 && config_needs_restart());
    key(KEY_RIGHT);
    assert(config_get_wgpu_mem() == 32);
    key(KEY_RIGHT);
    assert(config_get_wgpu_mem() == 64 && config_get_voodoo2_mem() == 8);
    config_set_wgpu_mem(96);
    assert(config_get_wgpu_mem() == 64);
    key(KEY_DOWN);
    assert(selected_row("CPU Boost Windows: DirectX/Glide/OpenGL") && tab_current("GPU"));
    assert(g_graphics_turbo == 0);
    key(KEY_SPACE);
    assert(config_get_graphics_turbo() == 1 && g_graphics_turbo == 1);
    key(KEY_SPACE);
    assert(g_graphics_turbo == 0 && g_voodoo2_turbo == 1);

    key(KEY_TAB);
    assert(tab_current("SOUND") && selected_row("Volume"));
    key(KEY_TAB);
    assert(tab_current("INPUT") && selected_row("Mouse"));
    key(KEY_PAGEDOWN);
    assert(tab_current("POWER"));
    key(KEY_TAB);
    assert(tab_current("CPU") && selected_row("Memory"));
    key(KEY_PAGEUP);
    assert(tab_current("POWER"));
    /* a long page scrolls inside itself */
    key(KEY_PAGEUP); key(KEY_PAGEUP);
    assert(tab_current("SOUND"));
    for (int i = 0; i < 14; i++) key(KEY_DOWN);          /* the 15th row, after a scroll */
    assert(selected_row("Covox / Sound Source") && tab_current("SOUND"));
    key(KEY_DOWN);                                       /* wraps to the page top */
    assert(selected_row("Volume") && tab_current("SOUND"));

    writes = 0;
    key(KEY_ENTER);                       /* saves; a restart is offered */
    assert(writes > 0);
    assert(strstr(saved_config, "graphics_turbo=0") != NULL);
    assert(strstr(saved_config, "voodoo2_turbo=1") != NULL);
    assert(strstr(saved_config, "wgpu=64\n") != NULL);
    assert(strstr(saved_config, "wgpu_enabled=1\n") != NULL);
    key(KEY_N);
    key(KEY_ESC);
    assert(!settingsui_is_open());
    puts("Settings menu pages, Voodoo2 memory (restart) and turbo (live), page wrap and scroll: PASS");
}

static void disk_tests(void)
{
    diskui_init();
    diskui_open();
    const int main_y = osd_cell_panel_y(12, 6);
    assert(main_y >= 0);
    /* to FDD-1 and back a few times, as a hand on the keys does */
    key(KEY_DOWN); key(KEY_UP); key(KEY_DOWN);
    const int fdd1 = find_row("[FDD-1");
    assert(fdd1 > 0 && selected_row("[FDD-1"));
    key(KEY_ENTER);                       /* the file browser */
    assert(find_row("Select Image for FDD-1") >= 0);
    key(KEY_DOWN); key(KEY_DOWN); key(KEY_DOWN); key(KEY_UP);
    key(KEY_ENTER);                       /* an image */
    assert(find_row("Select Image") < 0);
    /* the main panel is the only one, and owns the FDD-1 row again */
    assert(osd_panel_count() == 1);
    assert(osd_cell_panel_y(14, fdd1) == main_y);
    assert(row_has(fdd1, "[FDD-1", NULL) && row_has(fdd1, "c.img", NULL));
    key(KEY_ESC);
    assert(!diskui_is_open());
    puts("Disk manager: after choosing an FDD-1 image the main panel owns its rows again: PASS");
}

int main(void)
{
    settings_tests();
    disk_tests();
    puts("Menu host regression: PASS");
    return 0;
}
