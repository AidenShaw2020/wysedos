#include <stdio.h>
#include <string.h>
#include "native_disk.h"
#include "blkcache.h"
extern EFI_BOOT_SERVICES *gBS;
static const EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static const EFI_GUID guest_type = WYSE_GUEST_TYPE;
typedef struct { EFI_BLOCK_IO_PROTOCOL *bio; uint64_t first, count; uint32_t media; } native_t;
static native_t disk;

static uint32_t crc(const void *p, unsigned n) {
    const uint8_t *b = p; uint32_t c = ~0u;
    while (n--) { c ^= *b++; for (int k=0;k<8;k++) c=(c>>1)^(0xedb88320u & -(c&1)); }
    return ~c;
}
static int locate(void) {
    uint64_t n; EFI_HANDLE *hs;
    if (disk.bio) return 1;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&bio_guid,NULL,&n,&hs))) return 0;
    static uint8_t hdr[512] __attribute__((aligned(4096)));
    static uint8_t entries[16384] __attribute__((aligned(4096)));
    for (uint64_t i=0;i<n;i++) {
        EFI_BLOCK_IO_PROTOCOL *bio;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i],&bio_guid,(void **)&bio))) continue;
        EFI_BLOCK_IO_MEDIA *m=bio->Media;
        if (m->LogicalPartition || m->RemovableMedia || !m->MediaPresent || m->BlockSize!=512 ||
            bio_path_has(bio_device_path(hs[i]),DP_MESSAGING,DP_MSG_USB)) continue;
        if (EFI_ERROR(bio_read(bio,1,1,hdr)) || memcmp(hdr,"EFI PART",8)) continue;
        uint32_t size=*(uint32_t *)(hdr+12), saved=*(uint32_t *)(hdr+16);
        if (size<92 || size>512) continue;
        memset(hdr+16,0,4);
        if (crc(hdr,size)!=saved) continue;
        uint64_t table=*(uint64_t *)(hdr+72), first=*(uint64_t *)(hdr+40), last=*(uint64_t *)(hdr+48);
        uint32_t num=*(uint32_t *)(hdr+80), es=*(uint32_t *)(hdr+84);
        if (!num || num>128 || es!=128 || first<34 || first>last ||
            m->LastBlock<33 || last>m->LastBlock-33 || table<2 || table>m->LastBlock ||
            (num*128u+511)/512>m->LastBlock+1-table) continue;
        if (EFI_ERROR(bio_read(bio,table,(num*128u+511)/512,entries)) ||
            crc(entries,num*128u)!=*(uint32_t *)(hdr+88)) continue;
        for (unsigned j=0;j<num;j++) {
            uint8_t *e=entries+j*128;
            if (memcmp(e,&guest_type,16)) continue;
            uint64_t a=*(uint64_t *)(e+32), b=*(uint64_t *)(e+40);
            if (a<first || a<2048 || b>last || b>m->LastBlock || a>b ||
                b-a+1>0x10000000ull || a%128 || (b-a+1)%128) continue;
            /* Reject overlapping GPT ranges, including the firmware partition. */
            int overlap=0;
            for (unsigned k=0;k<num;k++) if (k!=j) {
                uint8_t *other=entries+k*128; EFI_GUID zero={0};
                if (!memcmp(other,&zero,16)) continue;
                uint64_t c=*(uint64_t *)(other+32), d=*(uint64_t *)(other+40);
                if (c<=b && d>=a) overlap=1;
            }
            if (overlap || disk.bio) continue;
            disk=(native_t){bio,a,b-a+1,m->MediaId};
        }
    }
    gBS->FreePool(hs);
    return disk.bio!=NULL;
}
static int seek_native(BlockDev *d,uint64_t off) {
    native_t *h=d->ctx;
    if ((off&511) || off>h->count*512) return -1;
    d->pos=off; return 0;
}
static int transfer(BlockDev *d,void *buf,unsigned len,int write) {
    native_t *h=d->ctx; uint64_t bytes=h->count*512;
    /* Subtraction avoids wraparound. Reject the ENTIRE request before I/O. */
    if ((d->pos&511) || (len&511) || d->pos>bytes || len>bytes-d->pos ||
        !h->bio->Media->MediaPresent || h->bio->Media->MediaId!=h->media) return -1;
    if (!len) return 0;
    uint64_t lba=h->first+d->pos/512;
    EFI_STATUS st=write ? blkcache_write(h->bio,lba,len/512,buf) : blkcache_read(h->bio,lba,len/512,buf);
    if (EFI_ERROR(st)) return -1;
    d->pos+=len; return (int)len;
}
static int read_native(BlockDev *d,void *buf,unsigned n) { return transfer(d,buf,n,0); }
static int write_native(BlockDev *d,const void *buf,unsigned n) { return transfer(d,(void *)buf,n,1); }
static uint64_t size_native(BlockDev *d) { return ((native_t *)d->ctx)->count*512; }
static int sync_native(BlockDev *d) { return EFI_ERROR(blkcache_flush(((native_t *)d->ctx)->bio))?-1:0; }
int native_disk_bind(BlockDev *d) {
    if (!d || !locate()) return 0;
    memset(d,0,sizeof(*d)); d->ctx=&disk; d->read=read_native; d->write=write_native;
    d->seek=seek_native; d->size=size_native; d->sync=sync_native; d->readonly=disk.bio->Media->ReadOnly;
    printf("native: guest disk %llu MB, physical LBA %llu..%llu; ESP/GPT inaccessible\n",
        (unsigned long long)(disk.count/2048),(unsigned long long)disk.first,
        (unsigned long long)(disk.first+disk.count-1));
    return 1;
}
/* Read-only diagnostics: invalid requests must fail without touching storage. */
void native_disk_test(void) {
    BlockDev d; uint8_t b[512];
    if (!native_disk_bind(&d)) { printf("native test: no disk\n"); return; }
    uint64_t size=blk_size(&d); int ok=1;
    ok &= blk_seek(&d,size+512)<0;
    ok &= blk_seek(&d,UINT64_MAX)<0;
    ok &= blk_seek(&d,1)<0;
    ok &= blk_seek(&d,size)==0 && blk_read(&d,b,512)<0 && blk_write(&d,b,512)<0;
    ok &= blk_seek(&d,size-512)==0 && blk_read(&d,b,1024)<0 && blk_write(&d,b,1024)<0;
    ok &= blk_seek(&d,0)==0 && blk_read(&d,b,511)<0;
    ok &= blk_seek(&d,0)==0 && blk_read(&d,b,512)==512;
    printf("native test: bounds %s, guest MBR signature %02x%02x\n",ok?"PASS":"FAIL",b[510],b[511]);
    uint32_t start=0,count=0;
    memcpy(&start,b+454,4); memcpy(&count,b+458,4);
    int mbr=b[446]==0x80 && b[450]==0x0c && start==2048 && (uint64_t)start+count==disk.count;
    int fat=blk_pread(&d,(uint64_t)start*512,b,512)==512 && !memcmp(b+82,"FAT32   ",8) &&
        b[510]==0x55 && b[511]==0xaa && *(uint32_t *)(b+28)==start && *(uint32_t *)(b+32)==count;
    printf("native test: MBR %s, FAT32/BPB %s, C: %u sectors (%u MiB), hidden %u, cluster %u bytes\n",
        mbr?"PASS":"FAIL",fat?"PASS":"FAIL",count,count/2048,*(uint32_t *)(b+28),b[13]*512u);
}
