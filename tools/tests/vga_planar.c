/* Exercise the actual VGA latch operations and sequencer blanking.
 * clang -O2 -ffunction-sections -DBPP=32 tools/tests/vga_planar.c
 *       -Wl,/opt:ref -o work/vga_planar_test.exe */
#include <stdint.h>
uint32_t get_uticks_fast(void);
#include "../../pc/vga.c"
uint32_t get_uticks(void) { return 0; }
uint32_t get_uticks_fast(void) { return 0; }
PCIDevice *pci_register_device(PCIBus *b,const char *name,int devfn,
                               uint16_t vendor,uint16_t device,uint8_t rev,uint16_t cls) {
    (void)b;(void)name;(void)devfn;(void)vendor;(void)device;(void)rev;(void)cls;
    abort();
}
void pci_register_bar(PCIDevice *d,unsigned int num,uint32_t size,int type,
                      void *opaque,PCIBarSetFunc *set) {
    (void)d;(void)num;(void)size;(void)type;(void)opaque;(void)set; abort();
}
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n",__LINE__); return 1; } } while (0)
static int redraws;
static void redraw(void *o,int x,int y,int w,int h) { (void)o;(void)x;(void)y;(void)w;(void)h;redraws++; }
int main(void) {
    uint32_t pixels[4] = {0x78563412,0,0,0};
    VGAState s = {0};
    s.vga_ram=(uint8_t *)pixels; s.vga_ram_size=sizeof(pixels);
    s.gr[6]=5; s.gr[4]=2; s.gr[8]=0xff; s.sr[2]=0xf;
    /* Reading one plane also loads all four latches for a later copy. */
    CHECK(vga_mem_read(&s,0)==0x56 && s.latch==0x78563412);
    s.gr[5]=1; vga_mem_write(&s,1,0);
    CHECK(pixels[1]==pixels[0]);
    /* Masked planar writes preserve the other bits through those latches. */
    s.gr[5]=0; s.gr[8]=0x0f; vga_mem_write(&s,2,0xab);
    CHECK(pixels[2]==0x7b5b3b1b);
    s.ar_index=0x20; CHECK(vga_get_mode(&s)==2);
    s.sr[1]=VGA_SR01_SCREEN_OFF; CHECK(vga_get_mode(&s)==0);
    CHECK(vga_get_graphics_mode(&s,NULL,NULL)==0);
    s.gr[6]=0; CHECK(vga_get_mode(&s)==0);
    uint32_t surface[64]; memset(surface,0xff,sizeof(surface));
    FBDevice fb={0};fb.width=8;fb.height=8;fb.stride=32;fb.fb_data=(uint8_t *)surface;
    s.fb_dev=&fb;s.graphic_mode=2;
    vga_refresh(&s,redraw,NULL,0);
    CHECK(s.graphic_mode==0 && redraws==1);
    for (unsigned i=0;i<64;i++) CHECK(surface[i]==0);
    s.sr[1]=0; CHECK(vga_get_mode(&s)==1);
    s.graphic_mode=1; s.gr[6]=0x04; s.sr[2]=0x04;
    uint32_t skips0=g_vga_refresh_skips;
    vga_refresh(&s,redraw,NULL,0);
    CHECK(g_vga_refresh_skips==skips0+1 && redraws==1);
    puts("PASS: planar latch copy, masked write, text/graphics sequencer blanking");
    return 0;
}
