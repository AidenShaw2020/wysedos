#include <string.h>
#include "installer_upgrade.h"
#include "native_disk.h"

static uint32_t layout_crc(const void *data, unsigned size) {
    const uint8_t *p=data; uint32_t crc=~0u;
    while (size--) { crc^=*p++; for (int i=0;i<8;i++) crc=(crc>>1)^(0xedb88320u&-(crc&1)); }
    return ~crc;
}
static uint64_t u64(const void *p) { uint64_t v; memcpy(&v,p,8); return v; }
static uint32_t u32(const void *p) { uint32_t v; memcpy(&v,p,4); return v; }

bool installer_read_layout(EFI_BLOCK_IO_PROTOCOL *disk, WyseInstallLayout *out) {
    static const EFI_GUID esp={0xc12a7328,0xf81f,0x11d2,{0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b}};
    static const EFI_GUID guest=WYSE_GUEST_TYPE, zero={0};
    static uint8_t h[512] __attribute__((aligned(4096)));
    static uint8_t entries[16384] __attribute__((aligned(4096)));
    if (!disk || !disk->Media || !out) return false;
    const EFI_BLOCK_IO_MEDIA *m=disk->Media;
    if (!m->MediaPresent || m->ReadOnly || m->LogicalPartition || m->RemovableMedia ||
        m->BlockSize!=512 || m->LastBlock<2048) return false;
    if (EFI_ERROR(bio_read(disk,1,1,h)) || memcmp(h,"EFI PART",8)) return false;
    uint32_t size=u32(h+12), saved=u32(h+16);
    if (size<92 || size>512 || u64(h+24)!=1 || u64(h+32)!=m->LastBlock) return false;
    memset(h+16,0,4);
    if (layout_crc(h,size)!=saved) return false;
    uint64_t first=u64(h+40),last=u64(h+48),table=u64(h+72);
    uint32_t count=u32(h+80),entry_size=u32(h+84);
    if (first<34 || first>last || last>m->LastBlock-33 || count<2 || count>128 || entry_size!=128 ||
        table<2 || table>=first || (count*128u+511)/512>first-table) return false;
    if (EFI_ERROR(bio_read(disk,table,(count*128u+511)/512,entries)) ||
        layout_crc(entries,count*128u)!=u32(h+88)) return false;
    WyseInstallLayout found={0};
    for (unsigned i=0;i<count;i++) {
        const uint8_t *e=entries+i*128;
        if (!memcmp(e,&zero,16)) continue;
        uint64_t a=u64(e+32),b=u64(e+40);
        if (a<first || a>b || b>last) return false;
        for (unsigned j=0;j<i;j++) {
            const uint8_t *o=entries+j*128;
            if (!memcmp(o,&zero,16)) continue;
            if (a<=u64(o+40) && u64(o+32)<=b) return false;
        }
        if (!memcmp(e,&esp,16)) {
            if (found.esp_count) return false;
            found.esp_first=a; found.esp_count=b-a+1;
        } else if (!memcmp(e,&guest,16)) {
            if (found.guest_count) return false;
            found.guest_first=a; found.guest_count=b-a+1;
        }
    }
    if (found.esp_first<2048 || found.esp_count<8192 || found.esp_first%128 || found.esp_count%128 ||
        found.guest_first<2048 || found.guest_first%128 || found.guest_count%128 ||
        found.guest_count<=2048 || found.guest_count>0x10000000ull) return false;
    *out=found;
    return true;
}
