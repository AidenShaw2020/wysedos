/* clang -O2 -Ipc tools/tests/installer_layout.c -o work/installer_layout.exe */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../efi/platform/installer_layout.c"
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static uint8_t media[32768*512];
static EFI_BLOCK_IO_MEDIA geometry;
static EFI_BLOCK_IO_PROTOCOL device;
static unsigned reads; static int read_fail;
EFI_STATUS bio_read(EFI_BLOCK_IO_PROTOCOL *d,uint64_t l,uint64_t n,void *p) {
    CHECK(d==&device && l<=32768 && n<=32768-l); reads++;
    if (read_fail) return EFI_DEVICE_ERROR;
    memcpy(p,media+l*512,n*512); return EFI_SUCCESS;
}
static void put32(void *p,uint32_t v) { memcpy(p,&v,4); }
static void put64(void *p,uint64_t v) { memcpy(p,&v,8); }
static void seal(void) {
    uint8_t *h=media+512;
    put32(h+88,layout_crc(media+1024,16384)); put32(h+16,0);
    put32(h+16,layout_crc(h,92));
}
static void fixture(void) {
    memset(media,0,sizeof(media)); memset(&geometry,0,sizeof(geometry)); memset(&device,0,sizeof(device));
    geometry.MediaPresent=1; geometry.BlockSize=512; geometry.LastBlock=32767;
    device.Media=&geometry; reads=0; read_fail=0;
    uint8_t *h=media+512; memcpy(h,"EFI PART",8); put32(h+8,0x10000); put32(h+12,92);
    put64(h+24,1); put64(h+32,32767); put64(h+40,34); put64(h+48,32734);
    put64(h+72,2); put32(h+80,128); put32(h+84,128);
    EFI_GUID esp={0xc12a7328,0xf81f,0x11d2,{0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b}};
    EFI_GUID guest=WYSE_GUEST_TYPE;
    memcpy(media+1024,&esp,16); put64(media+1024+32,2048); put64(media+1024+40,10239);
    memcpy(media+1152,&guest,16); put64(media+1152+32,10240); put64(media+1152+40,28671);
    seal();
}
static void rejected(void) {
    WyseInstallLayout out={11,22,33,44},before=out;
    CHECK(!installer_read_layout(&device,&out)); CHECK(!memcmp(&out,&before,sizeof(out)));
}
int main(void) {
    fixture(); WyseInstallLayout layout;
    CHECK(installer_read_layout(&device,&layout) && reads==2);
    CHECK(layout.esp_first==2048 && layout.esp_count==8192 &&
          layout.guest_first==10240 && layout.guest_count==18432);
    fixture(); media[512]^=1; rejected();
    fixture(); media[512+16]^=1; rejected();
    fixture(); media[1024+7]^=1; rejected();
    fixture(); put64(media+512+32,32766); seal(); rejected();
    fixture(); put32(media+512+80,129); seal(); rejected();
    fixture(); put64(media+512+72,32760); seal(); rejected();
    fixture(); put64(media+1152+32,8192); seal(); rejected(); /* Overlap. */
    fixture(); put64(media+1024+40,10240); seal(); rejected(); /* Misaligned and overlaps. */
    fixture(); memset(media+1152,0,16); seal(); rejected();
    fixture(); memcpy(media+1280,media+1024,128); seal(); rejected(); /* Duplicate ESP. */
    fixture(); put64(media+1152+40,32760); seal(); rejected();
    fixture(); geometry.ReadOnly=1; rejected(); CHECK(!reads);
    fixture(); geometry.RemovableMedia=1; rejected(); CHECK(!reads);
    fixture(); geometry.LogicalPartition=1; rejected(); CHECK(!reads);
    fixture(); read_fail=1; rejected();
    puts("PASS: read-only existing-layout qualification; GPT/header/table CRCs, ranges, overlaps, duplicates, alignment, media flags and I/O failure");
    return 0;
}
