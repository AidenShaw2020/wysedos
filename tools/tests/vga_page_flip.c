/* Native front/back page lifetime and fine panning, using the real VGA model.
 * clang -O2 -ffunction-sections -DBPP=32 -DWYSE_BUILD=1
 *   tools/tests/vga_page_flip.c -Wl,/opt:ref -o work/vga_page_flip.exe */
#include <stdint.h>
uint32_t get_uticks_fast(void);
#include "../../pc/vga.c"
static uint32_t now;
uint32_t get_uticks(void) { return now; }
uint32_t get_uticks_fast(void) { return now; }
uint32_t g_vga_flips;
void wyse_vga_surface(VGAState *s) { (void)s;abort(); }
void hwvga_fetch(VGAState *s) { (void)s;abort(); }
void hwvga_reg(uint16_t p,uint8_t i,uint8_t v) { (void)p;(void)i;(void)v;abort(); }
PCIDevice *pci_register_device(PCIBus *b,const char *n,int d,uint16_t v,
                              uint16_t id,uint8_t r,uint16_t c) {
    (void)b;(void)n;(void)d;(void)v;(void)id;(void)r;(void)c;abort();
}
void pci_register_bar(PCIDevice *d,unsigned int n,uint32_t s,int t,void *o,
                      PCIBarSetFunc *f) { (void)d;(void)n;(void)s;(void)t;(void)o;(void)f;abort(); }
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static void redraw(void *p,int x,int y,int w,int h) {
    (void)p;(void)x;(void)y;(void)w;(void)h;
}
static void mode(VGAState *s) {
    memset(s,0,sizeof *s);
    s->hw_mem=1;s->lat_valid=1;s->ar_index=0x20;
    s->gr[6]=5;s->sr[4]=6;s->sr[1]=9;
    s->cr[1]=39;s->cr[0x12]=0x8f;s->cr[7]=0x1f;
    s->cr[9]=0xc0;s->cr[0x13]=24;s->cr[0x17]=0xe3;s->cr[0x18]=0xff;
    s->retrace_phase=1;s->retrace_time=1000;
    for (int i=0;i<16;i++) s->ar[i]=i;
    s->palette[3]=63; /* index 1 = red */
    s->palette[7]=63; /* index 2 = green */
}
static int advance(VGAState *s,uint32_t t) { now=t;return vga_step(s); }
int main(void) {
    VGAState s;mode(&s);
    CHECK(!vga_native_planar_needs_software(&s)); /* Bash: 320 / virtual 384 */
    s.cr[0x13]=0x7c;
    CHECK(vga_native_planar_needs_software(&s)); /* Keen intro: virtual 1984 */
    s.cr[0x13]=0x15;
    CHECK(!vga_native_planar_needs_software(&s)); /* Keen game: virtual 336 */
    s.cr[1]=79;s.cr[0x13]=40;
    CHECK(vga_native_planar_needs_software(&s)); /* Windows VGA 640 */
    s.gr[5]=0x40;CHECK(!vga_native_planar_needs_software(&s)); /* Mode X */
    s.gr[5]=0;s.sr[4]=0x0e;CHECK(!vga_native_planar_needs_software(&s)); /* chain 4 */
    mode(&s);s.gr[6]=0x0e;s.cr[1]=79;
    CHECK(!vga_native_planar_needs_software(&s)); /* text */
    mode(&s);
    CHECK(vga_native_pages_disjoint(&s,0,0x8000));
    CHECK(vga_native_pages_disjoint(&s,0x2300,0x6300)); /* intro pages + scrolling */
    CHECK(vga_native_pages_disjoint(&s,0xf000,0x7000)); /* wrapped first page */
    CHECK(!vga_native_pages_disjoint(&s,0xf000,0x0500));
    CHECK(!vga_native_pages_disjoint(&s,0,1)); /* single-buffer scroll */
    CHECK(!vga_native_pages_disjoint(&s,0,0));
    s.gr[5]=0x40;CHECK(vga_native_pages_disjoint(&s,0,0x8000)); /* Mode X */
    s.gr[5]=0;s.cr[0x18]=129;s.cr[7]&=~0x10;s.cr[9]&=~0x40;
    CHECK(!vga_native_pages_disjoint(&s,0,0x8000));
    mode(&s);s.cr[0x17]&=~1;CHECK(!vga_native_pages_disjoint(&s,0,0x8000));
    mode(&s);s.cr[0x13]=255;CHECK(!vga_native_pages_disjoint(&s,0,0x8000));

    mode(&s);s.lat_pan=6;s.cr[0x0c]=0x80;
    CHECK(advance(&s,1000)==0); /* cannot publish old page or stale pan */
    CHECK(s.native_page_pending && s.lat_start==0x8000);
    /* Guest has seen retrace and starts erasing the old front buffer.
     * Only the completed new page may now feed the renderer. */
    s.vga_ram_size=0x40000;s.vga_ram=calloc(1,s.vga_ram_size);
    CHECK(s.vga_ram);
    for (unsigned a=0x8000;a<0x8000+9600;a++) s.vga_ram[4*a+1]=0xff;
    s.vga_ram[4*0x8000+1]=0x20; /* only pixel 2: distinguish pan 2 from 6/7 */
    s.ar[0x13]=2;
    CHECK(advance(&s,1833)==1 && !s.native_page_pending);
    CHECK((s.rend_frame&0xffffff)==0x028000);
    /* A later guest write must not change this frame's page/pan pair. */
    s.cr[0x0c]=0;s.ar[0x13]=7;
    uint32_t *pixels=calloc(640*480,sizeof *pixels);CHECK(pixels);
    FBDevice fb={0};fb.width=640;fb.height=480;fb.stride=2560;
    fb.fb_data=(uint8_t *)pixels;s.fb_dev=&fb;
    vga_graphic_refresh(&s,redraw,NULL,1);
    CHECK(pixels[40*640]==0x00ff00); /* intact new front, not erased old front */
    CHECK(pixels[40*640+1]==0x00ff00 && pixels[40*640+2]==0);
    /* No flip next period: still show that same complete front at the
     * earlier boundary, retaining its pan despite live-register changes. */
    s.cr[0x0c]=0x80;
    CHECK(advance(&s,16833)==0);
    CHECK(advance(&s,17666)==1);
    CHECK((s.rend_frame&0xffffff)==0x028000);
    free(pixels);free(s.vga_ram);

    mode(&s);s.lat_start=100;s.lat_pan=4;s.cr[0x0d]=101;
    CHECK(advance(&s,1000)==1); /* overlapping windows: old single-buffer timing */
    CHECK((s.rend_frame&0xffffff)==0x040064);
    s.ar[0x13]=5;CHECK(advance(&s,1833)==0);
    CHECK((s.rend_frame&0xffffff)==0x040064);
    mode(&s);s.hw_mem=0;s.cr[0x0c]=0x80;
    CHECK(advance(&s,1000)==0);s.ar[0x13]=3;
    CHECK(advance(&s,1833)==1 && s.lat_start==0x8000 && s.lat_pan==3);
    puts("PASS: native page lifetime, paired fine pan, wrap, split/banked/overlap fallback, software timing, virtual-pitch safety");
    return 0;
}
