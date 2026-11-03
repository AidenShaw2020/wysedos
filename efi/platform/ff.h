// FatFs-compatible file API for the PC model.
//
// The X86Pi PC model reads its BIOS, config and disk images through FatFs
// (f_open/f_read/...). fs.c sends each call to one of two backends: netfs.c,
// which forwards it over the direct cable to tools/rig/netfs.py on the
// development PC, or the real FatFs on a UEFI Block I/O volume (eMMC, USB
// stick; fatfs/fatvol.c). A path "N:..." names FatFs volume N; any other path
// goes to the default backend, the one the monitor was started from.
#ifndef FF_H
#define FF_H

#include <stdint.h>

#define FF_USE_FASTSEEK 0

typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint64_t QWORD;
typedef uint64_t FSIZE_t;
typedef uint64_t LBA_t;
typedef char TCHAR;

typedef enum {
    FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH,
    FR_INVALID_NAME, FR_DENIED, FR_EXIST, FR_INVALID_OBJECT, FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_NO_FILESYSTEM, FR_MKFS_ABORTED, FR_TIMEOUT,
    FR_LOCKED, FR_NOT_ENOUGH_CORE, FR_TOO_MANY_OPEN_FILES, FR_INVALID_PARAMETER
} FRESULT;

#define FA_READ             0x01
#define FA_WRITE            0x02
#define FA_OPEN_EXISTING    0x00
#define FA_CREATE_NEW       0x04
#define FA_CREATE_ALWAYS    0x08
#define FA_OPEN_ALWAYS      0x10
#define FA_OPEN_APPEND      0x30

#define AM_RDO  0x01
#define AM_HID  0x02
#define AM_SYS  0x04
#define AM_DIR  0x10
#define AM_ARC  0x20

#define CREATE_LINKMAP ((FSIZE_t)0 - 1)

typedef struct { int unused; } FATFS;

typedef struct {
    struct {
        void *fs;           // non-NULL while open
        uint32_t handle;
        FSIZE_t objsize;
    } obj;
    FSIZE_t fptr;
    BYTE flag;
    DWORD *cltbl;
    BYTE backend;           // FS_NETFS or FS_FAT
    BYTE image_volume;      // exclusive USB image source volume + 1, or zero
    uint64_t fat[80];       // FatFs's own FIL (FATVOL_FIL_BYTES)
} FIL;

typedef struct {
    struct {
        void *fs;
        uint32_t handle;
    } obj;
    BYTE backend;
    uint64_t fat[16];       // FatFs's own DIR (FATVOL_DIR_BYTES)
} DIR;

enum { FS_NONE, FS_NETFS, FS_FAT };

typedef struct {
    FSIZE_t fsize;
    WORD fdate;
    WORD ftime;
    BYTE fattrib;
    TCHAR altname[13];
    TCHAR fname[256];
} FILINFO;

#define f_size(fp) ((fp)->obj.objsize)
#define f_tell(fp) ((fp)->fptr)
#define f_eof(fp) ((fp)->fptr == (fp)->obj.objsize)

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT f_close(FIL *fp);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT f_sync(FIL *fp);
FRESULT f_truncate(FIL *fp);
FRESULT f_opendir(DIR *dp, const TCHAR *path);
FRESULT f_closedir(DIR *dp);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
FRESULT f_stat(const TCHAR *path, FILINFO *fno);
FRESULT f_unlink(const TCHAR *path);
FRESULT f_rename(const TCHAR *old, const TCHAR *new_);
FRESULT f_mkdir(const TCHAR *path);
TCHAR *f_gets(TCHAR *buff, int len, FIL *fp);
FRESULT f_expand(FIL *fp, FSIZE_t size);     // FatFs volumes only: contiguous, new empty file

// which backend plain paths go to (fs.c)
void fs_set_default(int backend, int vol);
int fs_default_backend(void);

#endif
