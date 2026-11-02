// Force-included into the real FatFs and its glue (fatvol.c): their API takes
// fat_ names so that the f_ names stay free for the file API in fs.c, which
// picks netfs or a FatFs volume per path.
#define FF_NO_WINDOWS_H 1
#define f_chdir fat_chdir
#define f_chdrive fat_chdrive
#define f_chmod fat_chmod
#define f_close fat_close
#define f_closedir fat_closedir
#define f_expand fat_expand
#define f_fdisk fat_fdisk
#define f_findfirst fat_findfirst
#define f_findnext fat_findnext
#define f_forward fat_forward
#define f_getcwd fat_getcwd
#define f_getfree fat_getfree
#define f_getlabel fat_getlabel
#define f_gets fat_gets
#define f_lseek fat_lseek
#define f_mkdir fat_mkdir
#define f_mkfs fat_mkfs
#define f_mount fat_mount
#define f_open fat_open
#define f_opendir fat_opendir
#define f_printf fat_printf
#define f_putc fat_putc
#define f_puts fat_puts
#define f_read fat_read
#define f_readdir fat_readdir
#define f_rename fat_rename
#define f_setcp fat_setcp
#define f_setlabel fat_setlabel
#define f_stat fat_stat
#define f_sync fat_sync
#define f_truncate fat_truncate
#define f_unlink fat_unlink
#define f_utime fat_utime
#define f_write fat_write
