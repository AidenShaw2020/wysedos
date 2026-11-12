/* A guest write between row capture and conversion must not poison scanout. */
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "../../pc/cpu_speed_policy.h"
static unsigned char *live_row;
static int mutate_after_copy;
static void *capture_copy(void *dst, const void *src, size_t bytes);
uint32_t get_uticks_fast(void);
#define memcpy capture_copy
#include "../../pc/vga.c"
#undef memcpy
static void *capture_copy(void *dst, const void *src, size_t bytes)
{
    void *result = memcpy(dst, src, bytes);
    if (mutate_after_copy && src == live_row && bytes == 16) {
        mutate_after_copy = 0;
        memset(live_row, 0, bytes);
    }
    return result;
}
uint32_t get_uticks(void) { return 0; }
uint32_t get_uticks_fast(void) { return 0; }
PCIDevice *pci_register_device(PCIBus *b,const char *n,int d,uint16_t v,
                              uint16_t id,uint8_t r,uint16_t c) {
    (void)b;(void)n;(void)d;(void)v;(void)id;(void)r;(void)c;abort();
}
void pci_register_bar(PCIDevice *d,unsigned int n,uint32_t s,int t,void *o,
                      PCIBarSetFunc *f) { (void)d;(void)n;(void)s;(void)t;(void)o;(void)f;abort(); }
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static void redraw(void *o,int x,int y,int w,int h) {
    (void)o;(void)x;(void)y;(void)w;(void)h;
}
int main(void)
{
    FBDevice caps_fb = {0};
    VGAState caps_vga = {0};
    caps_fb.width = 640; caps_fb.height = 480;
    caps_vga.fb_dev = &caps_fb;
    caps_vga.vbe_regs[VBE_DISPI_INDEX_ENABLE] = VBE_DISPI_GETCAPS;
    caps_vga.vbe_index = VBE_DISPI_INDEX_XRES;
    CHECK(vbe_read(&caps_vga, 2) == 640);
    caps_fb.max_width = 1600; caps_fb.max_height = 1200;
    CHECK(vbe_read(&caps_vga, 2) == 1600);
    caps_vga.vbe_index = VBE_DISPI_INDEX_YRES;
    CHECK(vbe_read(&caps_vga, 2) == 1200);
    caps_fb.width = 800; caps_fb.height = 600;
    CHECK(vbe_read(&caps_vga, 2) == 1200);
    caps_vga.vbe_regs[VBE_DISPI_INDEX_ENABLE] = VBE_DISPI_ENABLED;
    caps_vga.vbe_regs[VBE_DISPI_INDEX_YRES] = 600;
    CHECK(vbe_read(&caps_vga, 2) == 600);
    const unsigned depths[] = {8,15,16,32};
    for (unsigned i=0;i<4;i++) {
        uint8_t vram[64], original[64];
        uint32_t surface[64] = {0}, expected[64] = {0};
        FBDevice fb={0};VGAState s={0};
        unsigned bpp=depths[i], bytes=bpp==8?1:bpp==32?4:2;
        unsigned width=16/bytes;
        fb.width=width;fb.height=4;fb.stride=width*4;fb.fb_data=(uint8_t *)surface;
        s.fb_dev=&fb;s.ar_index=0x20;s.gr[6]=1;s.sr[4]=8;s.cr[0x17]=3;
        s.vga_ram=vram;s.vga_ram_size=sizeof vram;s.direct_vram=true;
        s.vbe_regs[VBE_DISPI_INDEX_ENABLE]=VBE_DISPI_ENABLED|VBE_DISPI_LFB_ENABLED;
        s.vbe_regs[VBE_DISPI_INDEX_XRES]=width;s.vbe_regs[VBE_DISPI_INDEX_YRES]=4;
        s.vbe_regs[VBE_DISPI_INDEX_BPP]=bpp;s.vbe_line_offset=16;
        for (unsigned p=0;p<sizeof vram;p++) original[p]=(uint8_t)(p+1);
        for (unsigned p=0;p<sizeof s.palette;p++) s.palette[p]=(uint8_t)(p*7);
        memcpy(vram,original,sizeof vram);
        vbe_update_vgaregs(&s);
        vga_refresh(&s,redraw,NULL,1);
        memcpy(expected,surface,sizeof surface);
        live_row=vram;mutate_after_copy=1;
        vga_refresh(&s,redraw,NULL,1);
        CHECK(!mutate_after_copy);
        CHECK(!memcmp(surface,expected,width*4));
        /* Guest restores the row before the next refresh. The shadow now
         * matches live memory, so that next refresh legitimately skips it. */
        memcpy(vram,original,sizeof vram);
        vga_refresh(&s,redraw,NULL,0);
        CHECK(!memcmp(surface,expected,sizeof surface));
        CHECK(cpu_speed_effective(133, 1, vbe_enabled(&s), 0, 0) == 0);
        vga_reset_display(&s);
        CHECK(!vbe_enabled(&s));
        CHECK(s.vbe_index == VBE_DISPI_INDEX_ID);
        CHECK(cpu_speed_effective(133, 1, vbe_enabled(&s), 0, 0) == 133);
        free(s.vbe_shadow);
    }
    puts("PASS: VBE 8/15/16/32-bit scanout uses the captured row during guest writes");
    return 0;
}
