#ifndef WYSE_EXTRAS_NTFS_H
#define WYSE_EXTRAS_NTFS_H

#include "ff.h"
#include "blockdev.h"
#include <stdbool.h>

#define EXTRAS_NTFS_VOLUME 9

bool extras_ntfs_mount(BlockDev *partition);
void extras_ntfs_unmount(void);
void extras_ntfs_shutdown(void);
bool extras_ntfs_writable(void);

FRESULT extras_ntfs_open(FIL *fp, const char *path, BYTE mode);
FRESULT extras_ntfs_close(FIL *fp);
FRESULT extras_ntfs_read(FIL *fp, void *buf, UINT size, UINT *got);
FRESULT extras_ntfs_write(FIL *fp, const void *buf, UINT size, UINT *put);
FRESULT extras_ntfs_sync(FIL *fp);
FRESULT extras_ntfs_stat(const char *path, FILINFO *info);
FRESULT extras_ntfs_opendir(DIR *dir, const char *path);
FRESULT extras_ntfs_readdir(DIR *dir, FILINFO *info);
FRESULT extras_ntfs_closedir(DIR *dir);
FRESULT extras_ntfs_unlink(const char *path);
FRESULT extras_ntfs_rename(const char *from, const char *to);
FRESULT extras_ntfs_mkdir(const char *path);
#endif
