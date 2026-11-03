/* Real FAT filesystem + file router + exclusive image ownership on a RAM disk.
 * No physical storage, firmware service or guest installation is accessed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../efi/platform/ff.h"
#include "../../efi/platform/bio.h"
#include "../../efi/platform/fatvol.h"
#include "../../pc/image_media.h"
#include "../../pc/blockdev.h"
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#ifdef IMAGE_DRIVE_TEST
#include "../../pc/disk.c"
#include "../../pc/blockdev.c"
uint8_t *cpu_get_phys_mem(CPUI386 *cpu) { (void)cpu; abort(); }
int native_disk_bind(BlockDev *d) { (void)d; abort(); }
#endif
static unsigned char *bytes; static size_t length;
static int visible=1, live=1, sync_fail, syncs, transfers, returned, write_calls;
static int seek_disk(BlockDev *d,uint64_t off) { d->pos=off; return live && off<=length?0:-1; }
static uint64_t size_disk(BlockDev *d) { (void)d; return live?length:0; }
static int read_disk(BlockDev *d,void *p,unsigned n) {
    if (!live || d->pos>length || n>length-d->pos) return -1;
    memcpy(p,bytes+d->pos,n); d->pos+=n; return n;
}
static int write_disk(BlockDev *d,const void *p,unsigned n) {
    if (!live || d->pos>length || n>length-d->pos) return -1;
    memcpy(bytes+d->pos,p,n); d->pos+=n; write_calls++; return n;
}
static int sync_disk(BlockDev *d) { (void)d; syncs++; return sync_fail?-1:0; }
int hostblk_image_info(const char *which,void **id,uint32_t *media) {
    if (strcmp(which,"uhd1") || !live) return 0;
    *id=bytes; *media=42; return 1;
}
int hostblk_image_take(const char *which,void *id,uint32_t media,BlockDev *d,int *port) {
    CHECK(!strcmp(which,"uhd1") && id==bytes && media==42 && visible);
    *d=(BlockDev){read_disk,write_disk,seek_disk,size_disk,sync_disk,bytes,0,0};
    *port=0; visible=0; transfers++; return 1;
}
int hostblk_image_return(BlockDev *d,int port) {
    CHECK(!visible && port==0);
    if (live && sync_fail) return 0;
    memset(d,0,sizeof(*d)); visible=live; returned++; return 1;
}
uint32_t wyse_fat_time(void) { return (46u<<25)|(1u<<21)|(1u<<16); }
/* These backends must never be reached by an image-source test. */
EFI_STATUS blkcache_read(EFI_BLOCK_IO_PROTOCOL *b,uint64_t l,uint64_t n,void *p) { abort(); }
EFI_STATUS blkcache_write(EFI_BLOCK_IO_PROTOCOL *b,uint64_t l,uint64_t n,const void *p) { abort(); }
EFI_STATUS blkcache_flush(EFI_BLOCK_IO_PROTOCOL *b) { abort(); }
EFI_STATUS bio_flush(EFI_BLOCK_IO_PROTOCOL *b) { abort(); }
FRESULT netfs_open(FIL *f,const TCHAR *p,BYTE m) { abort(); }
FRESULT netfs_close(FIL *f) { abort(); }
FRESULT netfs_read(FIL *f,void *p,UINT n,UINT *g) { abort(); }
FRESULT netfs_write(FIL *f,const void *p,UINT n,UINT *g) { abort(); }
FRESULT netfs_lseek(FIL *f,FSIZE_t n) { abort(); }
FRESULT netfs_sync(FIL *f) { abort(); }
FRESULT netfs_truncate(FIL *f) { abort(); }
FRESULT netfs_opendir(DIR *d,const TCHAR *p) { abort(); }
FRESULT netfs_closedir(DIR *d) { abort(); }
FRESULT netfs_readdir(DIR *d,FILINFO *i) { abort(); }
FRESULT netfs_stat(const TCHAR *p,FILINFO *i) { abort(); }
FRESULT netfs_unlink(const TCHAR *p) { abort(); }
FRESULT netfs_rename(const TCHAR *a,const TCHAR *b) { abort(); }
FRESULT netfs_mkdir(const TCHAR *p) { abort(); }
int main(int argc,char **argv) {
    CHECK(argc==2);
    FILE *input=fopen(argv[1],"rb"); CHECK(input);
    fseek(input,0,SEEK_END); length=(size_t)ftell(input); rewind(input);
    bytes=malloc(length); CHECK(bytes && fread(bytes,1,length,input)==length); fclose(input);
    CHECK(image_media_available(0) && !image_media_available(1));
    CHECK(image_media_begin(0)==4 && !visible && transfers==1);
    DIR dir; FILINFO info; CHECK(f_opendir(&dir,"4:/DIR")==FR_OK);
    CHECK(f_readdir(&dir,&info)==FR_OK && info.fname[0]); CHECK(f_closedir(&dir)==FR_OK);
    FIL first,second,missing,duplicate; UINT n; unsigned char data[512];
    CHECK(f_open(&missing,"4:/absent.img",FA_READ)==FR_NO_FILE && !visible);
    CHECK(f_open(&first,"4:/DIR/FLOPPY.IMG",FA_READ|FA_WRITE)==FR_OK);
    CHECK(f_open(&duplicate,"4:/DIR/FLOPPY.IMG",FA_READ|FA_WRITE)==FR_LOCKED);
    CHECK(f_open(&second,"4:/DIR/SECOND.IMG",FA_READ)==FR_OK);
    CHECK(image_media_end(4) && !visible); /* Browser closed; two images own it. */
    CHECK(f_read(&first,data,512,&n)==FR_OK && n==512);
    memset(data,0x5e,sizeof(data));
    CHECK(f_lseek(&first,512)==FR_OK && f_write(&first,data,512,&n)==FR_OK && n==512);
    CHECK(f_sync(&first)==FR_OK && write_calls);
    CHECK(f_close(&first)==FR_OK && !visible && returned==0);
    CHECK(f_close(&second)==FR_OK && visible && returned==1);
    CHECK(image_media_begin(0)==4 && transfers==2);
    CHECK(f_open(&first,"4:/DIR/FLOPPY.IMG",FA_READ|FA_WRITE)==FR_OK);
    CHECK(image_media_end(4) && !visible);
    CHECK(f_lseek(&first,512)==FR_OK && f_read(&first,data,512,&n)==FR_OK && n==512 && data[0]==0x5e);
    memset(data,0x62,sizeof(data)); CHECK(f_write(&first,data,512,&n)==FR_OK);
    sync_fail=1; CHECK(f_close(&first)!=FR_OK && !visible);
    sync_fail=0; CHECK(f_close(&first)==FR_OK && visible && returned==2);
#ifdef IMAGE_DRIVE_TEST
    /* Ejecting empty A: must never fall through to ATA slot zero (C:). */
    ata[0].name=strdup("sentinel-system-disk");
    CHECK(ejectdisk(0,true) && ata[0].name && !strcmp(ata[0].name,"sentinel-system-disk"));
    free(ata[0].name); ata[0].name=NULL;
    CHECK(image_media_begin(0)==4);
    CHECK(insertdisk(0,true,false,"4:/DIR/FLOPPY.IMG"));
    CHECK(!insertdisk(0,true,false,"4:/DIR/MISSING.IMG"));
    CHECK(fdd_is_inserted(0) && !strcmp(fdd_get_filename(0),"4:/DIR/FLOPPY.IMG"));
    CHECK(f_open(&missing,"4:/DIR/BAD.IMG",FA_CREATE_NEW|FA_WRITE)==FR_OK);
    CHECK(f_write(&missing,"x",1,&n)==FR_OK && f_close(&missing)==FR_OK);
    CHECK(!insertdisk(0,true,false,"4:/DIR/BAD.IMG") && fdd_is_inserted(0));
    CHECK(!insertdisk(1,true,false,"4:/DIR/FLOPPY.IMG"));
    CHECK(insertdisk(1,true,false,"4:/DIR/SECOND.IMG"));
    CHECK(image_media_end(4) && !visible);
    memset(data,0x67,sizeof(data)); CHECK(blk_pwrite(fdd_get_dev(0),512,data,512)==512);
    sync_fail=1; CHECK(!ejectdisk(0,true) && fdd_is_inserted(0) && !visible);
    sync_fail=0; CHECK(ejectdisk(0,true) && !visible);
    CHECK(ejectdisk(1,true) && visible);
#endif
    CHECK(image_media_begin(0)==4);
    CHECK(f_open(&first,"4:/DIR/FLOPPY.IMG",FA_READ)==FR_OK);
    CHECK(image_media_end(4)); live=0; int before=write_calls;
    CHECK(f_read(&first,data,512,&n)!=FR_OK);
    f_close(&first);
    CHECK(!image_media_owned(0) && !visible && write_calls==before);
    /* Unsupported filesystem plus a failed release must remain exclusive,
     * and an explicit retry must recover after the flush error clears. */
    live=visible=1;
    unsigned char mbr[512]; memcpy(mbr,bytes,512); memset(bytes,0,512);
    sync_fail=1;
    CHECK(image_media_available(0) && image_media_begin(0)<0);
    CHECK(image_media_owned(0) && !visible);
    memcpy(bytes,mbr,512); sync_fail=0;
    CHECK(image_media_begin(0)==4 && !visible);
    CHECK(image_media_end(4) && visible && !image_media_owned(0));
    puts("PASS: real FAT browse/open/RW, conflicting-open lock, failed-open rollback, two-image lease, last-close return, sync-error refusal, reconnect readback, removed-medium no writes");
    free(bytes); return 0;
}
