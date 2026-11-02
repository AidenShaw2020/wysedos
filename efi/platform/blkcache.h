// Block cache for 512-byte Block I/O devices (blkcache.c)
#ifndef BLKCACHE_H
#define BLKCACHE_H

#include "../include/efi.h"

EFI_STATUS blkcache_read(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint64_t count, void *buf);
EFI_STATUS blkcache_write(EFI_BLOCK_IO_PROTOCOL *bio, uint64_t lba, uint64_t count, const void *buf);
// Write back what changed: on `bio`, or on every device when NULL
EFI_STATUS blkcache_flush(EFI_BLOCK_IO_PROTOCOL *bio);
bool blkcache_dirty(void);
// Drop everything held for `bio` (its medium went away)
void blkcache_forget(EFI_BLOCK_IO_PROTOCOL *bio);

#endif
