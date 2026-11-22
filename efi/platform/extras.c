#include "lib.h"
#include "extras.h"
#include "ff.h"
#include "osd.h"
#include "storage.h"

#define SETTINGS "EXTRAS.CFG"
#define BACKUP "EXTRAS.BAK"
#define PENDING "EXTRAS.NEW"

static bool tab_ipv4, editing;
static int row;
static char entry[64];
static const char *notice;
static ExtrasConfig config;
extern uint64_t wyse_ticks_us(void);

static bool ascii_field(const char *s, size_t cap) {
    for (size_t i = 0; i < cap; i++) {
        unsigned char c = s[i];
        if (!c) return true;
        if (c < 33 || c > 126) return false;
    }
    return false;
}

static bool read_config(int volume, const char *name) {
    FIL file;
    unsigned got;
    ExtrasConfig saved;
    char header[4], path[16];
    snprintf(path, sizeof(path), "%d:%s", volume, name);
    if (f_open(&file, path, FA_READ) != FR_OK) return false;
    bool ok = f_size(&file) == sizeof(header) + sizeof(saved) &&
        f_read(&file, header, sizeof(header), &got) == FR_OK && got == sizeof(header) &&
        !memcmp(header, "WEX1", sizeof(header)) &&
        f_read(&file, &saved, sizeof(saved), &got) == FR_OK && got == sizeof(saved);
    f_close(&file);
    if (!ok || !ascii_field(saved.user, sizeof(saved.user)) ||
        !ascii_field(saved.password, sizeof(saved.password))) return false;
    const unsigned char *flags = (const unsigned char *)&saved;
    if (flags[0] > 1 || flags[1] > 1 || flags[2] > 1) return false;
    config = saved;
    return true;
}

static bool save_config(void) {
    int volume = storage_open_boot_volume();
    if (volume < 0) return false;
    char pending[16], backup[16], settings[16];
    snprintf(pending, sizeof(pending), "%d:%s", volume, PENDING);
    snprintf(backup, sizeof(backup), "%d:%s", volume, BACKUP);
    snprintf(settings, sizeof(settings), "%d:%s", volume, SETTINGS);
    FIL file;
    unsigned wrote;
    bool ok = f_open(&file, pending, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK;
    if (ok) {
        ok = f_write(&file, "WEX1", 4, &wrote) == FR_OK && wrote == 4 &&
            f_write(&file, &config, sizeof(config), &wrote) == FR_OK && wrote == sizeof(config);
        if (f_close(&file) != FR_OK) ok = false;
    }
    if (ok && f_stat(settings, NULL) == FR_OK) {
        FRESULT result = f_unlink(backup);
        ok = (result == FR_OK || result == FR_NO_FILE) && f_rename(settings, backup) == FR_OK;
    }
    if (ok) ok = f_rename(pending, settings) == FR_OK;
    if (ok) f_unlink(backup);
    storage_close_boot_volume(volume);
    return ok;
}

static void address_string(char *out, const uint8_t *ip) {
    snprintf(out, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static bool parse_address(const char *s, uint8_t *ip) {
    uint8_t value[4];
    for (int i = 0; i < 4; i++) {
        unsigned n = 0, count = 0;
        for (; *s >= '0' && *s <= '9'; s++) {
            n = n * 10 + (*s - '0');
            if (++count > 3 || n > 255) return false;
        }
        if (!count || *s != (i == 3 ? 0 : '.')) return false;
        value[i] = n;
        if (i < 3) s++;
    }
    memcpy(ip, value, sizeof(value));
    return true;
}

static unsigned address_offset(int field) {
    switch (field) {
    case 1: return offsetof(ExtrasConfig, ip);
    case 2: return offsetof(ExtrasConfig, mask);
    case 3: return offsetof(ExtrasConfig, gateway);
    default: return offsetof(ExtrasConfig, dns);
    }
}

static void draw(void) {
    const char *ftp_labels[] = {"FTP server", "Write to WyseDOS partition", "Username", "Password", "Service status"};
    const char *ip_labels[] = {"Address mode", "IP address", "Subnet mask", "Gateway", "DNS"};
    const ExtrasConfig *actual = extras_ftp_lease();
    osd_clear();
    osd_draw_box_titled(5, 3, 70, 19, "Extras", OSD_ATTR_BORDER);
    osd_print(9, 6, " FTP ", tab_ipv4 ? OSD_ATTR_NORMAL : OSD_ATTR_SELECTED);
    osd_print(16, 6, " IPv4 ", tab_ipv4 ? OSD_ATTR_SELECTED : OSD_ATTR_NORMAL);
    for (int i = 0; i < 5; i++) {
        char value[64] = "";
        bool masked = !tab_ipv4 && i == 3;
        bool disabled = tab_ipv4 && i > 0 && config.dhcp;
        uint8_t attr = disabled ? OSD_ATTR_DISABLED : i == row ? OSD_ATTR_SELECTED :
            !tab_ipv4 && i == 4 ? OSD_ATTR_VALUE : OSD_ATTR_NORMAL;
        if (tab_ipv4) {
            if (!i) snprintf(value, sizeof(value), "%s", config.dhcp ? "DHCP" : "Static");
            else if (!config.dhcp || actual)
                address_string(value, (const uint8_t *)(config.dhcp ? actual : &config) + address_offset(i));
            else strcpy(value, "-");
        } else {
            const char *values[] = {config.enabled ? "On" : "Off", config.esp_write ? "Yes" : "No",
                config.user, config.password, extras_ftp_status()};
            snprintf(value, sizeof(value), "%s", values[i]);
        }
        if (i == row && editing) snprintf(value, sizeof(value), "%s", entry);
        if (masked) memset(value, '*', strlen(value));
        if (strlen(value) > 33) memmove(value, value + strlen(value) - 33, 34);
        osd_fill(8, 8 + i, 64, 1, ' ', attr);
        osd_print(9, 8 + i, (tab_ipv4 ? ip_labels : ftp_labels)[i], attr);
        osd_print(38, 8 + i, value, attr);
    }
    if (notice) osd_print(9, 16, notice, OSD_ATTR_HIGHLIGHT);
    osd_print(9, 19, editing ? "Enter Apply    Esc Cancel    Backspace Delete" :
        "Tab Switch tab    Enter Change    F2 Save    Esc Back", OSD_ATTR_HINT);
    extras_draw();
}

static void apply(void) {
    extras_ftp_reset();
    extras_ftp_start(&config);
}

void extras_open(bool installer) {
    gBS->SetWatchdogTimer(30, 0x10000, 0, NULL);
    memset(&config, 0, sizeof(config));
    config.dhcp = true;
    int volume = storage_open_boot_volume();
    if (volume >= 0 && !read_config(volume, SETTINGS)) read_config(volume, BACKUP);
    storage_close_boot_volume(volume);
    tab_ipv4 = editing = false;
    row = 0;
    notice = storage_boot_handle() ? NULL : "Settings are temporary on network boot";
    osd_init();
    osd_show();
    draw();
    apply();
    draw();
    EFI_EVENT events[2] = {gST->ConIn->WaitForKey, NULL};
    bool timed = !EFI_ERROR(gBS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &events[1])) &&
                 !EFI_ERROR(gBS->SetTimer(events[1], TimerPeriodic, 30000));
    uint64_t heartbeat = wyse_ticks_us();
    for (;;) {
        uint64_t now = wyse_ticks_us();
        if (now - heartbeat >= 5000000) {
            gBS->SetWatchdogTimer(30, 0x10000, 0, NULL);
            heartbeat = now;
        }
        if (extras_ftp_poll()) draw();
        EFI_INPUT_KEY key;
        if (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) {
            uint64_t signaled;
            if (!timed || EFI_ERROR(gBS->WaitForEvent(2, events, &signaled))) gBS->Stall(3000);
            continue;
        }
        unsigned c = key.UnicodeChar, scan = key.ScanCode;
        if (editing) {
            size_t n = strlen(entry);
            if (scan == 0x17 || c == 27) editing = false;
            else if (c == 13) {
                if (tab_ipv4 && !parse_address(entry, (uint8_t *)&config + address_offset(row)))
                    notice = "Enter a valid IPv4 address";
                else {
                    if (!tab_ipv4) strcpy(row == 2 ? config.user : config.password, entry);
                    editing = false;
                    notice = "Changed. F2 saves to the boot medium.";
                    apply();
                }
            } else if (c == 8 && n) entry[n - 1] = 0;
            else if (c >= 33 && c <= 126 && n < (tab_ipv4 ? 15 : row == 2 ? 31 : 63))
                entry[n] = c, entry[n + 1] = 0;
        } else if (scan == 0x17 || c == 27) break;
        else if (scan == 12) {
            extras_ftp_reset();
            notice = save_config() ?
                (installer ? "Saved to installer flash" : "Saved to the boot medium") : "Could not save to the boot medium";
            extras_ftp_start(&config);
        }
        else if (c == 9 || scan == 3 || scan == 4) {
            tab_ipv4 = !tab_ipv4;
            row = 0;
        } else if (scan == 1 || scan == 2) {
            int rows = tab_ipv4 ? (config.dhcp ? 1 : 5) : 4;
            row = (row + (scan == 1 ? rows - 1 : 1)) % rows;
        } else if (c == 13 || c == ' ') {
            if ((tab_ipv4 && !row) || (!tab_ipv4 && row < 2)) {
                bool *setting = tab_ipv4 ? &config.dhcp : row ? &config.esp_write : &config.enabled;
                *setting = !*setting;
                notice = "Changed. F2 saves to the boot medium.";
                apply();
            } else {
                editing = true;
                if (tab_ipv4) address_string(entry, (uint8_t *)&config + address_offset(row));
                else if (row == 2) strcpy(entry, config.user);
                else entry[0] = 0;
            }
        }
        draw();
    }
    if (events[1]) gBS->CloseEvent(events[1]);
    extras_ftp_stop();
    memset(entry, 0, sizeof(entry));
    memset(&config, 0, sizeof(config));
    osd_hide();
    gBS->SetWatchdogTimer(installer ? 0 : 60, installer ? 0 : 0x10000, 0, NULL);
}
