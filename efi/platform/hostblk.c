// Real USB floppy, CD-ROM and flash / external disk drives behind the guest's drives.
//
// The firmware's USB mass storage driver gives each drive a Block I/O
// protocol; here they become the "ufd1", "ucd1", "uhd1"... that the disk layer
// binds with blk_bind_host() (config "usb:ufd1", "usb:uhd1", the disk menu,
// drive A: for a floppy, primary/secondary slave for a USB disk, and the
// secondary master for a USB CD drive by default when plugged in).
//
// Which is which: a UFI interface (USB subclass 4) is a floppy, 2048-byte
// blocks or an ATAPI interface (subclasses 2 and 5) a CD; any other 512-byte
// USB mass storage device is a USB disk ("uhd1", "uhd2"...).

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include "blockdev.h"
#include "disk.h"
#include "bio.h"
#include "storage.h"
#include "hostblk.h"

extern EFI_BOOT_SERVICES *gBS;
extern EFI_HANDLE gImage;
uint64_t wyse_ticks_us(void);
void wyse_vga_flush_before_io(void);

static const EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static const EFI_GUID usbio_guid = EFI_USB_IO_PROTOCOL_GUID;

#define RUN_BYTES     65536u
#define FD_MAX_BLOCKS 5760u
#define LBA28_MAX     0x0ffffff0ull

typedef enum {
    HOST_FD = 0,
    HOST_CD = 1,
    HOST_HD = 2,
} host_kind_t;

typedef struct {
    char name[8];
    EFI_BLOCK_IO_PROTOCOL *bio;
    host_kind_t kind;
    bool cd;
    uint32_t bs;                // block size of the medium (512 floppy/disk, 2048 CD)
    uint32_t run;               // blocks read at once
    uint32_t media_id;
    bool present;
    bool failed;               // identity lost: only an explicit new binding may recover
    uint64_t phys_blocks;       // physical medium size in blocks
    uint64_t blocks;            // guest-visible medium size in blocks, 0 when empty
    uint32_t lba_offset;        // guest LBA offset (63 when wrapping a superfloppy USB stick)
    bool has_synth_mbr;         // true when sector 0 is synthesized/adjusted for DOS/Win9x
    uint32_t vbr_lba[4];        // guest LBA of FAT VBRs needing HiddenSectors patched
    uint8_t synth_mbr[512];
    uint64_t cached;            // first block held, or ~0
    uint32_t cached_count;
    bool dirty;
    bool announce;              // a change seen, not yet passed on
    uint64_t last_use;
    BlockDev *dev;
    uint8_t *disk_cache;        // floppy sector cache (FD_MAX_BLOCKS * 512), preserved across rebinds
    uint64_t valid[FD_MAX_BLOCKS / 64];
    uint8_t buf[RUN_BYTES] __attribute__((aligned(4096)));
} hostblk_t;

static hostblk_t drives[8];
static int ndrives;
static int moans;
static BlockDev usb_devs[2];
static EFI_BLOCK_IO_PROTOCOL *usb_blocked[2];
static uint64_t usb_not_before[2];
static void (*usb_change)(int port, BlockDev *dev, unsigned serial);
static void usb_poll(void);

//--------------------------------------------------------------------+
// finding the drives
//--------------------------------------------------------------------+

typedef struct {
    EFI_BLOCK_IO_PROTOCOL *bio;
    host_kind_t kind;
} found_t;

// Whether two device paths belong to the same physical disk (up to their media node)
static bool same_usb_disk(EFI_HANDLE a, EFI_HANDLE b) {
    if (!a || !b) return false;
    EFI_DEVICE_PATH *pa = bio_device_path(a), *pb = bio_device_path(b);
    if (!pa || !pb) return false;
    if (!bio_path_has(pa, DP_MESSAGING, DP_MSG_USB) || !bio_path_has(pb, DP_MESSAGING, DP_MSG_USB))
        return false;
    for (;;) {
        if (dp_end(pa) || dp_end(pb) || pa->Length < 4 || pb->Length < 4) return true;
        if (pa->Type == DP_MEDIA || pb->Type == DP_MEDIA) return true;
        if (pa->Length != pb->Length || memcmp(pa, pb, pa->Length)) return false;
        pa = dp_next(pa);
        pb = dp_next(pb);
    }
}

// The USB floppies, CD drives and flash/external disks the firmware has, in its handle order
static int scan(found_t *out, int cap) {
    uint64_t n;
    EFI_HANDLE *hs;
    int count = 0;
    EFI_HANDLE boot_h = storage_boot_handle();
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &hs))) return 0;
    for (uint64_t i = 0; i < n && count < cap; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &bio_guid, (void **)&bio))) continue;
        EFI_BLOCK_IO_MEDIA *m = bio->Media;
        if (!m) continue;
        if (m->LogicalPartition || !bio_path_has(bio_device_path(hs[i]), DP_MESSAGING, DP_MSG_USB)) continue;
        if (boot_h && same_usb_disk(hs[i], boot_h)) continue;
        int subclass = -1;
        EFI_USB_IO_PROTOCOL *io;
        USB_INTERFACE_DESCRIPTOR ifd;
        if (!EFI_ERROR(gBS->HandleProtocol(hs[i], &usbio_guid, (void **)&io)) &&
            !EFI_ERROR(io->UsbGetInterfaceDescriptor(io, &ifd)) && ifd.InterfaceClass == 8)
            subclass = ifd.InterfaceSubClass;
        bool cd = m->BlockSize == 2048 || subclass == 2 || (!m->MediaPresent && subclass == 5);
        bool floppy = !cd && (subclass == 4 || (m->MediaPresent && m->BlockSize == 512 && m->LastBlock < 5760));
        bool hd = !cd && !floppy && (m->BlockSize == 512 || !m->MediaPresent);
        if (!cd && !floppy && !hd) continue;
        out[count].bio = bio;
        out[count].kind = cd ? HOST_CD : floppy ? HOST_FD : HOST_HD;
        count++;
    }
    gBS->FreePool(hs);
    return count;
}

static bool parse_which(const char *which, host_kind_t *want_kind, int *nth) {
    if (!which) return false;
    if (!strncmp(which, "ufd", 3)) *want_kind = HOST_FD;
    else if (!strncmp(which, "ucd", 3)) *want_kind = HOST_CD;
    else if (!strncmp(which, "uhd", 3)) *want_kind = HOST_HD;
    else return false;
    *nth = which[3] >= '1' && which[3] <= '9' ? which[3] - '0' : 1;
    return true;
}

// The drive the host knows as `which` ("ufd1", "ucd1", "uhd1"), or NULL
static EFI_BLOCK_IO_PROTOCOL *lookup(const char *which, host_kind_t *kind) {
    host_kind_t want;
    int nth;
    if (!parse_which(which, &want, &nth)) return NULL;
    found_t f[16];
    int n = scan(f, 16);
    for (int i = 0; i < n; i++) {
        if (f[i].kind != want || --nth) continue;
        if (kind) *kind = want;
        return f[i].bio;
    }
    return NULL;
}

void hostblk_list(void) {
    found_t f[16];
    int n = scan(f, 16), fd = 0, cd = 0, hd = 0;
    for (int i = 0; i < n; i++) {
        EFI_BLOCK_IO_MEDIA *m = f[i].bio->Media;
        const char *pfx = f[i].kind == HOST_CD ? "ucd" : f[i].kind == HOST_FD ? "ufd" : "uhd";
        int idx = f[i].kind == HOST_CD ? ++cd : f[i].kind == HOST_FD ? ++fd : ++hd;
        printf("usb drive: %s%d, %s, %u x %llu (%llu MB)\n", pfx, idx,
               m->MediaPresent ? "medium" : "empty", m->BlockSize,
               (unsigned long long)(m->LastBlock + 1),
               (unsigned long long)(((uint64_t)(m->LastBlock + 1) * m->BlockSize) >> 20));
    }
    if (!n) printf("usb drive: none\n");
}

void hostblk_poll(void) {
    static int prev_count = -1;
    static EFI_BLOCK_IO_PROTOCOL *prev_bios[16];
    found_t f[16];
    int n = scan(f, 16);
    bool diff = (n != prev_count);
    for (int i = 0; !diff && i < n; i++)
        if (f[i].bio != prev_bios[i]) diff = true;
    if (diff) {
        prev_count = n;
        for (int i = 0; i < n; i++) prev_bios[i] = f[i].bio;
        hostblk_list();
        if (!fdd_get_filename(0) && blk_host_available("ufd1"))
            insertdisk(0, true, false, "usb:ufd1");
        if (!fdd_get_filename(1) && blk_host_available("ufd2"))
            insertdisk(1, true, false, "usb:ufd2");
    }
    usb_poll();
}

//--------------------------------------------------------------------+
// the medium
//--------------------------------------------------------------------+

// The Wyse's firmware gives a USB drive's LastBlock as the number of blocks
// (e.g. 2881 for a 1.44 MB diskette, or 31342593 for a 31342592-sector USB stick).
static uint64_t medium_blocks(const hostblk_t *h, uint64_t n) {
    static const uint16_t sizes[] = {320, 360, 640, 720, 1280, 1440, 2400, 2880, 3040, 3200, 3360, 5760};
    if (h->kind == HOST_CD) return n;
    if (h->kind == HOST_FD) {
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
            if (n == sizes[i]) return n;
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
            if (n == sizes[i] + 1u) return n - 1;
        return n;
    }
    if (n > 1 && (n & 1)) n--;
    if (n > LBA28_MAX) n = LBA28_MAX;
    return n;
}

static void fill_mbr_chs(uint8_t *p, uint32_t lba) {
    uint32_t c = lba / (255u * 63u), h = (lba / 63u) % 255u, s = (lba % 63u) + 1u;
    if (c > 1023u) { c = 1023u; h = 254u; s = 63u; }
    p[0] = (uint8_t)h;
    p[1] = (uint8_t)(s | ((c >> 2) & 0xc0u));
    p[2] = (uint8_t)c;
}

static void write_mbr_entry(uint8_t *e, uint8_t active, uint8_t type, uint32_t start, uint32_t count) {
    memset(e, 0, 16);
    e[0] = active;
    fill_mbr_chs(e + 1, start);
    e[4] = type;
    fill_mbr_chs(e + 5, count ? start + count - 1u : start);
    memcpy(e + 8, &start, 4);
    memcpy(e + 12, &count, 4);
}

static int classify_fat_vbr(const uint8_t *vbr, bool *is_fat32) {
    if (vbr[510] != 0x55 || vbr[511] != 0xaa) return 0;
    if (vbr[0] != 0xeb && vbr[0] != 0xe9) return 0;
    uint16_t bps = (uint16_t)(vbr[11] | (vbr[12] << 8));
    uint8_t spc = vbr[13];
    if (bps != 512 || !spc || (spc & (spc - 1))) return 0;
    if (!memcmp(vbr + 82, "FAT32   ", 8)) { *is_fat32 = true; return 0x0c; }
    if (!memcmp(vbr + 54, "FAT16   ", 8)) { *is_fat32 = false; return 0x0e; }
    if (!memcmp(vbr + 54, "FAT12   ", 8)) { *is_fat32 = false; return 0x01; }
    return 0;
}

static bool is_dos_fat_type(uint8_t t) {
    return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0b || t == 0x0c || t == 0x0e;
}

static EFI_STATUS command(hostblk_t *h, bool write, uint64_t lba, uint32_t count, void *buf);

// Inspect sector 0 of a USB disk so superfloppy, GPT or EFI-typed FAT volumes
// appear to DOS and Windows 98 as standard MBR FAT partitions.
static void inspect_hd_layout(hostblk_t *h) {
    h->lba_offset = 0;
    h->has_synth_mbr = false;
    memset(h->vbr_lba, 0, sizeof(h->vbr_lba));
    memset(h->synth_mbr, 0, sizeof(h->synth_mbr));
    if (h->kind != HOST_HD || !h->present || h->phys_blocks < 64 || h->bs != 512) return;

    static uint8_t s0[512] __attribute__((aligned(4096)));
    static uint8_t s1[512] __attribute__((aligned(4096)));
    static uint8_t vbr[512] __attribute__((aligned(4096)));
    if (EFI_ERROR(command(h, false, 0, 1, s0))) return;

    // Case 1: Superfloppy (LBA 0 is directly a FAT12/FAT16/FAT32 VBR without an MBR)
    bool f32 = false;
    int fat_type = classify_fat_vbr(s0, &f32);
    if (fat_type) {
        uint32_t count = h->phys_blocks > (LBA28_MAX - 63u) ? (uint32_t)(LBA28_MAX - 63u) : (uint32_t)h->phys_blocks;
        h->lba_offset = 63;
        h->blocks = (uint64_t)count + 63u;
        h->has_synth_mbr = true;
        uint32_t disk_id = 0x55534231u;
        memcpy(h->synth_mbr + 440, &disk_id, 4);
        write_mbr_entry(h->synth_mbr + 446, 0x80, (uint8_t)fat_type, 63, count);
        h->synth_mbr[510] = 0x55;
        h->synth_mbr[511] = 0xaa;
        h->vbr_lba[0] = 63;
        printf("%s: superfloppy FAT volume wrapped with synthetic MBR at +63 (%u blocks)\n", h->name, count);
        return;
    }

    if (s0[510] != 0x55 || s0[511] != 0xaa) return;

    // Case 2: GPT protective MBR (type 0xEE) -> expose up to 4 GPT FAT partitions in a synthetic MBR
    if (s0[450] == 0xee && !EFI_ERROR(command(h, false, 1, 1, s1)) &&
        !memcmp(s1, "EFI PART", 8)) {
        uint64_t table_lba = *(uint64_t *)(s1 + 72);
        uint32_t num = *(uint32_t *)(s1 + 80), esz = *(uint32_t *)(s1 + 84);
        if (num > 128) num = 128;
        if (esz == 128 && table_lba >= 2 && table_lba < h->phys_blocks &&
            h->phys_blocks - table_lba > 32) {
            uint32_t disk_id = 0x55534247u;
            memcpy(h->synth_mbr + 440, &disk_id, 4);
            h->synth_mbr[510] = 0x55;
            h->synth_mbr[511] = 0xaa;
            int found = 0;
            for (uint32_t i = 0; i < num && found < 4; i++) {
                uint64_t elba = table_lba + (i * 128u) / 512u;
                uint32_t eoff = (i * 128u) % 512u;
                if (eoff == 0 && EFI_ERROR(command(h, false, elba, 1, s1)))
                    break;
                uint8_t *ge = s1 + eoff;
                uint64_t first = *(uint64_t *)(ge + 32), last = *(uint64_t *)(ge + 40);
                if (!first || last < first || first >= h->blocks) continue;
                if (last >= h->blocks) last = h->blocks - 1;
                if (EFI_ERROR(command(h, false, first, 1, vbr))) continue;
                bool pf32 = false;
                int ptype = classify_fat_vbr(vbr, &pf32);
                if (!ptype) continue;
                write_mbr_entry(h->synth_mbr + 446 + found * 16, found == 0 ? 0x80 : 0x00,
                                (uint8_t)ptype, (uint32_t)first, (uint32_t)(last - first + 1));
                h->vbr_lba[found] = (uint32_t)first;
                found++;
            }
            if (found > 0) {
                h->has_synth_mbr = true;
                printf("%s: GPT disk with %d FAT partition(s) exposed via synthetic MBR\n", h->name, found);
                return;
            }
        }
    }

    // Case 3: Standard MBR -> ensure any FAT partition with non-DOS type (e.g. 0xEF) is visible
    memcpy(h->synth_mbr, s0, 512);
    for (int p = 0; p < 4; p++) {
        uint8_t *e = h->synth_mbr + 446 + p * 16;
        uint8_t type = e[4];
        uint32_t start = 0, count = 0;
        memcpy(&start, e + 8, 4);
        memcpy(&count, e + 12, 4);
        if (!type || !start || !count || start >= h->blocks) continue;
        if (EFI_ERROR(command(h, false, start, 1, s1))) continue;
        bool pf32 = false;
        int ptype = classify_fat_vbr(s1, &pf32);
        if (!ptype) continue;
        if (!is_dos_fat_type(type)) {
            e[4] = (uint8_t)ptype;
            h->has_synth_mbr = true;
            printf("%s: partition %d FAT volume type %02x -> %02x for DOS/Win9x\n", h->name, p + 1, type, ptype);
        }
        uint32_t hidden = 0;
        memcpy(&hidden, s1 + 28, 4);
        if (hidden != start) {
            h->vbr_lba[p] = start;
        }
    }
}

static void forget(hostblk_t *h) {
    h->cached = UINT64_MAX;
    h->cached_count = 0;
    h->dirty = false;
    memset(h->valid, 0, sizeof(h->valid));
}

static bool overlay_readonly(const hostblk_t *h) {
    if (h->kind != HOST_HD) return false;
    if (h->has_synth_mbr || h->lba_offset) return true;
    for (int p = 0; p < 4; p++)
        if (h->vbr_lba[p]) return true;
    return false;
}

static bool media_failed(hostblk_t *h) {
    forget(h);
    h->failed = true;
    h->present = false;
    h->phys_blocks = 0;
    h->blocks = 0;
    h->announce = true;
    if (h->dev) h->dev->readonly = true;
    return false;
}

// Match the original protocol, not an ordinal that can move after unplug.
// Once missing, this binding stays failed even if a protocol address is reused.
static bool ensure_bio(hostblk_t *h) {
    if (h->failed || !h->bio) return false;
    found_t f[16];
    int n = scan(f, 16);
    for (int i = 0; i < n; i++)
        if (f[i].bio == h->bio) return true;
    return media_failed(h);
}

// Do not let either cached reads or dirty writes migrate to a new medium.
// Changed/empty/unsupported media require an explicit lifecycle-level rebind.
static bool refresh(hostblk_t *h) {
    if (!ensure_bio(h)) return false;
    EFI_BLOCK_IO_MEDIA *m = h->bio->Media;
    if (!m || !m->MediaPresent || m->MediaId != h->media_id ||
        m->BlockSize != h->bs || (h->bs != 512 && h->bs != 2048) ||
        (h->kind != HOST_CD && h->bs != 512) || m->LastBlock == UINT64_MAX ||
        m->LastBlock + 1 > UINT64_MAX / h->bs ||
        medium_blocks(h, m->LastBlock + 1) != h->phys_blocks)
        return media_failed(h);
    if (h->dev) h->dev->readonly = h->cd || m->ReadOnly || overlay_readonly(h);
    return true;
}

// Use the saved identity for exactly one firmware call; never retry a write
// (or a read whose geometry/overlay could belong to the previous medium).
static EFI_STATUS command(hostblk_t *h, bool write, uint64_t lba, uint32_t count, void *buf) {
    if (!refresh(h)) return EFI_NO_MEDIA;
    if (!count || lba >= h->phys_blocks || count > h->phys_blocks - lba)
        return EFI_INVALID_PARAMETER;
    if (write && (h->cd || h->bio->Media->ReadOnly || overlay_readonly(h)))
        return EFI_WRITE_PROTECTED;
    wyse_vga_flush_before_io();
    EFI_STATUS st = write ? h->bio->WriteBlocks(h->bio, h->media_id, lba, (uint64_t)count * h->bs, buf)
                          : h->bio->ReadBlocks(h->bio, h->media_id, lba, (uint64_t)count * h->bs, buf);
    if (st == EFI_MEDIA_CHANGED || st == EFI_NO_MEDIA) media_failed(h);
    if (!refresh(h) && !EFI_ERROR(st)) st = EFI_MEDIA_CHANGED;
    if (EFI_ERROR(st) && moans++ < 8)
        printf("%s: %s of %u at %llu: %llx\n", h->name, write ? "write" : "read", count, (unsigned long long)lba,
               (unsigned long long)st);
    return st;
}

static bool flush(hostblk_t *h) {
    if (!refresh(h)) return false;
    if (!h->dirty) return true;
    uint64_t first = h->cached;
    uint32_t count = h->cached_count;
    if (EFI_ERROR(command(h, true, first, count, h->buf))) return false;
    h->dirty = false;
    return true;
}

static void pass_on(hostblk_t *h) {
    if (!h->announce || !h->blocks) return;
    h->announce = false;
    printf("%s: medium of %llu blocks\n", h->name, (unsigned long long)h->blocks);
    blk_host_media_changed(h->dev);
}

static bool fd_run_valid(const hostblk_t *h, uint64_t first, uint32_t count) {
    if (h->kind != HOST_FD || !h->disk_cache || h->bs != 512 || first > FD_MAX_BLOCKS || count > FD_MAX_BLOCKS - first) return false;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t b = first + i;
        if (!(h->valid[b >> 6] & (1ull << (b & 63)))) return false;
    }
    return true;
}

static void fd_mark_valid(hostblk_t *h, uint64_t first, uint32_t count) {
    if (h->kind != HOST_FD || !h->disk_cache || h->bs != 512 || first > FD_MAX_BLOCKS || count > FD_MAX_BLOCKS - first) return;
    memcpy(h->disk_cache + first * 512u, h->buf, (size_t)count * 512u);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t b = first + i;
        h->valid[b >> 6] |= 1ull << (b & 63);
    }
}

static void apply_hd_overlays(hostblk_t *h, uint64_t first, uint32_t count) {
    if (h->kind != HOST_HD || h->bs != 512) return;
    if (h->has_synth_mbr && first == 0 && count > 0)
        memcpy(h->buf, h->synth_mbr, 512);
    for (int p = 0; p < 4; p++) {
        uint32_t vlba = h->vbr_lba[p];
        if (!vlba) continue;
        // Only the primary VBR: BPB_BkBootSec is not necessarily six.
        if (vlba >= first && vlba - first < count) {
            uint8_t *sec = h->buf + (size_t)(vlba - first) * 512u;
            uint16_t spt = 63, heads = 255;
            memcpy(sec + 24, &spt, 2);
            memcpy(sec + 26, &heads, 2);
            memcpy(sec + 28, &vlba, 4);
        }
    }
}

static bool load(hostblk_t *h, uint64_t block) {
    if (!refresh(h) || block >= h->blocks) return false;
    uint64_t now = wyse_ticks_us();
    uint64_t idle_limit = h->cd ? 500000ull : 5000000ull;
    if (now - h->last_use > idle_limit && h->cached != UINT64_MAX) {
        if (!flush(h)) return false;
        forget(h);
    }
    h->last_use = now;
    if (h->cached != UINT64_MAX && block >= h->cached && block < h->cached + h->cached_count) return true;
    if (!flush(h)) return false;
    uint64_t first = block - block % h->run;
    uint32_t count = h->run;
    if (count > h->blocks - first) count = (uint32_t)(h->blocks - first);
    if (fd_run_valid(h, first, count)) {
        memcpy(h->buf, h->disk_cache + first * 512u, (size_t)count * 512u);
        h->cached = first;
        h->cached_count = count;
        return true;
    }
    if (h->kind == HOST_HD && h->lba_offset > 0) {
        memset(h->buf, 0, (size_t)count * 512u);
        if (first + count > h->lba_offset) {
            uint32_t skip = first < h->lba_offset ? h->lba_offset - (uint32_t)first : 0;
            uint64_t pfirst = first < h->lba_offset ? 0 : first - h->lba_offset;
            uint32_t pcount = count - skip;
            if (EFI_ERROR(command(h, false, pfirst, pcount, h->buf + skip * 512u))) {
                forget(h);
                return false;
            }
        }
    } else {
        if (EFI_ERROR(command(h, false, first, count, h->buf))) {
            forget(h);
            return false;
        }
    }
    apply_hd_overlays(h, first, count);
    fd_mark_valid(h, first, count);
    h->cached = first;
    h->cached_count = count;
    pass_on(h);
    return true;
}

//--------------------------------------------------------------------+
// BlockDev
//--------------------------------------------------------------------+

static int host_seek(BlockDev *d, uint64_t off) {
    hostblk_t *h = d->ctx;
    if (!refresh(h) || !h->blocks || h->blocks > UINT64_MAX / h->bs ||
        off > h->blocks * h->bs) return -1;
    d->pos = off;
    return 0;
}

static bool host_range(BlockDev *d, unsigned len) {
    hostblk_t *h = d->ctx;
    if (!refresh(h) || !h->blocks || h->blocks > UINT64_MAX / h->bs ||
        len > 0x7fffffffu) return false;
    uint64_t bytes = h->blocks * h->bs;
    return d->pos <= bytes && len <= bytes - d->pos;
}

static int host_read(BlockDev *d, void *buf, unsigned len) {
    hostblk_t *h = d->ctx;
    uint8_t *out = buf;
    unsigned done = 0;
    if (!host_range(d, len)) return -1;
    while (done < len) {
        uint64_t block = d->pos / h->bs;
        if (h->blocks && block >= h->blocks) break;
        if (!load(h, block)) return done ? (int)done : -1;
        unsigned within = (unsigned)(d->pos - h->cached * h->bs);
        unsigned take = h->cached_count * h->bs - within;
        if (take > len - done) take = len - done;
        memcpy(out + done, h->buf + within, take);
        d->pos += take;
        done += take;
    }
    return (int)done;
}

static int host_write(BlockDev *d, const void *buf, unsigned len) {
    hostblk_t *h = d->ctx;
    const uint8_t *in = buf;
    unsigned done = 0;
    if (!host_range(d, len) || d->readonly || overlay_readonly(h)) return -1;
    while (done < len) {
        uint64_t block = d->pos / h->bs;
        if (h->blocks && block >= h->blocks) break;
        if (!load(h, block)) return done ? (int)done : -1;
        unsigned within = (unsigned)(d->pos - h->cached * h->bs);
        unsigned take = h->cached_count * h->bs - within;
        if (take > len - done) take = len - done;
        memcpy(h->buf + within, in + done, take);
        if (h->kind == HOST_FD && h->disk_cache && h->bs == 512 && h->cached + h->cached_count <= FD_MAX_BLOCKS)
            memcpy(h->disk_cache + h->cached * 512u + within, in + done, take);
        h->dirty = true;
        d->pos += take;
        done += take;
    }
    return (int)done;
}

static uint64_t host_size(BlockDev *d) {
    hostblk_t *h = d->ctx;
    return refresh(h) && h->blocks <= UINT64_MAX / h->bs ? h->blocks * h->bs : 0;
}

static int host_sync(BlockDev *d) {
    hostblk_t *h = d->ctx;
    if (!flush(h) || !refresh(h) || !h->bio->FlushBlocks) return -1;
    wyse_vga_flush_before_io();
    EFI_STATUS st = h->bio->FlushBlocks(h->bio);
    if (st == EFI_MEDIA_CHANGED || st == EFI_NO_MEDIA) media_failed(h);
    return refresh(h) && !EFI_ERROR(st) ? 0 : -1;
}

int blk_bind_host(BlockDev *d, const char *which) {
    host_kind_t kind = HOST_FD;
    EFI_BLOCK_IO_PROTOCOL *bio = lookup(which, &kind);
    if (!d || !bio || !bio->Media) return 0;
    EFI_BLOCK_IO_MEDIA *m = bio->Media;
    if ((m->BlockSize != 512 && m->BlockSize != 2048) ||
        (kind != HOST_CD && m->BlockSize != 512) ||
        (m->MediaPresent && (m->LastBlock == UINT64_MAX ||
                            m->LastBlock + 1 > UINT64_MAX / m->BlockSize))) return 0;
    hostblk_t *h = NULL;
    for (int i = 0; i < ndrives; i++) {
        hostblk_t *cur = &drives[i];
        bool active = cur->dev && cur->dev->ctx == cur;
        if (active && (cur->bio == bio || cur->dev == d)) return 0;
        if (!active && !h) h = cur;
    }
    if (!h) {
        if (ndrives >= (int)(sizeof(drives) / sizeof(drives[0]))) return 0;
        h = &drives[ndrives++];
    }
    uint8_t *saved_cache = h->disk_cache;
    memset(h, 0, offsetof(hostblk_t, buf));
    if (kind == HOST_FD && !saved_cache) saved_cache = malloc(FD_MAX_BLOCKS * 512u);
    h->disk_cache = saved_cache;
    snprintf(h->name, sizeof(h->name), "%s", which);
    h->bio = bio;
    h->kind = kind;
    h->cd = (kind == HOST_CD);
    h->bs = h->cd ? 2048 : 512;
    h->run = h->cd ? RUN_BYTES / 2048 : (kind == HOST_HD ? RUN_BYTES / 512 : 8);
    h->media_id = bio->Media->MediaId;
    h->present = bio->Media->MediaPresent;
    if (h->present && bio->Media->BlockSize) h->bs = bio->Media->BlockSize;
    h->phys_blocks = h->present ? medium_blocks(h, bio->Media->LastBlock + 1) : 0;
    h->blocks = h->phys_blocks;
    if (h->present && kind == HOST_HD)
        inspect_hd_layout(h);
    if (h->failed) return 0;
    h->dev = d;
    forget(h);
    memset(d, 0, sizeof(*d));
    d->ctx = h;
    d->read = host_read;
    d->write = host_write;
    d->seek = host_seek;
    d->size = host_size;
    d->sync = host_sync;
    d->readonly = h->cd || bio->Media->ReadOnly || overlay_readonly(h);
    printf("%s: bound, %s, %u x %llu (%llu MB)\n", which, h->present ? "medium" : "empty", h->bs,
           (unsigned long long)h->blocks, (unsigned long long)((h->blocks * h->bs) >> 20));
    return 1;
}

int blk_host_available(const char *which) {
    host_kind_t kind;
    return lookup(which, &kind) != NULL;
}

uint64_t blk_host_size_mb(const char *which) {
    host_kind_t kind;
    EFI_BLOCK_IO_PROTOCOL *bio = lookup(which, &kind);
    if (!bio || !bio->Media || !bio->Media->MediaPresent ||
        (bio->Media->BlockSize != 512 && bio->Media->BlockSize != 2048) ||
        bio->Media->LastBlock == UINT64_MAX) return 0;
    uint64_t blocks = bio->Media->LastBlock + 1;
    if (blocks > UINT64_MAX / bio->Media->BlockSize) return 0;
    if (kind == HOST_HD && blocks > 1 && (blocks & 1)) blocks--;
    return (blocks * bio->Media->BlockSize) >> 20;
}

static void usb_poll(void) {
    if (!usb_change) return;
    for (int port = 0; port < 2; port++) {
        BlockDev *dev = &usb_devs[port];
        if (dev->ctx && !blk_size(dev)) {
            usb_change(port, NULL, 0);
            memset(dev, 0, sizeof(*dev));
            return;
        }
    }
    found_t found[16];
    int count = scan(found, 16), ordinal = 0;
    for (int port = 0; port < 2; port++) {
        bool present = false;
        for (int index = 0; index < count; index++)
            if (found[index].bio == usb_blocked[port]) present = true;
        if (!present) usb_blocked[port] = NULL;
    }
    for (int index = 0; index < count; index++) {
        if (found[index].kind != HOST_HD) continue;
        ordinal++;
        if (!found[index].bio->Media->MediaPresent) continue;
        if (found[index].bio == usb_blocked[0] || found[index].bio == usb_blocked[1]) continue;
        bool owned = false;
        for (int drive = 0; drive < ndrives; drive++) {
            hostblk_t *host = &drives[drive];
            if (host->bio == found[index].bio && host->dev && host->dev->ctx == host)
                owned = true;
        }
        if (owned) continue;
        for (int port = 0; port < 2; port++) {
            if (usb_devs[port].ctx) continue;
            if (wyse_ticks_us()<usb_not_before[port]) continue;
            char which[8];
            snprintf(which, sizeof(which), "uhd%d", ordinal);
            if (blk_bind_host(&usb_devs[port], which))
                /* Identity belongs to the virtual reader, not an insertion. */
                usb_change(port, &usb_devs[port], (unsigned)port + 1);
            break;
        }
    }
}

void hostblk_usb_start(void (*change)(int port, BlockDev *dev, unsigned serial)) {
    usb_change = change;
    usb_poll();
}

const char *hostblk_usb_name(int port) {
    if (port < 0 || port >= 2 || !usb_devs[port].ctx) return NULL;
    return ((hostblk_t *)usb_devs[port].ctx)->name;
}

int hostblk_usb_eject(int port) {
    if (!usb_change || port < 0 || port >= 2 || !usb_devs[port].ctx) return 0;
    BlockDev *dev = &usb_devs[port];
    if (blk_size(dev) && blk_sync(dev) < 0) return 0;
    usb_blocked[port] = ((hostblk_t *)dev->ctx)->bio;
    usb_change(port, NULL, 0);
    memset(dev, 0, sizeof(*dev));
    return 1;
}

int hostblk_usb_insert(int port, const char *which) {
    if (!usb_change || port < 0 || port >= 2 || usb_devs[port].ctx ||
        !which || strncmp(which, "uhd", 3)) return 0;
    if (!blk_bind_host(&usb_devs[port], which)) return 0;
    EFI_BLOCK_IO_PROTOCOL *bio = ((hostblk_t *)usb_devs[port].ctx)->bio;
    for (int index = 0; index < 2; index++)
        if (usb_blocked[index] == bio) usb_blocked[index] = NULL;
    usb_change(port, &usb_devs[port], (unsigned)port + 1);
    return 1;
}
int hostblk_image_info(const char *which,void **identity,uint32_t *media_id) {
    host_kind_t kind;
    EFI_BLOCK_IO_PROTOCOL *bio=lookup(which,&kind);
    if (!bio || kind!=HOST_HD || !bio->Media || !bio->Media->MediaPresent) return 0;
    *identity=bio; *media_id=bio->Media->MediaId; return 1;
}
int hostblk_image_take(const char *which,void *identity,uint32_t media_id,BlockDev *target,int *return_port) {
    host_kind_t kind;
    EFI_BLOCK_IO_PROTOCOL *bio=lookup(which,&kind);
    if (!bio || kind!=HOST_HD || bio!=identity || !bio->Media ||
        bio->Media->MediaId!=media_id || !target || target->ctx) return 0;
    *return_port=-1;
    for (int port=0;port<2;port++) {
        BlockDev *dev=&usb_devs[port];
        if (!dev->ctx || ((hostblk_t *)dev->ctx)->bio!=bio) continue;
        if (!blk_size(dev) || blk_sync(dev)<0) return 0;
        /* Abort pending USB packets before moving the pinned backend. */
        usb_change(port,NULL,0);
        *target=*dev;
        ((hostblk_t *)target->ctx)->dev=target;
        memset(dev,0,sizeof(*dev));
        *return_port=port;
        printf("usb images: %s owned by monitor\n",which);
        return 1;
    }
    return blk_bind_host(target,which);
}
int hostblk_image_return(BlockDev *target,int return_port) {
    if (!target || !target->ctx) return 1;
    hostblk_t *h=target->ctx;
    /* A failed/removed binding is discarded, never flushed into replacement. */
    if (blk_size(target) && blk_sync(target)<0) return 0;
    EFI_BLOCK_IO_PROTOCOL *bio=h->bio;
    h->dev=NULL;
    memset(target,0,sizeof(*target));
    for (int port=0;port<2;port++) {
        if (usb_blocked[port]==bio) usb_blocked[port]=NULL;
        /* Let the guest observe disconnection before automatic re-enumeration. */
        usb_not_before[port]=wyse_ticks_us()+500000;
    }
    printf("usb images: medium released to system\n");
    return 1;
}
// Media replacement/rebinding is deliberately owned by the disk lifecycle.
