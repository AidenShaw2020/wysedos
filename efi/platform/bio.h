// UEFI Block I/O helpers shared by the FatFs volumes (fatfs/fatvol.c), the
// installer and the USB floppy / CD-ROM drives (hostblk.c).
#ifndef BIO_H
#define BIO_H

#include <stdint.h>
#include <stdbool.h>
#include "../include/efi.h"

// Read or write `count` blocks of the device's own size at `lba`. Buffers that
// do not meet the device's alignment go through a bounce buffer. Returns the
// firmware status (EFI_MEDIA_CHANGED is retried once with the new medium).
EFI_STATUS bio_read(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint64_t count, void *buf);
EFI_STATUS bio_write(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint64_t count, const void *buf);
EFI_STATUS bio_flush(EFI_BLOCK_IO_PROTOCOL *bio);

// The device path of a handle, and whether it holds a node of this type/subtype
EFI_DEVICE_PATH *bio_device_path(EFI_HANDLE h);
bool bio_path_has(EFI_DEVICE_PATH *p, uint8_t type, uint8_t subtype);
// Whether `part` lies on the disk `disk` (its path is the disk's plus a partition node)
bool bio_path_is_on(EFI_DEVICE_PATH *part, EFI_DEVICE_PATH *disk);
// "PciRoot/.../eMMC" style summary of a device path, for the log
void bio_path_str(EFI_DEVICE_PATH *p, char *out, int cap);

#endif
