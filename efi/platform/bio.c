// UEFI Block I/O helpers: alignment-safe reads and writes, device paths.

#include <string.h>
#include <stdio.h>
#include "bio.h"

extern EFI_BOOT_SERVICES *gBS;

#define BOUNCE_BYTES (64u * 1024u)
static uint8_t *bounce;     // page aligned, which satisfies any IoAlign a controller asks for

static bool aligned(const EFI_BLOCK_IO_PROTOCOL *bio, const void *buf) {
    uint32_t a = bio->Media->IoAlign;
    return a <= 1 || ((uintptr_t)buf & (a - 1)) == 0;
}

static bool have_bounce(void) {
    if (bounce) return true;
    EFI_PHYSICAL_ADDRESS p;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, BOUNCE_BYTES / 4096, &p))) return false;
    bounce = (uint8_t *)p;
    return true;
}

static EFI_STATUS rw(EFI_BLOCK_IO_PROTOCOL *bio, bool write, uint64_t lba, uint64_t count, void *buf) {
    uint32_t bs = bio->Media->BlockSize;
    for (int attempt = 0; attempt < 2; attempt++) {
        EFI_STATUS st = write ? bio->WriteBlocks(bio, bio->Media->MediaId, lba, count * bs, buf)
                              : bio->ReadBlocks(bio, bio->Media->MediaId, lba, count * bs, buf);
        // a new medium: the protocol now describes it, so ask once more
        if (st != EFI_MEDIA_CHANGED) return st;
    }
    return EFI_MEDIA_CHANGED;
}

static EFI_STATUS transfer(EFI_BLOCK_IO_PROTOCOL *bio, bool write, uint64_t lba, uint64_t count, void *buf) {
    if (!count) return EFI_SUCCESS;
    if (aligned(bio, buf)) return rw(bio, write, lba, count, buf);
    if (!have_bounce()) return EFI_DEVICE_ERROR;
    uint32_t bs = bio->Media->BlockSize;
    uint64_t per = BOUNCE_BYTES / bs;
    uint8_t *p = buf;
    while (count) {
        uint64_t n = count < per ? count : per;
        if (write) memcpy(bounce, p, n * bs);
        EFI_STATUS st = rw(bio, write, lba, n, bounce);
        if (EFI_ERROR(st)) return st;
        if (!write) memcpy(p, bounce, n * bs);
        p += n * bs;
        lba += n;
        count -= n;
    }
    return EFI_SUCCESS;
}

EFI_STATUS bio_read(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint64_t count, void *buf) {
    return transfer(bio, false, lba, count, buf);
}

EFI_STATUS bio_write(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint64_t count, const void *buf) {
#if WYSE_POC_READ_ONLY
    return EFI_WRITE_PROTECTED;
#else
    return transfer(bio, true, lba, count, (void *)buf);
#endif
}

EFI_STATUS bio_flush(EFI_BLOCK_IO_PROTOCOL *bio) { return bio->FlushBlocks(bio); }

EFI_DEVICE_PATH *bio_device_path(EFI_HANDLE h) {
    static const EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *p;
    return EFI_ERROR(gBS->HandleProtocol(h, &dp_guid, (void **)&p)) ? NULL : p;
}

bool bio_path_has(EFI_DEVICE_PATH *p, uint8_t type, uint8_t subtype) {
    for (; p && !dp_end(p) && p->Length >= 4; p = dp_next(p))
        if (p->Type == type && p->SubType == subtype) return true;
    return false;
}

bool bio_path_is_on(EFI_DEVICE_PATH *part, EFI_DEVICE_PATH *disk) {
    if (!part || !disk) return false;
    // every node of the disk's path, then a hard drive (partition) node
    for (; !dp_end(disk); disk = dp_next(disk), part = dp_next(part)) {
        if (disk->Length < 4 || part->Length != disk->Length || dp_end(part)) return false;
        if (memcmp(part, disk, disk->Length)) return false;
    }
    return part->Type == DP_MEDIA && part->SubType == DP_MEDIA_HARDDRIVE;
}

void bio_path_str(EFI_DEVICE_PATH *p, char *out, int cap) {
    int n = 0;
    out[0] = 0;
    for (; p && !dp_end(p) && p->Length >= 4 && n < cap - 16; p = dp_next(p)) {
        const char *name = NULL;
        if (p->Type == DP_MESSAGING) {
            switch (p->SubType) {
            case DP_MSG_USB: name = "USB"; break;
            case DP_MSG_SD: name = "SD"; break;
            case DP_MSG_EMMC: name = "eMMC"; break;
            case DP_MSG_MAC: name = "MAC"; break;
            case DP_MSG_IPV4: name = "IPv4"; break;
            }
        }
        if (p->Type == DP_MEDIA && p->SubType == DP_MEDIA_HARDDRIVE) {
            n += snprintf(out + n, cap - n, "/HD(%u)", ((HARDDRIVE_DEVICE_PATH *)p)->PartitionNumber);
            continue;
        }
        if (name) n += snprintf(out + n, cap - n, "/%s", name);
        else n += snprintf(out + n, cap - n, "/%u.%u", p->Type, p->SubType);
    }
}
