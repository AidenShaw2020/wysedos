/* Host regression test using the actual backend and a mock physical device.
 * clang -Ipc tools/tests/native_bounds.c -o work/native_bounds.exe */
#include "../../efi/platform/native_disk.c"
#include <stdlib.h>
EFI_BOOT_SERVICES *gBS;
static unsigned accesses;
static uint64_t last_lba;
static unsigned char physical[32*512];
EFI_STATUS blkcache_read(EFI_BLOCK_IO_PROTOCOL *b,uint64_t l,uint64_t n,void *p) {
    (void)b; accesses++; last_lba=l;
    if (l+n>32) abort(); memcpy(p,physical+l*512,n*512); return 0;
}
EFI_STATUS blkcache_write(EFI_BLOCK_IO_PROTOCOL *b,uint64_t l,uint64_t n,const void *p) {
    (void)b; accesses++; last_lba=l;
    if (l+n>32) abort(); memcpy(physical+l*512,p,n*512); return 0;
}
EFI_STATUS blkcache_flush(EFI_BLOCK_IO_PROTOCOL *b) { (void)b; return 0; }
EFI_STATUS bio_read(EFI_BLOCK_IO_PROTOCOL *b,uint64_t l,uint64_t n,void *p) {
    return blkcache_read(b,l,n,p);
}
EFI_DEVICE_PATH *bio_device_path(EFI_HANDLE h) { (void)h; return NULL; }
bool bio_path_has(EFI_DEVICE_PATH *p,uint8_t t,uint8_t s) { (void)p;(void)t;(void)s;return false; }
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    EFI_BLOCK_IO_MEDIA media={0}; EFI_BLOCK_IO_PROTOCOL bio={0};
    media.MediaPresent=true; media.MediaId=42; bio.Media=&media;
    disk=(native_t){&bio,8,16,42};
    BlockDev d; CHECK(native_disk_bind(&d));
    unsigned char buf[1024]; memset(buf,0xa5,sizeof(buf)); memset(physical,0x7b,sizeof(physical));
    CHECK(blk_size(&d)==8192);
    CHECK(blk_pwrite(&d,0,buf,512)==512 && last_lba==8);
    CHECK(blk_pwrite(&d,8192-512,buf,512)==512 && last_lba==23);
    CHECK(blk_pread(&d,0,buf,512)==512 && last_lba==8);
    unsigned calls=accesses;
    CHECK(blk_seek(&d,8192)==0);
    CHECK(blk_write(&d,buf,512)<0 && blk_read(&d,buf,512)<0);
    CHECK(blk_seek(&d,8192-512)==0);
    CHECK(blk_write(&d,buf,1024)<0 && blk_read(&d,buf,1024)<0);
    CHECK(blk_seek(&d,UINT64_MAX)<0 && blk_seek(&d,8193)<0 && blk_seek(&d,1)<0);
    CHECK(blk_seek(&d,0)==0 && blk_write(&d,buf,511)<0);
    d.pos=UINT64_MAX-511; CHECK(blk_write(&d,buf,1024)<0);
    media.MediaId++; CHECK(blk_seek(&d,0)==0 && blk_read(&d,buf,512)<0);
    CHECK(accesses==calls);
    for (unsigned i=0;i<8*512;i++) CHECK(physical[i]==0x7b);
    for (unsigned i=24*512;i<sizeof(physical);i++) CHECK(physical[i]==0x7b);
    puts("PASS: first/last sector mapping, complete range rejection, overflow, alignment, media changes, firmware/GPT sentinels");
    return 0;
}
