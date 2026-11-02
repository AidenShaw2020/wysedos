// The PC model's file API (platform/ff.h), sent to netfs or a FatFs volume.
//
// "N:path" is FatFs volume N (fatvol.h); "SD:/path", X86Pi's name for its
// card, and plain paths go to the default backend: netfs when the monitor was
// netbooted, the data volume when it started from the eMMC or a USB stick.

#include <string.h>
#include <ctype.h>
#include "ff.h"
#include "fatvol.h"

FRESULT netfs_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT netfs_close(FIL *fp);
FRESULT netfs_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT netfs_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT netfs_lseek(FIL *fp, FSIZE_t ofs);
FRESULT netfs_sync(FIL *fp);
FRESULT netfs_truncate(FIL *fp);
FRESULT netfs_opendir(DIR *dp, const TCHAR *path);
FRESULT netfs_closedir(DIR *dp);
FRESULT netfs_readdir(DIR *dp, FILINFO *fno);
FRESULT netfs_stat(const TCHAR *path, FILINFO *fno);
FRESULT netfs_unlink(const TCHAR *path);
FRESULT netfs_rename(const TCHAR *old, const TCHAR *new_);
FRESULT netfs_mkdir(const TCHAR *path);

static int default_backend = FS_NETFS;
static int default_vol;
static FATFS fat_marker;    // obj.fs of an open FatFs object

void fs_set_default(int backend, int vol) {
    default_backend = backend;
    default_vol = vol;
}
int fs_default_backend(void) { return default_backend; }

static int route(const TCHAR *path, int *vol, const TCHAR **rest) {
    if (isdigit((unsigned char)path[0]) && path[1] == ':') {
        *vol = path[0] - '0';
        *rest = path + 2;
        return FS_FAT;
    }
    if (!strncmp(path, "SD:", 3)) path += 3;
    while (*path == '/') path++;
    *vol = default_vol;
    *rest = path;
    return default_backend;
}

static void fil_sync_fields(FIL *fp) {
    fp->obj.objsize = fatvol_size(fp->fat);
    fp->fptr = fatvol_tell(fp->fat);
}

static void info_to(FILINFO *o, const fatvol_info *i) {
    memset(o, 0, sizeof(*o));
    o->fsize = i->size;
    o->fdate = i->date;
    o->ftime = i->time;
    o->fattrib = i->attr;
    memcpy(o->altname, i->altname, sizeof(o->altname));
    memcpy(o->fname, i->name, sizeof(o->fname));
}

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode) {
    int vol;
    const TCHAR *rest;
    if (route(path, &vol, &rest) == FS_NETFS) {
        FRESULT r = netfs_open(fp, rest, mode);
        fp->backend = FS_NETFS;
        return r;
    }
    memset(fp, 0, sizeof(*fp));
    FRESULT r = (FRESULT)fatvol_open(fp->fat, vol, rest, mode);
    if (r != FR_OK) return r;
    fp->backend = FS_FAT;
    fp->obj.fs = &fat_marker;
    fp->flag = mode;
    fil_sync_fields(fp);
    return FR_OK;
}

FRESULT f_close(FIL *fp) {
    if (fp->backend != FS_FAT) return netfs_close(fp);
    if (!fp->obj.fs) return FR_INVALID_OBJECT;
    fp->obj.fs = NULL;
    return (FRESULT)fatvol_close(fp->fat);
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
    if (fp->backend != FS_FAT) return netfs_read(fp, buff, btr, br);
    if (!fp->obj.fs) return FR_INVALID_OBJECT;
    FRESULT r = (FRESULT)fatvol_read(fp->fat, buff, btr, br);
    fil_sync_fields(fp);
    return r;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
    if (fp->backend != FS_FAT) return netfs_write(fp, buff, btw, bw);
    if (!fp->obj.fs) return FR_INVALID_OBJECT;
    FRESULT r = (FRESULT)fatvol_write(fp->fat, buff, btw, bw);
    fil_sync_fields(fp);
    return r;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
    if (fp->backend != FS_FAT) return netfs_lseek(fp, ofs);
    if (!fp->obj.fs) return FR_INVALID_OBJECT;
    if (ofs == CREATE_LINKMAP) return FR_OK;    // fatvol maps big files by itself
    FRESULT r = (FRESULT)fatvol_lseek(fp->fat, ofs);
    fil_sync_fields(fp);
    return r;
}

FRESULT f_sync(FIL *fp) {
    if (fp->backend != FS_FAT) return netfs_sync(fp);
    if (!fp->obj.fs) return FR_INVALID_OBJECT;
    return (FRESULT)fatvol_sync(fp->fat);
}

FRESULT f_truncate(FIL *fp) {
    if (fp->backend != FS_FAT) return netfs_truncate(fp);
    if (!fp->obj.fs) return FR_INVALID_OBJECT;
    FRESULT r = (FRESULT)fatvol_truncate(fp->fat);
    fil_sync_fields(fp);
    return r;
}

FRESULT f_expand(FIL *fp, FSIZE_t size) {
    if (fp->backend != FS_FAT || !fp->obj.fs) return FR_DENIED;
    FRESULT r = (FRESULT)fatvol_expand(fp->fat, size);
    fil_sync_fields(fp);
    return r;
}

FRESULT f_opendir(DIR *dp, const TCHAR *path) {
    int vol;
    const TCHAR *rest;
    if (route(path, &vol, &rest) == FS_NETFS) {
        FRESULT r = netfs_opendir(dp, rest);
        dp->backend = FS_NETFS;
        return r;
    }
    memset(dp, 0, sizeof(*dp));
    FRESULT r = (FRESULT)fatvol_opendir(dp->fat, vol, rest);
    if (r != FR_OK) return r;
    dp->backend = FS_FAT;
    dp->obj.fs = &fat_marker;
    return FR_OK;
}

FRESULT f_readdir(DIR *dp, FILINFO *fno) {
    if (dp->backend != FS_FAT) return netfs_readdir(dp, fno);
    if (!dp->obj.fs) return FR_INVALID_OBJECT;
    static fatvol_info i;
    FRESULT r = (FRESULT)fatvol_readdir(dp->fat, &i);
    info_to(fno, &i);
    return r;
}

FRESULT f_closedir(DIR *dp) {
    if (dp->backend != FS_FAT) return netfs_closedir(dp);
    if (!dp->obj.fs) return FR_INVALID_OBJECT;
    dp->obj.fs = NULL;
    return (FRESULT)fatvol_closedir(dp->fat);
}

FRESULT f_stat(const TCHAR *path, FILINFO *fno) {
    int vol;
    const TCHAR *rest;
    if (route(path, &vol, &rest) == FS_NETFS) return netfs_stat(rest, fno);
    static fatvol_info i;
    FRESULT r = (FRESULT)fatvol_stat(vol, rest, &i);
    if (r == FR_OK && fno) info_to(fno, &i);
    return r;
}

FRESULT f_unlink(const TCHAR *path) {
    int vol;
    const TCHAR *rest;
    if (route(path, &vol, &rest) == FS_NETFS) return netfs_unlink(rest);
    return (FRESULT)fatvol_unlink(vol, rest);
}

FRESULT f_mkdir(const TCHAR *path) {
    int vol;
    const TCHAR *rest;
    if (route(path, &vol, &rest) == FS_NETFS) return netfs_mkdir(rest);
    return (FRESULT)fatvol_mkdir(vol, rest);
}

FRESULT f_rename(const TCHAR *old, const TCHAR *new_) {
    int vol, vol2;
    const TCHAR *a, *b;
    int be = route(old, &vol, &a);
    route(new_, &vol2, &b);
    if (be == FS_NETFS) return netfs_rename(a, b);
    return (FRESULT)fatvol_rename(vol, a, b);
}

TCHAR *f_gets(TCHAR *buff, int len, FIL *fp) {
    int n = 0;
    while (n < len - 1) {
        char c;
        UINT got;
        if (f_read(fp, &c, 1, &got) != FR_OK || !got) break;
        buff[n++] = c;
        if (c == '\n') break;
    }
    buff[n] = 0;
    return n ? buff : NULL;
}
