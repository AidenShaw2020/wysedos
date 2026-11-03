// FatFs volumes on UEFI Block I/O (see platform/fatvol.h).
//
// Compiled with fatfs/rename.h, so f_open here is FatFs's (fat_open at link
// time). Each volume is a run of blocks on a 512-byte device - a partition
// handle from the firmware, or a range of the raw eMMC the installer has just
// partitioned - and FatFs sees it as an unpartitioned medium starting at 0.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ff.h"
#include "diskio.h"
#include "../platform/bio.h"
#include "../platform/fatvol.h"
#include "../platform/blkcache.h"
#include "../../pc/blockdev.h"

_Static_assert(sizeof(FIL) <= FATVOL_FIL_BYTES, "FATVOL_FIL_BYTES too small");
_Static_assert(sizeof(DIR) <= FATVOL_DIR_BYTES, "FATVOL_DIR_BYTES too small");

uint32_t wyse_fat_time(void);

typedef struct {
    EFI_BLOCK_IO_PROTOCOL *bio;
    BlockDev *block;
    uint64_t first, count;
    bool mounted;
    FATFS fs;
} vol_t;

static vol_t vols[FF_VOLUMES];

//--------------------------------------------------------------------+
// FatFs's disk interface
//--------------------------------------------------------------------+

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv>=FF_VOLUMES) return STA_NOINIT;
    vol_t *v=&vols[pdrv];
    if (v->block) return blk_size(v->block)?(v->block->readonly?STA_PROTECT:0):STA_NOINIT;
    return v->bio?0:STA_NOINIT;
}
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv>=FF_VOLUMES) return RES_PARERR;
    vol_t *v = &vols[pdrv];
    if (v->block) {
        if (sector>v->count || count>v->count-sector || count>0x7fffff) return RES_PARERR;
        return blk_pread(v->block,(uint64_t)sector*512,buff,count*512)==(int)(count*512)?RES_OK:RES_ERROR;
    }
    if (!v->bio) return RES_NOTRDY;
    if ((uint64_t)sector + count > v->count) return RES_PARERR;
    return EFI_ERROR(blkcache_read(v->bio, v->first + sector, count, buff)) ? RES_ERROR : RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv>=FF_VOLUMES) return RES_PARERR;
    vol_t *v = &vols[pdrv];
    if (v->block) {
        if (v->block->readonly) return RES_WRPRT;
        if (sector>v->count || count>v->count-sector || count>0x7fffff) return RES_PARERR;
        return blk_pwrite(v->block,(uint64_t)sector*512,buff,count*512)==(int)(count*512)?RES_OK:RES_ERROR;
    }
    if (!v->bio) return RES_NOTRDY;
    if ((uint64_t)sector + count > v->count) return RES_PARERR;
    EFI_STATUS st = blkcache_write(v->bio, v->first + sector, count, buff);
    if (st == EFI_WRITE_PROTECTED) return RES_WRPRT;
    return EFI_ERROR(st) ? RES_ERROR : RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv>=FF_VOLUMES) return RES_PARERR;
    vol_t *v = &vols[pdrv];
    if (v->block) {
        if (!blk_size(v->block)) return RES_NOTRDY;
        switch(cmd) {
        case CTRL_SYNC: return blk_sync(v->block)<0?RES_ERROR:RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t *)buff=(LBA_t)v->count; return RES_OK;
        case GET_SECTOR_SIZE: *(WORD *)buff=512; return RES_OK;
        case GET_BLOCK_SIZE: *(DWORD *)buff=1; return RES_OK;
        default: return RES_PARERR;
        }
    }
    if (!v->bio) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: return EFI_ERROR(blkcache_flush(v->bio)) ? RES_ERROR : RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buff = (LBA_t)v->count; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 2048; return RES_OK;     // 1 MB: eMMC erase blocks
    default: return RES_PARERR;
    }
}

DWORD get_fattime(void) { return wyse_fat_time(); }
void *ff_memalloc(UINT msize) { return malloc(msize); }
void ff_memfree(void *mblock) { free(mblock); }

//--------------------------------------------------------------------+
// volumes
//--------------------------------------------------------------------+

static const char *vpath(int vol, const char *path, char *buf, size_t cap) {
    while (*path == '/') path++;
    snprintf(buf, cap, "%d:%s", vol, path);
    return buf;
}

int fatvol_attach(int vol, void *block_io, uint64_t first_lba, uint64_t count) {
    if (vol < 0 || vol >= FF_VOLUMES) return FR_INVALID_DRIVE;
    EFI_BLOCK_IO_PROTOCOL *bio = block_io;
    if (!bio || bio->Media->BlockSize != 512 || count > 0xffffffffull) return FR_INVALID_PARAMETER;
    fatvol_detach(vol);
    vol_t *v = &vols[vol];
    v->bio = bio;
    v->first = first_lba;
    v->count = count;
    char p[4];
    FRESULT r = f_mount(&v->fs, vpath(vol, "", p, sizeof(p)), 1);
    v->mounted = r == FR_OK;
    return r;
}

void fatvol_detach(int vol) {
    if (vol < 0 || vol >= FF_VOLUMES || (!vols[vol].bio && !vols[vol].block)) return;
    char p[4];
    f_mount(NULL, vpath(vol, "", p, sizeof(p)), 0);
    memset(&vols[vol], 0, sizeof(vols[vol]));
}
int fatvol_attach_block(int vol,BlockDev *block) {
    uint64_t size=blk_size(block);
    if (vol<4 || vol>=FF_VOLUMES || !size || size%512 || size/512>0xffffffffull)
        return FR_INVALID_PARAMETER;
    fatvol_detach(vol);
    vol_t *v=&vols[vol]; v->block=block; v->count=size/512;
    char p[4];
    FRESULT r=f_mount(&v->fs,vpath(vol,"",p,sizeof(p)),1);
    v->mounted=r==FR_OK;
    return r;
}

bool fatvol_attached(int vol) { return vol >= 0 && vol < FF_VOLUMES && vols[vol].mounted; }

int fatvol_mkfs(int vol, const char *label) {
    if (vol < 0 || vol >= FF_VOLUMES || !vols[vol].bio) return FR_INVALID_DRIVE;
    vol_t *v = &vols[vol];
    char p[4];
    vpath(vol, "", p, sizeof(p));
    f_mount(NULL, p, 0);
    v->mounted = false;
    // FAT32 from 2 GB up (FatFs's own rule), 32 KB clusters there: a few
    // large disk images, and short cluster chains to map
    MKFS_PARM opt = {FM_FAT | FM_FAT32 | FM_SFD, 2, 0, 0, v->count >= 0x400000 ? 32768 : 0};
    static BYTE work[32768];
    FRESULT r = f_mkfs(p, &opt, work, sizeof(work));
    if (r != FR_OK) return r;
    r = f_mount(&v->fs, p, 1);
    if (r != FR_OK) return r;
    v->mounted = true;
    if (label && label[0]) {
        char l[40];
        snprintf(l, sizeof(l), "%s%s", p, label);
        f_setlabel(l);
    }
    return FR_OK;
}

/* DOS/Windows 98: FAT32, modest clusters, no outer partition scan. */
int fatvol_mkfs_dos(int vol) {
    if (vol<0 || vol>=FF_VOLUMES || !vols[vol].bio) return FR_INVALID_DRIVE;
    char p[4]; vpath(vol,"",p,sizeof(p)); f_mount(NULL,p,0); vols[vol].mounted=false;
    MKFS_PARM opt={FM_FAT32|FM_SFD,2,1,0,4096};
    static BYTE work[32768];
    FRESULT r=f_mkfs(p,&opt,work,sizeof(work));
    if (r!=FR_OK) return r;
    r=f_mount(&vols[vol].fs,p,1); vols[vol].mounted=r==FR_OK;
    return r;
}

uint64_t fatvol_free_bytes(int vol) {
    char p[4];
    DWORD clusters;
    FATFS *fs;
    if (f_getfree(vpath(vol, "", p, sizeof(p)), &clusters, &fs) != FR_OK) return 0;
    return (uint64_t)clusters * fs->csize * 512;
}

//--------------------------------------------------------------------+
// files: big ones get a cluster link map, so seeking is a table lookup
//--------------------------------------------------------------------+

static void drop_linkmap(FIL *f) {
    if (!f->cltbl) return;
    free(f->cltbl);
    f->cltbl = NULL;
}

static void make_linkmap(FIL *f) {
    if (f_size(f) < (1u << 20)) return;
    DWORD n = 64;
    for (int attempt = 0; attempt < 2; attempt++) {
        DWORD *tbl = malloc(n * sizeof(*tbl));
        if (!tbl) return;
        tbl[0] = n;
        f->cltbl = tbl;
        FRESULT r = f_lseek(f, CREATE_LINKMAP);
        if (r == FR_OK) return;
        DWORD needed = tbl[0];
        f->cltbl = NULL;
        free(tbl);
        if (r != FR_NOT_ENOUGH_CORE || needed <= n) return;
        n = needed;
    }
}

int fatvol_open(void *fil, int vol, const char *path, uint8_t mode) {
    FIL *f = fil;
    char p[300];
    memset(f, 0, sizeof(*f));
    FRESULT r = f_open(f, vpath(vol, path, p, sizeof(p)), mode);
    if (r != FR_OK) return r;
    make_linkmap(f);
    if ((mode & FA_OPEN_APPEND) == FA_OPEN_APPEND) {
        drop_linkmap(f);
        f_lseek(f, f_size(f));
    }
    return FR_OK;
}

int fatvol_close(void *fil) {
    FIL *f = fil;
    drop_linkmap(f);
    return f_close(f);
}

int fatvol_read(void *fil, void *buf, unsigned n, unsigned *got) {
    UINT g = 0;
    FRESULT r = f_read(fil, buf, n, &g);
    *got = g;
    return r;
}

int fatvol_write(void *fil, const void *buf, unsigned n, unsigned *put) {
    FIL *f = fil;
    // a link-mapped file cannot grow; one that is about to loses its map
    if (f->cltbl && (uint64_t)f_tell(f) + n > f_size(f)) {
        FSIZE_t at = f_tell(f);
        drop_linkmap(f);
        f_lseek(f, at);
    }
    UINT p = 0;
    FRESULT r = f_write(f, buf, n, &p);
    *put = p;
    return r;
}

int fatvol_lseek(void *fil, uint64_t ofs) {
    FIL *f = fil;
    if (ofs > 0xffffffffull) return FR_INVALID_PARAMETER;
    if (f->cltbl && ofs > f_size(f)) {
        drop_linkmap(f);
    }
    return f_lseek(f, (FSIZE_t)ofs);
}

int fatvol_sync(void *fil) { return f_sync(fil); }

int fatvol_truncate(void *fil) {
    drop_linkmap(fil);
    return f_truncate(fil);
}

int fatvol_expand(void *fil, uint64_t size) {
    FIL *f = fil;
    if (size > 0xffffffffull) return FR_INVALID_PARAMETER;
    drop_linkmap(f);
    FRESULT r = f_expand(f, (FSIZE_t)size, 1);
    if (r == FR_OK) make_linkmap(f);
    return r;
}

uint64_t fatvol_size(void *fil) { return f_size((FIL *)fil); }
uint64_t fatvol_tell(void *fil) { return f_tell((FIL *)fil); }

//--------------------------------------------------------------------+
// directories and names
//--------------------------------------------------------------------+

static void info_from(fatvol_info *o, const FILINFO *i) {
    o->size = i->fsize;
    o->date = i->fdate;
    o->time = i->ftime;
    o->attr = i->fattrib;
    memcpy(o->altname, i->altname, sizeof(o->altname));
    o->altname[sizeof(o->altname) - 1] = 0;
    strncpy(o->name, i->fname, sizeof(o->name) - 1);
    o->name[sizeof(o->name) - 1] = 0;
}

int fatvol_opendir(void *dir, int vol, const char *path) {
    char p[300];
    memset(dir, 0, sizeof(DIR));
    return f_opendir(dir, vpath(vol, path, p, sizeof(p)));
}

int fatvol_readdir(void *dir, fatvol_info *info) {
    static FILINFO i;
    FRESULT r = f_readdir(dir, &i);
    memset(info, 0, sizeof(*info));
    if (r == FR_OK) info_from(info, &i);
    return r;
}

int fatvol_closedir(void *dir) { return f_closedir(dir); }

int fatvol_stat(int vol, const char *path, fatvol_info *info) {
    static FILINFO i;
    char p[300];
    FRESULT r = f_stat(vpath(vol, path, p, sizeof(p)), &i);
    if (r == FR_OK && info) info_from(info, &i);
    return r;
}

int fatvol_unlink(int vol, const char *path) {
    char p[300];
    return f_unlink(vpath(vol, path, p, sizeof(p)));
}

int fatvol_rename(int vol, const char *from, const char *to) {
    char a[300], b[300];
    vpath(vol, from, a, sizeof(a));
    // FatFs takes the new name without a drive, on the same volume
    while (*to == '/') to++;
    snprintf(b, sizeof(b), "%s", to);
    return f_rename(a, b);
}

int fatvol_mkdir(int vol, const char *path) {
    char p[300];
    return f_mkdir(vpath(vol, path, p, sizeof(p)));
}
