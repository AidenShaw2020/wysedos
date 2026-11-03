// The wyseDOS installer: the same bootx64.efi, started from a medium whose
// root holds install.ini (a USB stick made by tools/installer, or netfs on
// the development PC).
//
// It checks that the machine is a Dell Wyse 3040, finds the eMMC, and after
// the user confirms with Y it
//   - writes a GPT with a 128 MiB firmware partition and a private guest container,
//   - creates a guest MBR and full-capacity FAT32 inside that container,
//   - copies only runtime assets onto the firmware partition (no disk images),
//   - adds a "wyseDOS" boot entry first in the boot order.
// Progress is drawn over the startup logo and sent to the UDP log.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ff.h"
#include "fatvol.h"
#include "bio.h"
#include "blkcache.h"
#include "storage.h"
#include "installer.h"
#include "delltoken.h"
#include "native_disk.h"

extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;
uint64_t wyse_ticks_us(void);
void wyse_console_log(bool on);

static const EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static const EFI_GUID esp_type = {0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};
static const EFI_GUID data_type = WYSE_GUEST_TYPE;
static const EFI_GUID global_var = EFI_GLOBAL_VARIABLE_GUID;

#define MB_BLOCKS 2048ull           // 1 MB in 512-byte blocks
#define ESP_BLOCKS (128 * MB_BLOCKS)

//--------------------------------------------------------------------+
// console
//--------------------------------------------------------------------+

void installer_ui_begin(void);
void installer_ui_status(const char *step,const char *detail,unsigned percent);
static bool graphical;
static const char *copy_step="4 / 6 - Installing wyseDOS";
static uint64_t last_step_us;
static void step_wait(uint64_t min_us) {
    if (!graphical || !last_step_us) return;
    while (wyse_ticks_us() - last_step_us < min_us) __asm__ volatile("pause");
}
static void step(const char *label,const char *detail,unsigned pct) {
    step_wait(1800000);
    printf("installer: %u%% %s: %s\n",pct,label,detail);
    if (graphical) {
        installer_ui_status(label,detail,pct);
        last_step_us = wyse_ticks_us();
    }
}

static int wait_key(void) {
    EFI_INPUT_KEY k;
    gST->ConIn->Reset(gST->ConIn, false);
    for (;;) {
        uint64_t idx;
        gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &idx);
        if (!EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &k))) return k.UnicodeChar ? k.UnicodeChar : 0x100 + k.ScanCode;
    }
}

__attribute__((noreturn)) static void stop(const char *why) {
    printf("\nInstallation stopped: %s\nPress any key to restart.\n", why);
    last_step_us = 0;
    step("Installation stopped",why,0);
    wait_key();
    gST->RuntimeServices->ResetSystem(EfiResetCold, 0, 0, NULL);
    for (;;) __asm__ volatile("hlt");
}

//--------------------------------------------------------------------+
// SMBIOS: manufacturer and product name (type 1)
//--------------------------------------------------------------------+

static const char *smbios_string(const uint8_t *s, int idx) {
    if (!idx) return "";
    const char *p = (const char *)s + s[1];
    while (--idx && *p) p += strlen(p) + 1;
    return p;
}

// Diagnostics ("dmi"): the SMBIOS structure types there are, and Dell's own:
// 0xDA describes the SMI calling interface that changes BIOS settings
// (tokens), 0xD4/0xD6 list the tokens.
void installer_dmi_report(void) {
    static const EFI_GUID g3 = SMBIOS3_TABLE_GUID;
    const uint8_t *table = NULL, *end = NULL;
    for (uint64_t i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &gST->ConfigurationTable[i];
        const uint8_t *ep = t->VendorTable;
        if (!memcmp(&t->VendorGuid, &g3, sizeof(EFI_GUID)) && !memcmp(ep, "_SM3_", 5)) {
            table = (const uint8_t *)(uintptr_t)*(const uint64_t *)(ep + 0x10);
            end = table + *(const uint32_t *)(ep + 0x0c);
        }
    }
    if (!table) { printf("dmi: no SMBIOS 3 table\n"); return; }
    char line[400];
    int n = snprintf(line, sizeof(line), "dmi: types");
    int tokens = 0;
    for (const uint8_t *s = table; s + 4 <= end && s[0] != 127;) {
        if (n < (int)sizeof(line) - 8) n += snprintf(line + n, sizeof(line) - n, " %02x", s[0]);
        if (s[0] == 0xda) {
            char hex[200];
            int k = 0;
            for (int j = 0; j < s[1] && j < 40; j++) k += snprintf(hex + k, sizeof(hex) - k, "%02x ", s[j]);
            printf("dmi: Dell calling interface (DA): %s\n", hex);
        }
        if (s[0] == 0xd4 || s[0] == 0xd6) tokens += (s[1] - 0x0c) / 6;
        const uint8_t *p = s + s[1];
        while (p + 1 < end && (p[0] || p[1])) p++;
        s = p + 2;
    }
    printf("%s\n", line);
    printf("dmi: about %d Dell tokens listed (D4/D6)\n", tokens);
}

static bool system_names(char *maker, char *product, int cap) {
    static const EFI_GUID g2 = SMBIOS_TABLE_GUID, g3 = SMBIOS3_TABLE_GUID;
    const uint8_t *table = NULL, *end = NULL;
    for (uint64_t i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &gST->ConfigurationTable[i];
        const uint8_t *ep = t->VendorTable;
        if (!memcmp(&t->VendorGuid, &g3, sizeof(EFI_GUID)) && !memcmp(ep, "_SM3_", 5)) {
            table = (const uint8_t *)(uintptr_t)*(const uint64_t *)(ep + 0x10);
            end = table + *(const uint32_t *)(ep + 0x0c);
            break;
        }
        if (!memcmp(&t->VendorGuid, &g2, sizeof(EFI_GUID)) && !memcmp(ep, "_SM_", 4)) {
            table = (const uint8_t *)(uintptr_t)*(const uint32_t *)(ep + 0x18);
            end = table + *(const uint16_t *)(ep + 0x16);
        }
    }
    if (!table) return false;
    for (const uint8_t *s = table; s + 4 <= end && s[0] != 127;) {
        if (s[0] == 1 && s[1] >= 6) {
            snprintf(maker, cap, "%s", smbios_string(s, s[4]));
            snprintf(product, cap, "%s", smbios_string(s, s[5]));
            return true;
        }
        const uint8_t *p = s + s[1];
        while (p + 1 < end && (p[0] || p[1])) p++;
        s = p + 2;
    }
    return false;
}

//--------------------------------------------------------------------+
// the eMMC: the biggest fixed whole-disk device that is not on USB
//--------------------------------------------------------------------+

static EFI_BLOCK_IO_PROTOCOL *find_emmc(char *path, int cap) {
    uint64_t n;
    EFI_HANDLE *hs;
    EFI_BLOCK_IO_PROTOCOL *best = NULL;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &hs))) return NULL;
    for (uint64_t i = 0; i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &bio_guid, (void **)&bio))) continue;
        EFI_BLOCK_IO_MEDIA *m = bio->Media;
        EFI_DEVICE_PATH *dp = bio_device_path(hs[i]);
        if (m->LogicalPartition || m->RemovableMedia || !m->MediaPresent || m->ReadOnly || m->BlockSize != 512) continue;
        if (bio_path_has(dp, DP_MESSAGING, DP_MSG_USB)) continue;
        if (best && m->LastBlock <= best->Media->LastBlock) continue;
        best = bio;
        bio_path_str(dp, path, cap);
    }
    gBS->FreePool(hs);
    return best;
}

//--------------------------------------------------------------------+
// GPT
//--------------------------------------------------------------------+

static uint32_t crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t c = 0xffffffffu;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}

static void random_guid(EFI_GUID *g) {
    uint8_t *b = (uint8_t *)g;
    for (int i = 0; i < 16; i++) {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t x = ((uint64_t)hi << 32 | lo) * 0x9e3779b97f4a7c15ull + i * 0x632be59bd9b4e019ull;
        b[i] = (uint8_t)(x >> 29 ^ x >> 41);
        for (volatile int d = 0; d < 37 + (int)(x & 63); d++) {}
    }
    b[7] = (b[7] & 0x0f) | 0x40;    // version 4
    b[8] = (b[8] & 0x3f) | 0x80;
}

typedef struct __attribute__((packed)) {
    EFI_GUID type, unique;
    uint64_t first, last, attributes;
    uint16_t name[36];
} gpt_entry;

typedef struct __attribute__((packed)) {
    char signature[8];
    uint32_t revision, header_size, header_crc, reserved;
    uint64_t my_lba, alternate_lba, first_usable, last_usable;
    EFI_GUID disk;
    uint64_t entries_lba;
    uint32_t entry_count, entry_size, entries_crc;
} gpt_header;

typedef struct {
    uint64_t first, count;
    EFI_GUID unique;
} part_t;

static void set_name(uint16_t *dst, const char *s) {
    for (int i = 0; i < 36 && s[i]; i++) dst[i] = (uint8_t)s[i];
}

static bool write_gpt(EFI_BLOCK_IO_PROTOCOL *bio, part_t *esp, part_t *data) {
    uint64_t last = bio->Media->LastBlock;
    static uint8_t block[512];
    static gpt_entry entries[128];          // 32 blocks

    esp->first = MB_BLOCKS;
    esp->count = ESP_BLOCKS;
    data->first = esp->first + esp->count;
    data->count = ((last - 33) / MB_BLOCKS * MB_BLOCKS) - data->first;
    random_guid(&esp->unique);
    random_guid(&data->unique);

    memset(entries, 0, sizeof(entries));
    entries[0].type = esp_type;
    entries[0].unique = esp->unique;
    entries[0].first = esp->first;
    entries[0].last = esp->first + esp->count - 1;
    set_name(entries[0].name, "EFI system partition");
    entries[1].type = data_type;
    entries[1].unique = data->unique;
    entries[1].first = data->first;
    entries[1].last = data->first + data->count - 1;
    set_name(entries[1].name, "wyseDOS guest disk");

    gpt_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.signature, "EFI PART", 8);
    h.revision = 0x00010000;
    h.header_size = sizeof(h);
    h.first_usable = 34;
    h.last_usable = last - 33;
    random_guid(&h.disk);
    h.entry_count = 128;
    h.entry_size = sizeof(gpt_entry);
    h.entries_crc = crc32(entries, sizeof(entries));

    // protective MBR
    memset(block, 0, sizeof(block));
    uint8_t *e = block + 446;
    e[1] = 0x00; e[2] = 0x02; e[3] = 0x00;
    e[4] = 0xee;
    e[5] = 0xff; e[6] = 0xff; e[7] = 0xff;
    uint32_t one = 1, span = last > 0xffffffffull ? 0xffffffffu : (uint32_t)last;
    memcpy(e + 8, &one, 4);
    memcpy(e + 12, &span, 4);
    block[510] = 0x55;
    block[511] = 0xaa;
    if (EFI_ERROR(bio_write(bio, 0, 1, block))) return false;

    // backup copy at the end, then the primary
    gpt_header b = h;
    b.my_lba = last;
    b.alternate_lba = 1;
    b.entries_lba = last - 32;
    b.header_crc = crc32(&b, sizeof(b));
    if (EFI_ERROR(bio_write(bio, last - 32, 32, entries))) return false;
    memset(block, 0, sizeof(block));
    memcpy(block, &b, sizeof(b));
    if (EFI_ERROR(bio_write(bio, last, 1, block))) return false;

    h.my_lba = 1;
    h.alternate_lba = last;
    h.entries_lba = 2;
    h.header_crc = crc32(&h, sizeof(h));
    if (EFI_ERROR(bio_write(bio, 2, 32, entries))) return false;
    memset(block, 0, sizeof(block));
    memcpy(block, &h, sizeof(h));
    if (EFI_ERROR(bio_write(bio, 1, 1, block))) return false;
    return !EFI_ERROR(bio_flush(bio));
}

//--------------------------------------------------------------------+
// copying
//--------------------------------------------------------------------+

static uint64_t total_bytes, copied_bytes, copy_start_us;
static int last_percent = -1;

static void progress(void) {
    if (!total_bytes) return;
    int pct = total_bytes ? (int)(copied_bytes * 100 / total_bytes) : 100;
    if (pct > 100) pct = 100;
    if (pct == last_percent) return;
    if (last_percent < 0) step_wait(1800000);
    else step_wait(35000);
    last_percent = pct;
    char detail[100];
    snprintf(detail,sizeof(detail),"%d%% - %llu / %llu MiB",pct,
        (unsigned long long)(copied_bytes>>20),(unsigned long long)(total_bytes>>20));
    if (graphical) {
        installer_ui_status(copy_step,detail,30+(unsigned)pct*60/100);
        last_step_us = wyse_ticks_us();
    }
    uint64_t ms = (wyse_ticks_us() - copy_start_us) / 1000 + 1;
    printf("  %3d %%  %llu of %llu MB, %llu MB/s\r", pct, (unsigned long long)(copied_bytes >> 20),
           (unsigned long long)(total_bytes >> 20), (unsigned long long)(copied_bytes / 1000 / ms));
}

// The bytes under a source directory, for the progress display
static bool runtime_file(const char *name) {
    const char *dot=strrchr(name,'.');
    return strncmp(name,"emmc-before-native",18) && strcmp(name,"config.ini") &&
        (!dot || (strcasecmp(dot,".img") && strcasecmp(dot,".partial")));
}
static uint64_t tree_size(const char *dir, bool runtime) {
    DIR d;
    FILINFO fi;
    uint64_t sum = 0;
    if (f_opendir(&d, dir) != FR_OK) return 0;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        char p[300];
        snprintf(p, sizeof(p), "%s/%s", dir, fi.fname);
        sum += fi.fattrib & AM_DIR ? tree_size(p,runtime) : ((!runtime || runtime_file(fi.fname))?fi.fsize:0);
    }
    f_closedir(&d);
    return sum;
}

static bool copy_file(const char *from, const char *to) {
    static uint8_t buf[1 << 20] __attribute__((aligned(4096)));
    FIL in, out;
    if (f_open(&in, from, FA_READ) != FR_OK) {
        printf("cannot open %s\n", from);
        return false;
    }
    uint64_t size = f_size(&in);
    if (f_open(&out, to, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        f_close(&in);
        printf("cannot create %s\n", to);
        return false;
    }
    // disk images in one contiguous run
    if (out.backend == FS_FAT && size >= (1u << 20) && f_expand(&out, size) != FR_OK) {
        printf("no room for %s (%llu MB)\n", to, (unsigned long long)(size >> 20));
        f_close(&in);
        f_close(&out);
        return false;
    }
    bool ok = true;
    for (uint64_t at = 0; at < size;) {
        UINT want = size - at < sizeof(buf) ? (UINT)(size - at) : sizeof(buf), got = 0, put = 0;
        if (f_read(&in, buf, want, &got) != FR_OK || got != want) { ok = false; break; }
        if (f_write(&out, buf, got, &put) != FR_OK || put != got) { ok = false; break; }
        at += got;
        copied_bytes += got;
        progress();
        // a disk image takes minutes over netfs: the firmware's 30 s watchdog
        // and the hang detector would reset the Wyse in the middle of it
        extern volatile uint64_t g_main_heartbeat;
        g_main_heartbeat++;
        gBS->SetWatchdogTimer(30, 0x10000, 0, NULL);
    }
    f_close(&in);
    if (f_close(&out) != FR_OK) ok = false;
    if (!ok) printf("\ncopying %s failed\n", from);
    return ok;
}

static bool copy_tree(const char *from, const char *to, bool runtime) {
    DIR d;
    FILINFO fi;
    FRESULT result=f_mkdir(to);
    if (result!=FR_OK && result!=FR_EXIST) return false;
    if (f_opendir(&d, from) != FR_OK) return false;
    bool ok = true;
    while (ok) {
        result=f_readdir(&d,&fi);
        if (result!=FR_OK) { ok=false; break; }
        if (!fi.fname[0]) break;
        char a[300], b[300];
        if (snprintf(a,sizeof(a),"%s/%s",from,fi.fname)>=(int)sizeof(a) ||
            snprintf(b,sizeof(b),"%s/%s",to,fi.fname)>=(int)sizeof(b)) { ok=false; break; }
        ok = fi.fattrib & AM_DIR ? copy_tree(a,b,runtime) :
             (runtime && !runtime_file(fi.fname)) || copy_file(a,b);
    }
    f_closedir(&d);
    return ok;
}

//--------------------------------------------------------------------+
// the boot entry
//--------------------------------------------------------------------+

static bool add_boot_entry(const part_t *esp) {
    static const char file[] = "\\EFI\\BOOT\\BOOTX64.EFI";
    static uint8_t opt[512];
    size_t n = 0;
    uint32_t attributes = 1;                    // LOAD_OPTION_ACTIVE
    memcpy(opt, &attributes, 4);
    n = 6;                                      // FilePathListLength after the description
    static const char desc[] = "wyseDOS";
    for (size_t i = 0; i <= strlen(desc); i++) { opt[n++] = (uint8_t)desc[i]; opt[n++] = 0; }
    size_t dp_start = n;
    HARDDRIVE_DEVICE_PATH hd;
    memset(&hd, 0, sizeof(hd));
    hd.Header.Type = DP_MEDIA;
    hd.Header.SubType = DP_MEDIA_HARDDRIVE;
    hd.Header.Length = sizeof(hd);
    hd.PartitionNumber = 1;
    hd.PartitionStart = esp->first;
    hd.PartitionSize = esp->count;
    memcpy(hd.Signature, &esp->unique, 16);
    hd.MBRType = 2;
    hd.SignatureType = 2;
    memcpy(opt + n, &hd, sizeof(hd));
    n += sizeof(hd);
    EFI_DEVICE_PATH fp = {DP_MEDIA, DP_MEDIA_FILEPATH, (uint16_t)(4 + 2 * (strlen(file) + 1))};
    memcpy(opt + n, &fp, 4);
    n += 4;
    for (size_t i = 0; i <= strlen(file); i++) { opt[n++] = (uint8_t)file[i]; opt[n++] = 0; }
    EFI_DEVICE_PATH end = {DP_END, 0xff, 4};
    memcpy(opt + n, &end, 4);
    n += 4;
    uint16_t dp_len = (uint16_t)(n - dp_start);
    memcpy(opt + 4, &dp_len, 2);

    // reuse an entry called wyseDOS, else the first free number
    int num = -1, free_num = -1;
    for (int i = 0; i < 0x100 && num < 0; i++) {
        CHAR16 name[9] = {'B', 'o', 'o', 't'};
        static const char hex[] = "0123456789ABCDEF";
        for (int k = 0; k < 4; k++) name[4 + k] = hex[(i >> (12 - 4 * k)) & 15];
        name[8] = 0;
        static uint8_t old[512];
        uint64_t size = sizeof(old);
        EFI_STATUS st = gST->RuntimeServices->GetVariable(name, &global_var, NULL, &size, old);
        if (st == EFI_NOT_FOUND) {
            if (free_num < 0) free_num = i;
            continue;
        }
        if (EFI_ERROR(st) || size < 6 + 16) continue;
        if (!memcmp(old + 6, opt + 6, 16)) num = i;     // "wyseDOS" in UTF-16 with its NUL
    }
    if (num < 0) num = free_num;
    if (num < 0) return false;
    CHAR16 name[9] = {'B', 'o', 'o', 't'};
    static const char hex[] = "0123456789ABCDEF";
    for (int k = 0; k < 4; k++) name[4 + k] = hex[(num >> (12 - 4 * k)) & 15];
    name[8] = 0;
    uint32_t attr = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
    if (EFI_ERROR(gST->RuntimeServices->SetVariable(name, &global_var, attr, n, opt))) return false;

    // first in the boot order, the rest as it was
    static uint16_t order[128];
    uint64_t size = sizeof(order) - 2;
    if (EFI_ERROR(gST->RuntimeServices->GetVariable((const CHAR16 *)L"BootOrder", &global_var, NULL, &size, order + 1))) size = 0;
    size_t count = 1, old = size / 2;
    order[0] = (uint16_t)num;
    for (size_t i = 1; i <= old; i++)
        if (order[i] != num) order[count++] = order[i];
    printf("boot entry Boot%04X, boot order of %u\n", num, (unsigned)count);
    return !EFI_ERROR(gST->RuntimeServices->SetVariable((const CHAR16 *)L"BootOrder", &global_var, attr, count * 2, order));
}

//--------------------------------------------------------------------+

// Development: the monitor, config.ini and the ROMs of the running source
// (netfs) onto an installed eMMC, which keeps its disk images. A file of 386/
// named in `replace` ("emmcupd games.img") is copied again even though it is there.
void installer_update_emmc(const char *replace) {
    if (fs_default_backend()!=FS_NETFS) {
        printf("emmcupd: netboot required; refusing to overwrite the running source\n");
        return;
    }
    uint64_t n;
    EFI_HANDLE *hs;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &hs))) return;
    bool efi = false, cfg = false;
    for (uint64_t i = 0; i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &bio_guid, (void **)&bio))) continue;
        if (!bio->Media->LogicalPartition || bio->Media->RemovableMedia || bio->Media->BlockSize != 512) continue;
        if (bio_path_has(bio_device_path(hs[i]), DP_MESSAGING, DP_MSG_USB)) continue;
        if (fatvol_attach(FATVOL_TARGET_ESP, bio, 0, bio->Media->LastBlock + 1) != FR_OK) continue;
        if (!efi && f_stat("2:EFI/BOOT/BOOTX64.EFI", NULL) == FR_OK)
            efi = copy_file("EFI/BOOT/BOOTX64.EFI", "2:EFI/BOOT/BOOTX64.EFI");
        if (!cfg && f_stat("2:386/config.ini", NULL) == FR_OK) {
            bool native_install=f_stat("2:386/native.ini",NULL)==FR_OK;
            bool write_native_config(void);
            cfg = native_install ? write_native_config() : copy_file("386/config.ini", "2:386/config.ini");
            // files new in the source's 386/ (a SoundFont, an image) join the
            // ones there; those already there are left as the guest wrote them
            DIR d;
            FILINFO fi;
            if (f_opendir(&d, "386") == FR_OK) {
                // under a temporary name until complete: a copy cut short
                // (a reset) is not taken for the file next time
                static const char tmp[] = "2:386/adding.tmp";
                f_unlink(tmp);
                while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
                    char a[300], b[300];
                    snprintf(a, sizeof(a), "386/%s", fi.fname);
                    snprintf(b, sizeof(b), "2:386/%s", fi.fname);
                    // the ROMs are never written by the guest: the source's win
                    bool again = (replace && *replace && !strcasecmp(replace, fi.fname)) ||
                                 !strcasecmp(fi.fname, "bios.bin") || !strcasecmp(fi.fname, "vgabios.bin");
                    if (native_install && !runtime_file(fi.fname)) continue;
                    if (fi.fattrib & AM_DIR || (f_stat(b, NULL) == FR_OK && !again)) continue;
                    printf("emmcupd: %s %s (%llu KB)\n", again ? "replacing" : "adding", a,
                           (unsigned long long)(fi.fsize >> 10));
                    if (!copy_file(a, tmp)) {
                        f_unlink(tmp);
                        continue;
                    }
                    f_unlink(b);
                    if (f_rename(tmp, b) != FR_OK) printf("emmcupd: cannot rename to %s\n", b);
                }
                f_closedir(&d);
            }
        }
        fatvol_detach(FATVOL_TARGET_ESP);
    }
    gBS->FreePool(hs);
    blkcache_flush(NULL);
    printf("emmcupd: monitor %s, config.ini %s\n", efi ? "updated" : "not found", cfg ? "updated" : "not found");
}

/* Back up the installed image while netbooted: the eMMC is not the guest C:.
 * No overwrite: an interrupted .partial is never mistaken for a complete backup. */
void installer_backup_emmc(void) {
    if (fs_default_backend()!=FS_NETFS) { printf("backup: netboot required\n"); return; }
    uint64_t n; EFI_HANDLE *hs;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&bio_guid,NULL,&n,&hs))) return;
    bool ok=false;
    for (uint64_t i=0;i<n;i++) {
        EFI_BLOCK_IO_PROTOCOL *bio;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i],&bio_guid,(void **)&bio))) continue;
        if (!bio->Media->LogicalPartition || bio->Media->RemovableMedia || bio->Media->BlockSize!=512 ||
            bio_path_has(bio_device_path(hs[i]),DP_MESSAGING,DP_MSG_USB)) continue;
        if (fatvol_attach(2,bio,0,bio->Media->LastBlock+1)!=FR_OK) continue;
        FILINFO fi;
        if (f_stat("2:386/hdd.img",&fi)==FR_OK) {
            const char *srcs[3]={"2:386/hdd.img","2:386/games.img","2:386/config.ini"};
            const char *dsts[3]={"386/emmc-before-native.img","386/emmc-before-native-games.img","386/emmc-before-native.ini"};
            ok=true;
            for (int j=0;j<3 && ok;j++) {
                FRESULT r=f_stat(srcs[j],&fi);
                if (r==FR_NO_FILE && j==1) continue;
                if (r!=FR_OK) { ok=false; break; }
                FILINFO exists;
                if (f_stat(dsts[j],&exists)==FR_OK) {
                    ok=exists.fsize==fi.fsize;
                    printf("backup: existing %s size %s\n",dsts[j],ok?"matches":"DIFFERS");
                    continue;
                }
                char tmp[128]; snprintf(tmp,sizeof(tmp),"%s.partial",dsts[j]);
                total_bytes=fi.fsize; copied_bytes=0; last_percent=-1; copy_start_us=wyse_ticks_us();
                printf("backup: %s %llu bytes -> %s\n",srcs[j],(unsigned long long)fi.fsize,tmp);
                f_unlink(tmp);
                ok=copy_file(srcs[j],tmp);
                FILINFO check;
                ok=ok && f_stat(tmp,&check)==FR_OK && check.fsize==fi.fsize;
                if (ok) ok=f_rename(tmp,dsts[j])==FR_OK;
            }
            fatvol_detach(2); break;
        }
        fatvol_detach(2);
    }
    gBS->FreePool(hs);
    printf("backup: %s\n",ok?"COMPLETE; verify SHA256 on PC before installing":"FAILED / no installed image found");
}

bool write_native_config(void) {
    FIL in,out; UINT got,put; char buf[8192];
    if (f_open(&in,"386/config.ini",FA_READ)!=FR_OK) return false;
    if (f_size(&in)>=sizeof(buf)-128) { f_close(&in); return false; }
    FRESULT r=f_read(&in,buf,sizeof(buf)-128,&got); f_close(&in);
    if (r!=FR_OK) return false;
    buf[got]=0;
    if (f_open(&out,"2:386/config.ini",FA_CREATE_ALWAYS|FA_WRITE)!=FR_OK) return false;
    bool ok=true;
    char *line=buf;
    while (*line && ok) {
        char *end=strchr(line,'\n'); if (end) *end=0;
        if (*line && line[strlen(line)-1]=='\r') line[strlen(line)-1]=0;
        const char *text=!strncmp(line,"hda=",4)?"hda=host:emmc":
                         !strncmp(line,"hdb=",4)?"hdb=":
                         !strncmp(line,"mem=",4)?"mem=16M":line;
        unsigned len=(unsigned)strlen(text);
        ok=f_write(&out,text,len,&put)==FR_OK && put==len && f_write(&out,"\r\n",2,&put)==FR_OK && put==2;
        if (!end) break;
        line=end+1;
    }
    if (f_close(&out)!=FR_OK) ok=false;
    return ok;
}

static bool prepare_guest(EFI_BLOCK_IO_PROTOCOL *bio,const part_t *guest) {
    if (guest->count<=2048 || guest->count>0xffffffffull) return false;
    static uint8_t mbr[512] __attribute__((aligned(4096)));
    FIL f; UINT got;
    if (f_open(&f,"386/guest_mbr.bin",FA_READ)!=FR_OK) return false;
    bool ok=f_read(&f,mbr,512,&got)==FR_OK && got==512; f_close(&f);
    if (!ok) return false;
    memset(mbr+446,0,64);
    uint8_t *e=mbr+446; e[0]=0x80; e[4]=0x0c;
    uint32_t start=2048, count=(uint32_t)guest->count-start;
    uint32_t values[2]={start,start+count-1};
    for (int k=0;k<2;k++) {
        uint32_t lba=values[k], c=lba/(255*63), h=(lba/63)%255, sec=lba%63+1;
        if (c>1023) { c=1023; h=254; sec=63; }
        uint8_t *p=e+(k?5:1); p[0]=(uint8_t)h; p[1]=(uint8_t)(sec|((c>>2)&0xc0)); p[2]=(uint8_t)c;
    }
    memcpy(e+8,&start,4); memcpy(e+12,&count,4); mbr[510]=0x55; mbr[511]=0xaa;
    if (EFI_ERROR(bio_write(bio,guest->first,1,mbr))) return false;
    /* Clear the rest of the guest's reserved first MiB, including stale boot metadata. */
    static uint8_t zero[65536] __attribute__((aligned(4096)));
    for (uint64_t off=1;off<2048;) {
        uint64_t n=2048-off<128?2048-off:128;
        if (EFI_ERROR(bio_write(bio,guest->first+off,n,zero))) return false;
        off+=n;
    }
    int r=fatvol_attach(3,bio,guest->first+start,count);
    if (r!=FR_OK && r!=FR_NO_FILESYSTEM) return false;
    if (fatvol_mkfs_dos(3)!=FR_OK) return false;
    fatvol_detach(3);
    if (EFI_ERROR(blkcache_flush(bio))) return false;
    /* FatFs formats a range as a superfloppy. DOS's BPB must use GUEST offsets. */
    for (unsigned k=0;k<2;k++) {
        uint64_t lba=guest->first+start+(k?6:0);
        if (EFI_ERROR(bio_read(bio,lba,1,mbr))) return false;
        uint16_t sectors=63,heads=255;
        memcpy(mbr+24,&sectors,2); memcpy(mbr+26,&heads,2); memcpy(mbr+28,&start,4); mbr[64]=0x80;
        if (memcmp(mbr+82,"FAT32   ",8) || mbr[510]!=0x55 || mbr[511]!=0xaa) return false;
        if (EFI_ERROR(bio_write(bio,lba,1,mbr))) return false;
    }
    blkcache_forget(bio);
    return !EFI_ERROR(bio_flush(bio));
}

bool installer_wanted(void) {
    FILINFO fi;
    return f_stat("install.ini", &fi) == FR_OK;
}

void installer_run(void) {
    wyse_console_log(false);
    gBS->SetWatchdogTimer(0,0,0,NULL);
    installer_ui_begin(); graphical=true; last_step_us=0;
    step("wyseDOS device installer","Checking Dell Wyse 3040 and installation files",0);
    char maker[64]="?",product[64]="?",path[160],detail[128];
    system_names(maker,product,sizeof(maker));
    if (!strstr(maker,"Dell") || !strstr(product,"3040")) stop("This is not a Dell Wyse 3040");
    EFI_BLOCK_IO_PROTOCOL *emmc=find_emmc(path,sizeof(path));
    if (!emmc) stop("No eMMC found");
    uint64_t blocks=emmc->Media->LastBlock+1;
    if (blocks<2048*MB_BLOCKS || blocks>0xffffffffull) stop("Unsupported eMMC capacity");
    if (f_stat("EFI/BOOT/BOOTX64.EFI",NULL)!=FR_OK || f_stat("386/config.ini",NULL)!=FR_OK ||
        f_stat("386/guest_mbr.bin",NULL)!=FR_OK || f_stat("386/bios.bin",NULL)!=FR_OK ||
        f_stat("386/vgabios.bin",NULL)!=FR_OK) stop("Installation files missing");
    FILINFO driver_dir;
    if (f_stat("DRIVERS",&driver_dir)!=FR_OK || !(driver_dir.fattrib&AM_DIR))
        stop("Unpacked DRIVERS bundle missing");
    static const char *driver_files[]={"DRIVERS/README.TXT","DRIVERS/SHA256.TXT",
        "DRIVERS/VGA/vmdisp9x.inf","DRIVERS/USBDOS/USBUHCIL.COM",
        "DRIVERS/USBDOS/USBDRIVE.COM","DRIVERS/CTMOUSE/CTMOUSE.EXE",
        "DRIVERS/NUSBFE/_nusb.inf","DRIVERS/NUSBSE/_nusb.inf"};
    for (unsigned i=0;i<sizeof(driver_files)/sizeof(driver_files[0]);i++) {
        FILINFO file;
        if (f_stat(driver_files[i],&file)!=FR_OK || !file.fsize || (file.fattrib&AM_DIR))
            stop("Driver bundle incomplete - eMMC unchanged");
    }
    total_bytes=tree_size("386",true); copied_bytes=0; last_percent=-1;
    if (total_bytes+4*1024*1024>ESP_BLOCKS*512) stop("Monitor files do not fit firmware partition");
    total_bytes+=tree_size("DRIVERS",false);
    step_wait(3000000);
    snprintf(detail,sizeof(detail),"eMMC %llu MiB. ERASE eMMC? Y = install, any other key = cancel",
        (unsigned long long)(blocks/2048));
    step("Back up your data before continuing",detail,0);
    int k=wait_key();
    last_step_us=0;
    if (k!='y' && k!='Y') stop("Cancelled - eMMC unchanged");
    step("1 / 6 - Partitioning eMMC","Creating protected firmware area and isolated guest disk",5);
    if (EFI_ERROR(blkcache_flush(emmc))) stop("Cannot flush old filesystem");
    blkcache_forget(emmc);
    part_t esp,guest;
    if (!write_gpt(emmc,&esp,&guest)) stop("Partition table write failed");
    step("2 / 6 - Formatting firmware partition","128 MiB for wyseDOS and its settings",10);
    int r=fatvol_attach(2,emmc,esp.first,esp.count);
    if (r!=FR_OK && r!=FR_NO_FILESYSTEM) stop("Cannot open firmware partition");
    if (fatvol_mkfs(2,"WYSEDOS EFI")!=FR_OK) stop("Firmware format failed");
    snprintf(detail,sizeof(detail),"FAT32 C: %llu MiB - remaining eMMC capacity",(unsigned long long)((guest.count-2048)/2048));
    step("3 / 6 - Preparing guest hard disk",detail,18);
    if (!prepare_guest(emmc,&guest)) stop("Guest FAT32 format failed");
    step("4 / 6 - Installing wyseDOS","Copying monitor, ROMs, logo and soundfont",30);
    copy_start_us=wyse_ticks_us();
    f_mkdir("2:EFI"); f_mkdir("2:EFI/BOOT");
    if (!copy_file("EFI/BOOT/BOOTX64.EFI","2:EFI/BOOT/BOOTX64.EFI") ||
        !copy_tree("386","2:386",true) || !write_native_config()) stop("Copying monitor files failed");
    /* Mount only the freshly formatted guest FAT32 partition. The guest MBR
     * reserves 2048 sectors inside its protected container. */
    if (fatvol_attach(FATVOL_TARGET_DATA,emmc,guest.first+2048,guest.count-2048)!=FR_OK)
        stop("Cannot mount guest partition for drivers");
    if (graphical) installer_ui_status(copy_step,"Copying unpacked drivers to C:\\DRIVERS",last_percent<0?30:30+(unsigned)last_percent*60/100);
    if (!copy_tree("DRIVERS","3:DRIVERS",false)) stop("Copying guest drivers failed");
    fatvol_detach(FATVOL_TARGET_DATA);
    step("5 / 6 - Setting boot entry","UEFI starts wyseDOS; DOS sees only the guest disk",92);
    if (!add_boot_entry(&esp)) stop("Cannot write UEFI boot entry");
    step("6 / 6 - Finishing","Flushing files and configuring firmware",97);
    dell_token_activate(DELL_TOKEN_FASTBOOT_MINIMAL);
    dell_token_activate(DELL_TOKEN_VT_ENABLE);
    FIL layout; UINT put;
    if (f_open(&layout,"2:386/native.ini",FA_CREATE_ALWAYS|FA_WRITE)!=FR_OK) stop("Cannot record native layout");
    snprintf(detail,sizeof(detail),"version=1\r\nguest_first=%llu\r\nguest_blocks=%llu\r\n",
        (unsigned long long)guest.first,(unsigned long long)guest.count);
    unsigned len=(unsigned)strlen(detail);
    bool saved=f_write(&layout,detail,len,&put)==FR_OK && put==len;
    if (f_close(&layout)!=FR_OK || !saved) stop("Cannot save native layout");
    fatvol_detach(2);
    if (EFI_ERROR(blkcache_flush(emmc)) || EFI_ERROR(bio_flush(emmc))) stop("Final storage flush failed");
    gBS->SetWatchdogTimer(0,0,0,NULL);
    step("wyseDOS installation complete","Ready for operating system installation. Remove USB, press any key.",100);
    printf("installer: native FAT32 ready, guest LBA %llu + %llu, firmware protected\n",
        (unsigned long long)guest.first,(unsigned long long)guest.count);
    wait_key();
    gST->RuntimeServices->ResetSystem(EfiResetCold,0,0,NULL);
}
