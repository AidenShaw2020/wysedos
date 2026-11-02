/* Validate the actual BAR2 data provider, including corrupt firmware EDID. */
#include "../../pc/edid.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n",__LINE__);return 1; } } while(0)
static void checksum(uint8_t *p) {
    unsigned s=0;p[127]=0;for(unsigned i=0;i<127;i++)s+=p[i];p[127]=-s;
}
int main(void) {
    uint8_t d[384]={0,255,255,255,255,255,255,0};
    checksum(d);CHECK(guest_edid_set(d,128));
    CHECK(guest_edid_read(0,4)==0xffffff00u);
    CHECK(!guest_edid_set(NULL,128) && !guest_edid_set(d,127));
    d[20]++;CHECK(!guest_edid_set(d,128));d[20]--;checksum(d);
    d[126]=2;checksum(d);d[128]=2;checksum(d+128);
    CHECK(guest_edid_set(d,sizeof(d)));CHECK(guest_edid_page()[126]==1);
    unsigned s=0;for(unsigned i=0;i<128;i++)s+=guest_edid_page()[i];CHECK(!(s&255));
    CHECK(guest_edid_page()[128]==2 && guest_edid_page()[256]==0);
    d[129]++;CHECK(guest_edid_set(d,256));CHECK(guest_edid_page()[126]==0 && guest_edid_page()[128]==0);
    CHECK(guest_edid_set(d,128) && guest_edid_page()[126]==0);
    CHECK(guest_edid_read(4095,4)==0xffffff00u && guest_edid_read(4096,4)==0xffffffffu);
    CHECK(!((uintptr_t)guest_edid_page()&4095));
    FILE *f=fopen("disks/386/vgabios.bin","rb");CHECK(f);
    uint8_t rom[39936];CHECK(fread(rom,1,sizeof(rom),f)==sizeof(rom));fclose(f);
    CHECK(!guest_vga_bind(rom,sizeof(rom)-1,16));
    CHECK(guest_vga_bind(rom,sizeof(rom),16));
    CHECK(rom[0x9894]==16 && rom[0x9895]==0);
    s=0;for(unsigned i=0;i<sizeof(rom);i++)s+=rom[i];CHECK(!(s&255));
    CHECK(!guest_vga_bind(rom,sizeof(rom),16));
    puts("PASS: EDID header/checksums, extension clamp/fallback, BAR bounds/alignment");return 0;
}
