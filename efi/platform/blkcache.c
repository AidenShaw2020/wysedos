// A block cache in front of the firmware's eMMC driver.
//
// Every Block I/O call to the Wyse's eMMC costs 1.3 to 2.5 ms whatever its
// size (measured: one block 1.35 ms, 64 blocks 2.9 ms, 2048 blocks 15 ms), so
// DOS reading a sector at a time would wait more than it reads. Here reads
// fetch whole 64 KB chunks and keep them; writes land in the chunk and go out
// on the next flush (FatFs's sync, which disk.c asks for after each guest
// write, and a timer from the main loop), only the span that changed.
// Transfers of a whole chunk or more bypass the cache, which keeps copying
// big files at the device's full speed.

#include <string.h>
#include <stdio.h>
#include "bio.h"
#include "blkcache.h"

extern EFI_BOOT_SERVICES *gBS;

#define CHUNK_BLOCKS 128u           // 64 KB of 512-byte blocks
#define CHUNK_BYTES (CHUNK_BLOCKS * 512u)
#define SLOTS 512u                  // 32 MB
#define NO_CHUNK UINT64_MAX

typedef struct {
    EFI_BLOCK_IO_PROTOCOL *bio;
    uint64_t chunk;                 // chunk number on that device
    uint64_t used;                  // LRU stamp
    uint16_t dirty_lo, dirty_hi;    // dirty blocks [lo, hi) within the chunk
    uint8_t *data;
} slot_t;

static slot_t slots[SLOTS];
static uint8_t *pool;
static uint64_t clock_;
uint32_t g_cache_hits, g_cache_misses, g_cache_flushes;

static bool ready(void) {
    if (pool) return true;
    EFI_PHYSICAL_ADDRESS p;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, (uint64_t)SLOTS * CHUNK_BYTES / 4096, &p)))
        return false;
    pool = (uint8_t *)p;
    for (unsigned i = 0; i < SLOTS; i++) {
        slots[i].chunk = NO_CHUNK;
        slots[i].data = pool + (uint64_t)i * CHUNK_BYTES;
    }
    return true;
}

static uint64_t dev_blocks(EFI_BLOCK_IO_PROTOCOL *bio) { return bio->Media->LastBlock + 1; }

static slot_t *find(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t chunk) {
    for (unsigned i = 0; i < SLOTS; i++)
        if (slots[i].chunk == chunk && slots[i].bio == bio) return &slots[i];
    return NULL;
}

static EFI_STATUS write_back(slot_t *s) {
    if (s->dirty_hi <= s->dirty_lo) return EFI_SUCCESS;
    uint64_t lba = s->chunk * CHUNK_BLOCKS + s->dirty_lo;
    EFI_STATUS st = bio_write(s->bio, lba, s->dirty_hi - s->dirty_lo, s->data + s->dirty_lo * 512u);
    if (!EFI_ERROR(st)) s->dirty_lo = s->dirty_hi = 0;
    g_cache_flushes++;
    return st;
}

// A slot holding `chunk`, loaded from the device unless `fill` is false
static slot_t *get(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t chunk, bool fill, EFI_STATUS *st) {
    slot_t *s = find(bio, chunk);
    *st = EFI_SUCCESS;
    if (s) {
        g_cache_hits++;
        s->used = ++clock_;
        return s;
    }
    g_cache_misses++;
    slot_t *victim = &slots[0];
    for (unsigned i = 0; i < SLOTS; i++) {
        if (slots[i].chunk == NO_CHUNK) { victim = &slots[i]; break; }
        if (slots[i].used < victim->used) victim = &slots[i];
    }
    if (victim->chunk != NO_CHUNK && EFI_ERROR(*st = write_back(victim))) return NULL;
    victim->chunk = NO_CHUNK;
    if (fill) {
        uint64_t first = chunk * CHUNK_BLOCKS, n = CHUNK_BLOCKS;
        if (first + n > dev_blocks(bio)) n = dev_blocks(bio) - first;
        if (EFI_ERROR(*st = bio_read(bio, first, n, victim->data))) return NULL;
    }
    victim->bio = bio;
    victim->chunk = chunk;
    victim->dirty_lo = victim->dirty_hi = 0;
    victim->used = ++clock_;
    return victim;
}

// Cached chunks in [lba, lba+count): written back first (read) or dropped (write)
static EFI_STATUS settle(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint64_t count, bool drop) {
    uint64_t c0 = lba / CHUNK_BLOCKS, c1 = (lba + count - 1) / CHUNK_BLOCKS;
    for (unsigned i = 0; i < SLOTS; i++) {
        slot_t *s = &slots[i];
        if (s->bio != bio || s->chunk == NO_CHUNK || s->chunk < c0 || s->chunk > c1) continue;
        EFI_STATUS st = write_back(s);
        if (EFI_ERROR(st)) return st;
        if (drop) s->chunk = NO_CHUNK;
    }
    return EFI_SUCCESS;
}

EFI_STATUS blkcache_read(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint64_t count, void *buf) {
    if (!count) return EFI_SUCCESS;
    if (bio->Media->BlockSize != 512 || !ready()) return bio_read(bio, lba, count, buf);
    if (count >= CHUNK_BLOCKS) {
        EFI_STATUS st = settle(bio, lba, count, false);
        return EFI_ERROR(st) ? st : bio_read(bio, lba, count, buf);
    }
    uint8_t *p = buf;
    while (count) {
        uint64_t chunk = lba / CHUNK_BLOCKS, at = lba % CHUNK_BLOCKS;
        uint64_t n = CHUNK_BLOCKS - at < count ? CHUNK_BLOCKS - at : count;
        EFI_STATUS st;
        slot_t *s = get(bio, chunk, true, &st);
        if (!s) return st;
        memcpy(p, s->data + at * 512u, n * 512u);
        p += n * 512u;
        lba += n;
        count -= n;
    }
    return EFI_SUCCESS;
}

EFI_STATUS blkcache_write(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint64_t count, const void *buf) {
    if (!count) return EFI_SUCCESS;
    if (bio->Media->BlockSize != 512 || !ready()) return bio_write(bio, lba, count, buf);
    if (count >= CHUNK_BLOCKS) {
        EFI_STATUS st = settle(bio, lba, count, true);
        return EFI_ERROR(st) ? st : bio_write(bio, lba, count, buf);
    }
    const uint8_t *p = buf;
    while (count) {
        uint64_t chunk = lba / CHUNK_BLOCKS, at = lba % CHUNK_BLOCKS;
        uint64_t n = CHUNK_BLOCKS - at < count ? CHUNK_BLOCKS - at : count;
        EFI_STATUS st;
        slot_t *s = get(bio, chunk, true, &st);
        if (!s) return st;
        memcpy(s->data + at * 512u, p, n * 512u);
        if (s->dirty_hi <= s->dirty_lo) {
            s->dirty_lo = (uint16_t)at;
            s->dirty_hi = (uint16_t)(at + n);
        } else {
            if (at < s->dirty_lo) s->dirty_lo = (uint16_t)at;
            if (at + n > s->dirty_hi) s->dirty_hi = (uint16_t)(at + n);
        }
        p += n * 512u;
        lba += n;
        count -= n;
    }
    return EFI_SUCCESS;
}

EFI_STATUS blkcache_flush(EFI_BLOCK_IO_PROTOCOL *bio) {
    EFI_STATUS result = EFI_SUCCESS;
    for (unsigned i = 0; i < SLOTS; i++) {
        slot_t *s = &slots[i];
        if (s->chunk == NO_CHUNK || (bio && s->bio != bio)) continue;
        EFI_STATUS st = write_back(s);
        if (EFI_ERROR(st)) result = st;
    }
    if (bio) bio_flush(bio);
    return result;
}

bool blkcache_dirty(void) {
    for (unsigned i = 0; i < SLOTS; i++)
        if (slots[i].chunk != NO_CHUNK && slots[i].dirty_hi > slots[i].dirty_lo) return true;
    return false;
}

void blkcache_forget(EFI_BLOCK_IO_PROTOCOL *bio) {
    for (unsigned i = 0; i < SLOTS; i++)
        if (slots[i].bio == bio) slots[i].chunk = NO_CHUNK;
}
