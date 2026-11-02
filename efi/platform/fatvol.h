// FatFs volumes on UEFI Block I/O (implemented in fatfs/fatvol.c).
//
// The real FatFs is compiled with its API renamed (fatfs/rename.h), so its
// types never meet the file API of platform/ff.h. Callers hand over storage
// for FatFs's FIL and DIR as opaque bytes and get results as FRESULT codes,
// which have the same values on both sides.
#ifndef FATVOL_H
#define FATVOL_H

#include <stdint.h>
#include <stdbool.h>

#define FATVOL_FIL_BYTES 640
#define FATVOL_DIR_BYTES 128

enum { FATVOL_DATA = 0, FATVOL_SOURCE = 1, FATVOL_TARGET_ESP = 2, FATVOL_TARGET_DATA = 3 };

typedef struct {
    uint64_t size;
    uint16_t date, time;
    uint8_t attr;
    char altname[13];
    char name[256];
} fatvol_info;

// Put volume `vol` on blocks [first_lba, first_lba + count) of a 512-byte
// Block I/O device and mount it (FR_NO_FILESYSTEM if it is not formatted)
int fatvol_attach(int vol, void *block_io, uint64_t first_lba, uint64_t count);
void fatvol_detach(int vol);
bool fatvol_attached(int vol);
// Format an attached volume: FAT32 when it is big enough, else FAT16
int fatvol_mkfs(int vol, const char *label);
int fatvol_mkfs_dos(int vol);
uint64_t fatvol_free_bytes(int vol);

int fatvol_open(void *fil, int vol, const char *path, uint8_t mode);
int fatvol_close(void *fil);
int fatvol_read(void *fil, void *buf, unsigned n, unsigned *got);
int fatvol_write(void *fil, const void *buf, unsigned n, unsigned *put);
int fatvol_lseek(void *fil, uint64_t ofs);
int fatvol_sync(void *fil);
int fatvol_truncate(void *fil);
int fatvol_expand(void *fil, uint64_t size);    // contiguous, at once (new empty file)
uint64_t fatvol_size(void *fil);
uint64_t fatvol_tell(void *fil);

int fatvol_opendir(void *dir, int vol, const char *path);
int fatvol_readdir(void *dir, fatvol_info *info);   // empty name at the end
int fatvol_closedir(void *dir);
int fatvol_stat(int vol, const char *path, fatvol_info *info);
int fatvol_unlink(int vol, const char *path);
int fatvol_rename(int vol, const char *from, const char *to);
int fatvol_mkdir(int vol, const char *path);

#endif
