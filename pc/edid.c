/* Read-only PCI VGA BAR2. SeaVGABIOS exports this through VBE/DDC 4F15h. */
#include "edid.h"
#include <string.h>
static uint8_t page[4096] __attribute__((aligned(4096)));
bool guest_edid_set(const uint8_t *data,unsigned len) {
    static const uint8_t magic[8]={0,255,255,255,255,255,255,0};
    if (!data || len<128 || memcmp(data,magic,8)) return false;
    unsigned sum=0;for (unsigned i=0;i<128;i++) sum+=data[i];
    if (sum&255) return false;
    unsigned blocks=1;
    if (data[126] && len>=256) {
        sum=0;for (unsigned i=128;i<256;i++) sum+=data[i];
        if (!(sum&255)) blocks=2;
    }
    memset(page,0,sizeof(page));memcpy(page,data,128*blocks);
    /* SeaVGABIOS can return two 128-byte blocks. Advertise only those. */
    if (page[126]!=blocks-1) {
        page[126]=blocks-1;page[127]=0;sum=0;
        for (unsigned i=0;i<127;i++) sum+=page[i];page[127]=-sum;
    }
    return true;
}
const uint8_t *guest_edid_page(void) { return page; }
/* This ROM is entered by the legacy BIOS with AX=0, instead of its PCI BDF.
 * Seed its existing VgaBDF variable so standard SeaVGABIOS reads BAR0/BAR2.
 * Match the supported ROM's instructions/data; never patch an unknown ROM. */
bool guest_vga_bind(uint8_t *rom,unsigned size,uint32_t bdf) {
    static const uint8_t read_bdf[]={0x2e,0x66,0x8b,0x0e,0x94,0x98};
    static const uint8_t pci_id[]={0x50,0x43,0x49,0x52,0x34,0x12,0x11,0x11};
    if (!rom || size!=39936 || bdf>65535 ||
        memcmp(rom+0x36b4,read_bdf,sizeof(read_bdf)) ||
        memcmp(rom+0x98a0,pci_id,sizeof(pci_id)) ||
        memcmp(rom+0x9894,"\xff\xff\xff\xff",4)) return false;
    memcpy(rom+0x9894,&bdf,4);
    unsigned sum=0;for (unsigned i=0;i<size;i++)sum+=rom[i];rom[size-1]-=sum;
    return true;
}
uint32_t guest_edid_read(unsigned off,unsigned size) {
    uint32_t v=0;for (unsigned i=0;i<size && i<4;i++)
        v|=(uint32_t)(off+i<4096?page[off+i]:255)<<(8*i);
    return v;
}
