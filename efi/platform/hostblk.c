// Real USB floppy and CD-ROM drives behind the guest's drives.
//
// The firmware's USB mass storage driver gives each drive a Block I/O
// protocol; here they become the "ufd1", "ucd1"... that X86Pi's disk layer
// binds with blk_bind_host() (config "usb:ufd1", the disk menu, and drive A:
// and the secondary master by default when one is plugged in).
//
// Which is which: a UFI interface (USB subclass 4) is a floppy, 2048-byte
// blocks or an ATAPI interface (subclasses 2 and 5) a CD; any other USB disk
// is a stick and left alone, unless it holds a floppy-sized medium.
//
// The controllers above move bytes; the drives want whole blocks, and every
// command to a floppy waits a revolution (about 200 ms) whatever its length.
// So a run of blocks is read at once and served from (8 sectors on a floppy,
// as X86Pi found that some drives stall on a whole track; 16 on a CD), and a
// write changes the held run and goes out when the next run displaces it or
// the guest's operation ends (blk_sync).
//
// A changed medium is noticed by the firmware on the next command, which then
// fails with EFI_MEDIA_CHANGED or carries a new MediaId: what was held is
// dropped and the disk layer is told (blk_host_media_changed) once the new
// medium's size is known.

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include "blockdev.h"
#include "bio.h"

extern EFI_BOOT_SERVICES *gBS;
uint64_t wyse_ticks_us(void);
void wyse_vga_flush_before_io(void);

static const EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static const EFI_GUID usbio_guid = EFI_USB_IO_PROTOCOL_GUID;

#define RUN_BYTES     32768u
#define FD_MAX_BLOCKS 5760u

typedef struct {
    char name[8];
    EFI_BLOCK_IO_PROTOCOL *bio;
    bool cd;
    uint32_t bs;                // block size of the medium (512 floppy, 2048 CD)
    uint32_t run;               // blocks read at once
    uint32_t media_id;
    bool present;
    uint64_t blocks;            // medium size in blocks, 0 when there is none (or not known yet)
    uint64_t cached;            // first block held, or ~0
    uint32_t cached_count;
    bool dirty;
    bool announce;              // a change seen, not yet passed on
    uint64_t last_probe;
    uint64_t last_use;
    BlockDev *dev;
    uint8_t *disk_cache;        // floppy sector cache (FD_MAX_BLOCKS * 512), preserved across rebinds
    uint64_t valid[FD_MAX_BLOCKS / 64];
    uint8_t buf[RUN_BYTES] __attribute__((aligned(4096)));
} hostblk_t;

static hostblk_t drives[4];
static int ndrives;
static int moans;

//--------------------------------------------------------------------+
// finding the drives
//--------------------------------------------------------------------+

typedef struct {
    EFI_BLOCK_IO_PROTOCOL *bio;
    bool cd;
} found_t;

// The USB floppies and CD drives the firmware has, in its handle order
static int scan(found_t *out, int cap) {
    uint64_t n;
    EFI_HANDLE *hs;
    int count = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &hs))) return 0;
    for (uint64_t i = 0; i < n && count < cap; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &bio_guid, (void **)&bio))) continue;
        EFI_BLOCK_IO_MEDIA *m = bio->Media;
        if (m->LogicalPartition || !bio_path_has(bio_device_path(hs[i]), DP_MESSAGING, DP_MSG_USB)) continue;
        int subclass = -1;
        EFI_USB_IO_PROTOCOL *io;
        USB_INTERFACE_DESCRIPTOR ifd;
        if (!EFI_ERROR(gBS->HandleProtocol(hs[i], &usbio_guid, (void **)&io)) &&
            !EFI_ERROR(io->UsbGetInterfaceDescriptor(io, &ifd)) && ifd.InterfaceClass == 8)
            subclass = ifd.InterfaceSubClass;
        bool cd = m->BlockSize == 2048 || subclass == 2 || subclass == 5;
        bool floppy = !cd && (subclass == 4 || (m->MediaPresent && m->BlockSize == 512 && m->LastBlock < 5760));
        if (!cd && !floppy) continue;
        out[count].bio = bio;
        out[count].cd = cd;
        count++;
    }
    gBS->FreePool(hs);
    return count;
}

// The drive the host knows as `which` ("ufd1", "ucd2"), or NULL
static EFI_BLOCK_IO_PROTOCOL *lookup(const char *which, bool *cd) {
    if (!which || (strncmp(which, "ufd", 3) && strncmp(which, "ucd", 3))) return NULL;
    bool want_cd = which[1] == 'c';
    int nth = which[3] ? which[3] - '0' : 1;
    found_t f[8];
    int n = scan(f, 8);
    for (int i = 0; i < n; i++) {
        if (f[i].cd != want_cd || --nth) continue;
        *cd = want_cd;
        return f[i].bio;
    }
    return NULL;
}

void hostblk_list(void) {
    found_t f[8];
    int n = scan(f, 8), fd = 0, cd = 0;
    for (int i = 0; i < n; i++) {
        EFI_BLOCK_IO_MEDIA *m = f[i].bio->Media;
        printf("usb drive: %s%d, %s, %u x %llu\n", f[i].cd ? "ucd" : "ufd", f[i].cd ? ++cd : ++fd,
               m->MediaPresent ? "medium" : "empty", m->BlockSize, (unsigned long long)(m->LastBlock + 1));
    }
    if (!n) printf("usb drive: none\n");
}

//--------------------------------------------------------------------+
// the medium
//--------------------------------------------------------------------+

// The Wyse's firmware gives a USB floppy's LastBlock as the number of blocks
// (2881 for a 1.44 MB diskette), and the disk layer finds the geometry by the
// exact size: a count one past a diskette size is taken as that size.
static uint64_t medium_blocks(const hostblk_t *h, uint64_t n) {
    static const uint16_t sizes[] = {320, 360, 640, 720, 1280, 1440, 2400, 2880, 3040, 3200, 3360, 5760};
    if (h->cd) return n;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if (n == sizes[i]) return n;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if (n == sizes[i] + 1u) return n - 1;
    return n;
}

static void forget(hostblk_t *h) {
    h->cached = UINT64_MAX;
    h->cached_count = 0;
    h->dirty = false;
    memset(h->valid, 0, sizeof(h->valid));
}

// Take in what the firmware says about the medium now; a different one drops what is held
static void refresh(hostblk_t *h) {
    EFI_BLOCK_IO_MEDIA *m = h->bio->Media;
    bool present = m->MediaPresent;
    if (present != h->present || m->MediaId != h->media_id) {
        forget(h);
        h->announce = true;
        h->present = present;
        h->media_id = m->MediaId;
    }
    if (present && m->BlockSize) h->bs = m->BlockSize;
    h->blocks = present ? medium_blocks(h, m->LastBlock + 1) : 0;
}

// One command to the drive, noticing a medium that changed meanwhile
static EFI_STATUS command(hostblk_t *h, bool write, uint64_t lba, uint32_t count, void *buf) {
    wyse_vga_flush_before_io();
    EFI_STATUS st = EFI_MEDIA_CHANGED;
    for (int attempt = 0; attempt < 2 && st == EFI_MEDIA_CHANGED; attempt++) {
        st = write ? h->bio->WriteBlocks(h->bio, h->bio->Media->MediaId, lba, (uint64_t)count * h->bs, buf)
                   : h->bio->ReadBlocks(h->bio, h->bio->Media->MediaId, lba, (uint64_t)count * h->bs, buf);
        refresh(h);
    }
    if (EFI_ERROR(st) && moans++ < 8)
        printf("%s: %s of %u at %llu: %llx\n", h->name, write ? "write" : "read", count, (unsigned long long)lba,
               (unsigned long long)st);
    return st;
}

// An empty drive is looked into now and then: the firmware learns of a
// medium only when it is asked for one
static void probe(hostblk_t *h) {
    uint64_t now = wyse_ticks_us();
    if (h->present || now - h->last_probe < 1000000) return;
    h->last_probe = now;
    command(h, false, 0, 1, h->buf);
    forget(h);
}

static bool flush(hostblk_t *h) {
    if (!h->dirty) return true;
    if (EFI_ERROR(command(h, true, h->cached, h->cached_count, h->buf))) return false;
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
    if (h->cd || !h->disk_cache || h->bs != 512 || first + count > FD_MAX_BLOCKS) return false;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t b = first + i;
        if (!(h->valid[b >> 6] & (1ull << (b & 63)))) return false;
    }
    return true;
}

static void fd_mark_valid(hostblk_t *h, uint64_t first, uint32_t count) {
    if (h->cd || !h->disk_cache || h->bs != 512 || first + count > FD_MAX_BLOCKS) return;
    memcpy(h->disk_cache + first * 512u, h->buf, (size_t)count * 512u);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t b = first + i;
        h->valid[b >> 6] |= 1ull << (b & 63);
    }
}

static bool load(hostblk_t *h, uint64_t block) {
    // What is held may belong to a diskette that has since been swapped, and
    // the firmware only finds out when the drive is asked. A physical diskette
    // swap while the drive is idle takes several seconds; during active boot
    // and driver loading, keeping previously read tracks (FAT, directory) in
    // disk_cache avoids thrashing the physical USB floppy head back to track 0.
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
    if (h->blocks && first + count > h->blocks) count = (uint32_t)(h->blocks - first);
    if (fd_run_valid(h, first, count)) {
        memcpy(h->buf, h->disk_cache + first * 512u, (size_t)count * 512u);
        h->cached = first;
        h->cached_count = count;
        return true;
    }
    if (EFI_ERROR(command(h, false, first, count, h->buf))) {
        forget(h);
        return false;
    }
    // the read may have been what found the medium
    if (h->announce) {
        refresh(h);
        if (first + count > h->blocks && h->blocks) count = (uint32_t)(h->blocks - first);
    }
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
    if (!h->present) probe(h);
    if (h->blocks && off > h->blocks * h->bs) return -1;
    d->pos = off;
    return 0;
}

static int host_read(BlockDev *d, void *buf, unsigned len) {
    hostblk_t *h = d->ctx;
    uint8_t *out = buf;
    unsigned done = 0;
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
    if (h->cd) return -1;
    while (done < len) {
        uint64_t block = d->pos / h->bs;
        if (h->blocks && block >= h->blocks) break;
        if (!load(h, block)) return done ? (int)done : -1;
        unsigned within = (unsigned)(d->pos - h->cached * h->bs);
        unsigned take = h->cached_count * h->bs - within;
        if (take > len - done) take = len - done;
        memcpy(h->buf + within, in + done, take);
        if (h->disk_cache && h->bs == 512 && h->cached + h->cached_count <= FD_MAX_BLOCKS)
            memcpy(h->disk_cache + h->cached * 512u + within, in + done, take);
        h->dirty = true;
        d->pos += take;
        done += take;
    }
    return (int)done;
}

static uint64_t host_size(BlockDev *d) {
    hostblk_t *h = d->ctx;
    // the ATAPI emulation asks for the size before it reads anything, and an
    // unknown size reads as "no disc": so an empty CD drive is looked into here
    if (!h->present) {
        probe(h);
        pass_on(h);
    }
    return h->blocks * h->bs;
}

static int host_sync(BlockDev *d) {
    hostblk_t *h = d->ctx;
    return flush(h) ? 0 : -1;
}

int blk_bind_host(BlockDev *d, const char *which) {
    bool cd = false;
    EFI_BLOCK_IO_PROTOCOL *bio = lookup(which, &cd);
    if (!d || !bio) return 0;
    hostblk_t *h = NULL;
    for (int i = 0; i < ndrives; i++)
        if (drives[i].bio == bio) h = &drives[i];
    if (!h) {
        if (ndrives >= (int)(sizeof(drives) / sizeof(drives[0]))) return 0;
        h = &drives[ndrives++];
    }
    uint8_t *saved_cache = h->disk_cache;
    memset(h, 0, offsetof(hostblk_t, buf));
    if (!cd && !saved_cache) saved_cache = malloc(FD_MAX_BLOCKS * 512u);
    h->disk_cache = saved_cache;
    snprintf(h->name, sizeof(h->name), "%s", which);
    h->bio = bio;
    h->cd = cd;
    h->bs = cd ? 2048 : 512;
    h->run = cd ? RUN_BYTES / 2048 : 8;
    h->media_id = bio->Media->MediaId;
    h->present = bio->Media->MediaPresent;
    if (h->present && bio->Media->BlockSize) h->bs = bio->Media->BlockSize;
    h->blocks = h->present ? medium_blocks(h, bio->Media->LastBlock + 1) : 0;
    h->dev = d;
    forget(h);
    memset(d, 0, sizeof(*d));
    d->ctx = h;
    d->read = host_read;
    d->write = host_write;
    d->seek = host_seek;
    d->size = host_size;
    d->sync = host_sync;
    d->readonly = cd || bio->Media->ReadOnly;
    printf("%s: bound, %s, %u x %llu\n", which, h->present ? "medium" : "empty", h->bs,
           (unsigned long long)h->blocks);
    return 1;
}

int blk_host_available(const char *which) {
    bool cd;
    return lookup(which, &cd) != NULL;
}
