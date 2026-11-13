/* Run tools/tests/hwvga_transfer.ps1. No hardware, firmware build or deployment.
 * Actual hw_update/fetch/store bodies are extracted on each run. Register I/O,
 * aperture writes, EPT and display-core hold are host seams. pc/vga.c is real.
 * Expected counterexamples PASS here: this is an investigation, not a fix. */
#include <stdint.h>
uint32_t get_uticks_fast(void);
#include "../../pc/vga.c"
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define VGA_BYTES 0x40000u
static uint32_t physical[0x10000], model[0x10000], saved[0x10000];
static const uint32_t *win=physical;
uint64_t g_hwvga_fetches,g_hwvga_fetch_tsc;
uint32_t g_vga_flips;
static VGAState state;
static struct { VGAState *vga; } pc_stub={&state};
static typeof(pc_stub) *g_pc=&pc_stub;
static struct { int unused; } vm;
static bool hw_on,text_trapped,ram_13h,ram_planar;
static int g_hwvga_on=1,safe_planar=1;
enum { WIN_EMU,WIN_RAM,WIN_HW };
static int win_mode;
static unsigned fetch_before, stores, maps;
static uint8_t bus_sr[8],bus_gr[16],bus_msr,sri,gri;
static void flush_window(void) {}
static void outb(uint16_t p,uint8_t v) {
    if(p==0x3c4) sri=v;
    else if(p==0x3c5) bus_sr[sri]=v;
    else if(p==0x3ce) gri=v;
    else if(p==0x3cf) bus_gr[gri]=v;
    else if(p==0x3c2) bus_msr=v;
    else CHECK(0);
}
static void bus_write(unsigned a,uint8_t value) {
    /* Transfer funcs explicitly select linear planar, mode 0, full bitmask. */
    CHECK(bus_sr[4]==6 && bus_gr[5]==0 && bus_gr[6]==5 && bus_gr[8]==255);
    CHECK(!bus_gr[0] && !bus_gr[1] && !bus_gr[3] && a<0x10000);
    for(unsigned p=0;p<4;p++) if(bus_sr[2]&(1u<<p))
        ((uint8_t *)physical)[a*4+p]=value;
    stores++;
}
void hwvga_reg(uint16_t p,uint8_t i,uint8_t v) { outb(p,i);outb(p+1,v); }
void hwvga_sync(VGAState *s) {
    bus_msr=s->msr;memcpy(bus_sr,s->sr,8);memcpy(bus_gr,s->gr,16);
}
static void io_pass_off(void) {}
static void plane_release(void) {}
void pc_vga_hold(int hold) { (void)hold; }
static void vm_map(void *v,unsigned a,void *p,unsigned n,bool wr) {
    (void)v;(void)a;(void)p;(void)n;(void)wr;maps++;
}
static void vm_map_mmio(void *v,unsigned a,unsigned b,unsigned n) {
    (void)v;(void)a;(void)b;(void)n;maps++;
}
void hwvga_fetch(VGAState *s);
static void transition_fetch_proposal(VGAState *s,int want) {
    /* TEST-ONLY proposal: match ram_planar's exact producer predicate. Do not
     * change display fetch: existing text backing still needs compression. */
    if(want==WIN_EMU && !vga_vbe_on(s) && !(s->sr[4]&8) && !(s->gr[6]&2)) {
        CHECK(s->vga_ram_size>=VGA_BYTES);flush_window();memcpy(s->vga_ram,win,VGA_BYTES);
    } else hwvga_fetch(s);
}
#include "../../work/hwvga-transfer/actual_transfer.h"
void wyse_vga_surface(VGAState *s) { (void)s; }
uint32_t get_uticks(void) { return 1; }
uint32_t get_uticks_fast(void) { return 1; }
PCIDevice *pci_register_device(PCIBus *b,const char *n,int d,uint16_t v,uint16_t i,uint8_t r,uint16_t c) {
    (void)b;(void)n;(void)d;(void)v;(void)i;(void)r;(void)c;abort();
}
void pci_register_bar(PCIDevice *d,unsigned n,uint32_t z,int t,void *o,PCIBarSetFunc *f) {
    (void)d;(void)n;(void)z;(void)t;(void)o;(void)f;abort();
}
static void reset(void) {
    memset(&state,0,sizeof state);memset(physical,0,sizeof physical);memset(model,0,sizeof model);
    state.vga_ram=(uint8_t *)model;state.vga_ram_size=VGA_BYTES;
    state.msr=0x67;state.sr[4]=2;state.sr[2]=3;state.gr[6]=0x0e;state.gr[5]=0x10;state.gr[8]=255;
    state.cr[1]=79;state.cr[0x13]=40;state.cr[9]=15;state.cr[0x12]=0x8f;
    state.cr[7]=0x12;state.cr[0x18]=255;state.cr[0xa]=0x20;
    state.sr[1]=1;state.ar_index=0x20;state.last_width=~0u;
    for(unsigned i=0;i<16;i++) { state.ar[i]=i;state.palette[i*3]=state.palette[i*3+1]=state.palette[i*3+2]=i*4; }
    state.palette[7*3]=state.palette[7*3+1]=state.palette[7*3+2]=63;
    for(unsigned i=0;i<0x10000;i++) physical[i]=0xa55a0000u|((i*37+11)&0xffff);
    win_mode=WIN_HW;hw_on=true;text_trapped=true;state.hw_mem=1;
    ram_13h=ram_planar=false;g_hwvga_on=1;stores=maps=0;
    fetch_before=(unsigned)g_hwvga_fetches;hwvga_sync(&state);
}
static void trace_reg(unsigned sr,unsigned value) { state.sr[sr]=value;hwvga_sync(&state);hw_update(false); }
static void trace_gr(unsigned gr,unsigned value) { state.gr[gr]=value;hwvga_sync(&state);hw_update(false); }
static void redraw(void *o,int x,int y,int w,int h) { (void)o;(void)x;(void)y;(void)w;(void)h; }
static uint32_t surface[640*400], expected[640*400];
static FBDevice fb;
static void surface_setup(void) {
    memset(&fb,0,sizeof fb);fb.width=640;fb.height=400;fb.stride=640*4;fb.fb_data=(uint8_t *)surface;
    state.fb_dev=&fb;state.graphic_mode=1;state.cursor_blink_time=1000000;
    for(unsigned i=0;i<256*32;i++) ((uint8_t *)physical)[i*4+2]=0;
    for(unsigned r=0;r<16;r++) {
        ((uint8_t *)physical)[('O'*32+r)*4+2]=0xff;
        ((uint8_t *)physical)[('N'*32+r)*4+2]=0x81;
    }
    /* All 2000 cells initially old; text CPU address 2*i -> physical dword 2*i. */
    for(unsigned i=0;i<2000;i++) physical[2*i]=(physical[2*i]&0xffff0000u)|0x074f;
    hwvga_fetch(&state);vga_text_refresh(&state,redraw,NULL,1);
    CHECK(surface[0]!=0 && state.last_palette[7]!=0);
}
static void unchanged_native_trace(void) {
    reset();surface_setup();
    unsigned before=(unsigned)g_hwvga_fetches;
    trace_reg(4,7);trace_gr(5,0);trace_gr(6,4);trace_gr(4,2);trace_reg(2,4);
    /* Real font-plane model writes must not touch character/attribute planes. */
    hwvga_fetch(&state);
    CHECK((model[123]&0xffff)==(physical[246]&0xffff));
    uint32_t old=physical[123];physical[123]=(old&0xff00ffffu)|0x003c0000;
    trace_reg(4,3);trace_gr(6,0x0e);trace_gr(4,0);trace_gr(5,0x10);trace_reg(2,3);trace_reg(4,2);
    CHECK(win_mode==WIN_HW && !stores && g_hwvga_fetches==before+1);
    /* Fresh output changes 160 cells; untouched menu remains across the surface. */
    for(unsigned i=0;i<160;i++) physical[2*i]=(physical[2*i]&0xffff0000u)|0x074e;
    vga_refresh(&state,redraw,NULL,0);
    memcpy(expected,surface,sizeof surface);
    CHECK(model[159]%65536==0x074e && model[160]%65536==0x074f && model[1999]%65536==0x074f);
    /* Full repaint must agree pixel-for-pixel: no stale-cell cache/margin cause. */
    memset(surface,0xcc,sizeof surface);vga_refresh(&state,redraw,NULL,1);
    CHECK(!memcmp(surface,expected,sizeof surface));
    CHECK(surface[0]!=surface[1] && surface[32*640]==surface[32*640+1]);
    puts("PASS native BIOS trace: HW throughout, 0 transition transfers; full 640x400 surface = 160 fresh + 1840 untouched old cells; full repaint identical");
}
static void transfer_controls(void) {
    reset();state.gr[6]=5;hwvga_fetch(&state);memcpy(saved,physical,sizeof saved);
    memset(physical,0,sizeof physical);hwvga_store_planar((uint8_t *)model);
    CHECK(!memcmp(physical,saved,sizeof saved));
    reset();state.sr[4]=0x0e;state.gr[6]=5;hwvga_fetch(&state);memcpy(saved,model,0x10000);
    memset(physical,0,sizeof physical);hwvga_store_chain4((uint8_t *)model);hwvga_fetch(&state);
    CHECK(!memcmp(model,saved,0x10000));
    puts("PASS actual transfers: planar 256KiB roundtrip; chain4 64KiB roundtrip");
    for(unsigned page=0;page<2;page++) {
        reset();state.msr=page?0x47:0x67;hwvga_fetch(&state);
        CHECK((model[1]&0xffff)==(physical[2+page]&0xffff));
        CHECK((model[1]&0xffff0000u)==(physical[1]&0xffff0000u));
        CHECK(model[0x8000]==physical[0x8000]);
    }
    puts("PASS odd/even fetch: both Misc page values, planar fonts and upper 128KiB");
}
static void concrete_handoff_loss(void) {
    reset();state.gr[6]=4;state.sr[4]=7;memcpy(saved,physical,sizeof saved);
    /* Counterexample deliberately needs hardware availability toggle, absent
     * from supplied trace. Text-mode fetch compresses even when SR4 disables OE. */
    g_hwvga_on=0;hw_update(false);
    CHECK(win_mode==WIN_EMU && ram_planar && g_hwvga_fetches==fetch_before+1);
    CHECK((model[1]&0xffff)==(saved[2]&0xffff));
    g_hwvga_on=1;hw_update(false);
    CHECK(win_mode==WIN_HW && stores==VGA_BYTES);
    CHECK((physical[2]&0xffff)==(saved[4]&0xffff));
    CHECK((physical[2]&0xffff)!=(saved[2]&0xffff));
    printf("PASS counterexample actual hw_update font HW->EMU->HW: cell1 old=%04x new=%04x (=old cell2); no guest writes; SR4=07 GR6=04\n",saved[2]&0xffff,physical[2]&0xffff);
    for(unsigned i=0;i<0x10000;i++) CHECK((physical[i]&0xffff0000u)==(saved[i]&0xffff0000u));
    puts("PASS counterexample scope: characters/attributes remapped, font planes preserved; toggle NOT present in supplied native trace");
}
static void surface_handoff_and_proposal(void) {
    reset();surface_setup();memcpy(expected,surface,sizeof surface);
    state.gr[6]=4;state.sr[4]=7;
    g_hwvga_on=0;hw_update(false);g_hwvga_on=1;hw_update(false);
    state.gr[6]=0x0e;state.sr[4]=2;hwvga_fetch(&state);vga_text_refresh(&state,redraw,NULL,1);
    unsigned badcells=0,badpixels=0;
    for(unsigned i=0;i<2000;i++) badcells+=(model[i]&0xffff)!=0x074f;
    for(unsigned i=0;i<640*400;i++) badpixels+=surface[i]!=expected[i];
    printf("PASS full-surface handoff counterexample: %u/2000 character-attribute cells and %u/256000 pixels differ at identical geometry\n",badcells,badpixels);
    CHECK(badcells>0 && badpixels>0);
    reset();surface_setup();memcpy(expected,surface,sizeof surface);memcpy(saved,physical,sizeof saved);
    state.gr[6]=4;state.sr[4]=7;
    g_hwvga_on=0;proposed_hw_update(false);
    /* Actual software font write respects SR2=4 at a linear plane address. */
    state.sr[2]=4;state.gr[4]=2;state.gr[5]=0;
    vga_mem_write(&state,123,0x3c);
    saved[123]=(saved[123]&0xff00ffffu)|0x003c0000u;
    state.sr[2]=3;g_hwvga_on=1;proposed_hw_update(false);
    CHECK(!memcmp(saved,physical,sizeof saved));
    state.gr[6]=0x0e;state.sr[4]=2;state.gr[4]=0;state.gr[5]=0x10;
    hwvga_fetch(&state);vga_text_refresh(&state,redraw,NULL,1);
    CHECK(!memcmp(surface,expected,sizeof surface));
    puts("PASS TEST-ONLY raw planar handoff proposal: 256KiB preserved except intended plane2 font write; 2000 cells and full 640x400 surface preserved");
    /* Regression against the tempting SR4-only display-fetch change. */
    reset();surface_setup();memcpy(expected,surface,sizeof surface);
    state.gr[6]=4;state.sr[4]=7;
    memcpy(model,physical,sizeof model);vga_text_refresh(&state,redraw,NULL,1);
    badcells=badpixels=0;
    for(unsigned i=0;i<2000;i++) badcells+=(model[i]&0xffff)!=0x074f;
    for(unsigned i=0;i<640*400;i++) badpixels+=surface[i]!=expected[i];
    CHECK(badcells==1000 && badpixels>0);
    printf("PASS rejected SR4-only display-fetch proposal: raw font-upload fetch misreads %u/2000 old text cells, %u pixels\n",badcells,badpixels);
}
static void old_new_selector_loss(void) {
    reset();surface_setup();
    /* New graphics registers are installed before hw_update fetches old text. */
    trace_reg(4,7);trace_gr(6,5);
    CHECK(win_mode==WIN_EMU && ram_planar);
    CHECK((model[1]&0xffff)==(physical[1]&0xffff));
    CHECK((model[1]&0xffff)!=(physical[2]&0xffff));
    CHECK((model[0]&0xffff)==0x074f);
    printf("PASS old/new selector counterexample: HW text->wide planar EMU fetch cell1=%04x, old text cell1=%04x; raw physical layout (not compressed text)\n",model[1]&0xffff,physical[2]&0xffff);
    /* This is correct planar backing, not lost raw storage. No-write roundtrip
     * returning to native restores the original physical contents exactly. */
    memcpy(saved,physical,sizeof saved);trace_gr(6,4);
    CHECK(!memcmp(saved,physical,sizeof saved));
    trace_gr(6,0x0e);trace_reg(4,2);hwvga_fetch(&state);
    CHECK((model[1]&0xffff)==0x074f);
    puts("PASS old/new selector control: ordinary wide-planar no-write excursion returns byte-exact; layout change alone is NOT storage loss");
}
static void vbe_clear_handoff(void) {
    for(unsigned preserve=0;preserve<2;preserve++) {
        reset();memset(model,0xa5,sizeof model);memcpy(saved,physical,sizeof saved);
        state.vbe_regs[VBE_DISPI_INDEX_XRES]=640;
        state.vbe_regs[VBE_DISPI_INDEX_YRES]=200;
        state.vbe_regs[VBE_DISPI_INDEX_BPP]=8;
        vbe_write(&state,0,VBE_DISPI_INDEX_ENABLE);
        vbe_write(&state,1,VBE_DISPI_ENABLED|(preserve?VBE_DISPI_NOCLEARMEM:0));
        if(!preserve) for(unsigned index=0;index<640*200;index++) CHECK(((uint8_t *)model)[index]==0);
        unsigned before=(unsigned)g_hwvga_fetches;
        hw_update(false);
        CHECK(win_mode==WIN_EMU && !state.hw_mem);
        CHECK(!memcmp(saved,physical,sizeof saved));
        if(preserve) {
            CHECK(g_hwvga_fetches==before+1);
            for(unsigned index=0;index<0x4000;index++) CHECK(model[index]==physical[(index<<2&0xfffc)|index>>12]);
        } else {
            CHECK(g_hwvga_fetches==before);
            for(unsigned index=0;index<640*200;index++) CHECK(((uint8_t *)model)[index]==0);
        }
        hw_update(false);CHECK(g_hwvga_fetches==before+preserve);
    }
    puts("PASS VBE handoff: clear survives native transition; NOCLEARMEM preserves transfer; physical VGA untouched");
    reset();
    uint32_t frame_pixels[32];memset(frame_pixels,0xff,sizeof frame_pixels);
    FBDevice frame_fb={0};frame_fb.width=8;frame_fb.height=4;frame_fb.stride=32;frame_fb.fb_data=(uint8_t *)frame_pixels;
    state.fb_dev=&frame_fb;state.graphic_mode=2;state.ar_index=0x20;
    state.vbe_regs[VBE_DISPI_INDEX_XRES]=8;state.vbe_regs[VBE_DISPI_INDEX_YRES]=4;
    state.vbe_regs[VBE_DISPI_INDEX_BPP]=32;
    vbe_write(&state,0,VBE_DISPI_INDEX_ENABLE);vbe_write(&state,1,VBE_DISPI_ENABLED);
    unsigned before=(unsigned)g_hwvga_fetches;
    vga_refresh(&state,redraw,NULL,1);
    CHECK(state.hw_mem && g_hwvga_fetches==before);
    for(unsigned index=0;index<32;index++) CHECK(frame_pixels[index]==0);
    free(state.vbe_shadow);
    puts("PASS VBE display-core transition: stale hw_mem flag cannot fetch native pixels into cleared VBE frame");
}
static void planar_graphics_backend(void) {
    reset();state.sr[4]=6;state.sr[2]=15;state.gr[6]=5;state.gr[5]=0;
    memcpy(saved,physical,sizeof saved);
    hw_update(false);
    CHECK(win_mode==WIN_EMU && !hw_on && !state.hw_mem && ram_planar);
    CHECK(g_hwvga_fetches==fetch_before+1 && !memcmp(model,saved,sizeof saved));
    state.gr[5]=1;hw_update(false);
    CHECK(win_mode==WIN_EMU && g_hwvga_fetches==fetch_before+1 && !stores);
    uint32_t source=model[23];
    (void)vga_mem_read(&state,23);
    vga_mem_write(&state,700,0);
    CHECK(model[700]==source);
    saved[700]=source;
    state.gr[5]=0x40;hw_update(false);
    CHECK(win_mode==WIN_EMU && !hw_on && !state.hw_mem && !stores);
    CHECK(!memcmp(model,saved,sizeof saved));
    /* Unaligned multi-plane words must retain both bytes across an 8-byte
     * aperture boundary; simple single-plane stores still qualify for RAM. */
    state.sr[2]=15;
    vga_mem_write16(&state,7,0x6655);
    CHECK(model[7]==0x55555555 && model[8]==0x66666666);
    state.sr[2]=1;
    uint32_t base=~0u;
    CHECK(vga_simple_plane(&state,&base)==0 && base==0);
    state.sr[2]=15;
    state.sr[4]=0x0e;hw_update(false);
    CHECK(win_mode==WIN_RAM && !hw_on && ram_13h);
    reset();state.sr[4]=6;state.sr[2]=15;state.gr[6]=5;state.gr[5]=0;
    safe_planar=0;hw_update(false);
    CHECK(win_mode==WIN_HW && hw_on && !stores);
    safe_planar=1;
    puts("PASS planar graphics backend: 16-color mode and latch copy use software; all planes survive Mode X return; chain4 stays RAM; explicit native override remains");
}
static void chain4_planar_handoff(void) {
    for(unsigned trapped=0;trapped<2;trapped++) {
    reset();state.sr[4]=0x0e;state.sr[2]=15;state.gr[5]=0x40;state.gr[6]=5;
    hw_update(false);
    CHECK(win_mode==WIN_RAM && ram_13h);
    for(unsigned index=0;index<0x10000;index++)
        ((uint8_t *)model)[index]=(uint8_t)(index*37+(index>>8));
    hwvga_store_chain4((uint8_t *)model);
    memcpy(saved,physical,sizeof saved);
    if(trapped) {
        state.sr[2]=1;hw_update(false);
        CHECK(win_mode==WIN_EMU && ram_13h);
    }
    state.sr[4]=6;state.gr[5]=0;
    hw_update(false);
    CHECK(win_mode==WIN_EMU && ram_planar && !ram_13h);
    CHECK(!memcmp(model,saved,sizeof saved));
    state.gr[5]=0x40;hw_update(false);
    CHECK(win_mode==WIN_EMU && !memcmp(model,saved,sizeof saved));
    }
    puts("PASS direct and trapped chain4 -> software planar -> safe Mode X preserve all VGA planes");
    reset();
    state.vga_ram_size=0x100000;
    state.vbe_regs[VBE_DISPI_INDEX_XRES]=640;
    state.vbe_regs[VBE_DISPI_INDEX_YRES]=480;
    state.vbe_regs[VBE_DISPI_INDEX_BPP]=15;
    vbe_write(&state,0,VBE_DISPI_INDEX_ENABLE);
    vbe_write(&state,1,VBE_DISPI_ENABLED);
    CHECK(!(state.gr[VGA_GFX_MODE]&0x10));
    vbe_write(&state,0,VBE_DISPI_INDEX_BANK);
    vbe_write(&state,1,2);
    /* SeaVGABIOS 4F02 tail writes after DISPI ENABLE must not retire VBE. */
    vga_ioport_write(&state,0x3ce,6);vga_ioport_write(&state,0x3cf,0x05);
    vga_ioport_write(&state,0x3c4,2);vga_ioport_write(&state,0x3c5,0x0f);
    vga_ioport_write(&state,0x3c4,4);vga_ioport_write(&state,0x3c5,0x0a);
    vga_ioport_write(&state,0x3ce,5);vga_ioport_write(&state,0x3cf,0x40);
    CHECK(vbe_enabled(&state) && state.bank_offset==0x20000);
    vga_ioport_write(&state,0x3ce,6);vga_ioport_write(&state,0x3cf,0x07);
    CHECK(vbe_enabled(&state) && state.bank_offset==0x20000);
    vga_ioport_write(&state,0x3cf,0x05);
    CHECK(vbe_enabled(&state) && state.bank_offset==0x20000);
    vga_mem_write16(&state,0,0x7fff);
    CHECK(*(uint16_t *)((uint8_t *)model+0x20000)==0x7fff);
    vbe_write(&state,0,VBE_DISPI_INDEX_BANK);
    vbe_write(&state,1,9);
    CHECK(vbe_enabled(&state) && state.bank_offset==0x90000);
    state.vga_ram_size=VGA_BYTES;
    vga_ioport_write(&state,0x3c4,4);vga_ioport_write(&state,0x3c5,0x0a);
    vga_ioport_write(&state,0x3ce,5);vga_ioport_write(&state,0x3cf,0x50);
    vga_ioport_write(&state,0x3ce,6);vga_ioport_write(&state,0x3cf,0x05);
    CHECK(vbe_enabled(&state) && state.bank_offset==0x90000);
    vga_ioport_write(&state,0x3c4,4);vga_ioport_write(&state,0x3c5,0x06);
    CHECK(!vbe_enabled(&state) && state.bank_offset==0);
    vga_ioport_write(&state,0x3c4,4);vga_ioport_write(&state,0x3c5,0x0a);
    vga_ioport_write(&state,0x3d4,0x13);vga_ioport_write(&state,0x3d5,0x50);
    CHECK(!vbe_enabled(&state) && state.bank_offset==0);
    const uint8_t legacy_gr6[] = {0x04,0x01,0x09,0x0d,0x0e};
    for (unsigned index=0;index<sizeof(legacy_gr6);index++) {
        state.vbe_regs[VBE_DISPI_INDEX_ENABLE] |= VBE_DISPI_ENABLED;
        state.bank_offset=0x90000;
        vga_ioport_write(&state,0x3ce,6);vga_ioport_write(&state,0x3cf,legacy_gr6[index]);
        CHECK(!vbe_enabled(&state) && state.bank_offset==0);
    }
    vga_ioport_write(&state,0x3cf,0x05);
    hw_update(false);
    CHECK(win_mode==WIN_RAM && ram_13h);
    puts("PASS VBE BIOS tail writes preserve VBE; direct legacy reprogramming retires active VBE banking");
}
static void vbe_depth_shadow(void) {
    reset();
    uint32_t pixels[32]={0};
    FBDevice display={0};display.width=8;display.height=4;display.stride=32;
    display.fb_data=(uint8_t *)pixels;state.fb_dev=&display;
    state.palette[0]=state.palette[1]=state.palette[2]=63;
    state.vbe_regs[VBE_DISPI_INDEX_XRES]=8;state.vbe_regs[VBE_DISPI_INDEX_YRES]=4;
    state.vbe_regs[VBE_DISPI_INDEX_BPP]=8;
    vbe_write(&state,0,VBE_DISPI_INDEX_ENABLE);vbe_write(&state,1,VBE_DISPI_ENABLED);
    vga_refresh(&state,redraw,NULL,1);
    for(unsigned index=0;index<32;index++) CHECK(pixels[index]==0x00ffffff);
    uint8_t retained[64];memset(retained,0x5a,sizeof retained);
    memcpy(model,retained,sizeof retained);
    vbe_write(&state,0,VBE_DISPI_INDEX_ENABLE);vbe_write(&state,1,0);
    CHECK(!memcmp(model,retained,sizeof retained));
    vbe_write(&state,1,VBE_DISPI_GETCAPS);
    CHECK(!memcmp(model,retained,sizeof retained));
    vbe_write(&state,1,VBE_DISPI_ENABLED|VBE_DISPI_NOCLEARMEM);
    CHECK(!memcmp(model,retained,sizeof retained));
    vbe_write(&state,1,VBE_DISPI_ENABLED);
    CHECK(!memcmp(model,retained,sizeof retained));
    vbe_write(&state,1,0);
    puts("PASS VBE disable, capability queries and active-enable writes preserve framebuffer memory");
    vbe_write(&state,0,VBE_DISPI_INDEX_BPP);vbe_write(&state,1,15);
    vbe_write(&state,0,VBE_DISPI_INDEX_ENABLE);vbe_write(&state,1,VBE_DISPI_ENABLED);
    vga_refresh(&state,redraw,NULL,0);
    for(unsigned index=0;index<32;index++) CHECK(pixels[index]==0);
    vbe_write(&state,0,VBE_DISPI_INDEX_ENABLE);vbe_write(&state,1,0);
    vga_graphic_refresh(&state,redraw,NULL,1);
    for(unsigned index=0;index<5;index++) CHECK(state.vbe_shadow_key[index]==0);
    free(state.vbe_shadow);
    puts("PASS same-size 8 -> 15 bpp invalidates zero-filled shadow rows");
}
/* Windows 98 VDD, physically traced (0028:c0259d30..d93) when a windowed DOS
 * box opens over the 800x600x32 LFB desktop: plane-2 font access, then the
 * exact restore. The LFB desktop must keep scanning out throughout. */
static void vbe_lfb_survives_vdd_font_access(void) {
    reset();
    state.vga_ram_size=0x800000;
    state.vbe_regs[VBE_DISPI_INDEX_XRES]=800;
    state.vbe_regs[VBE_DISPI_INDEX_YRES]=600;
    state.vbe_regs[VBE_DISPI_INDEX_BPP]=32;
    vbe_write(&state,0,VBE_DISPI_INDEX_ENABLE);
    vbe_write(&state,1,VBE_DISPI_ENABLED|VBE_DISPI_LFB_ENABLED|VBE_DISPI_NOCLEARMEM);
    vga_ioport_write(&state,0x3c4,2);vga_ioport_write(&state,0x3c5,0x0f);
    vga_ioport_write(&state,0x3c4,4);vga_ioport_write(&state,0x3c5,0x0a);
    vga_ioport_write(&state,0x3ce,5);vga_ioport_write(&state,0x3cf,0x40);
    vga_ioport_write(&state,0x3ce,6);vga_ioport_write(&state,0x3cf,0x07);
    static const uint16_t save[] = {0x0302,0x0204,0x0000,0x0001,0x0002,0x0003,0x0004,0x5005,0x0f06,0x0f07,0xff08};
    static const uint16_t restore[] = {0x0f02,0x0a04,0x0000,0x0001,0x0002,0x0003,0x0004,0x4005,0x0706,0x0f07,0xff08};
    for (unsigned pass=0;pass<2;pass++) {
        const uint16_t *list=pass?restore:save;
        for (unsigned index=0;index<11;index++) {
            uint32_t port=index<2?0x3c4:0x3ce;
            vga_ioport_write(&state,port,list[index]&0xff);
            vga_ioport_write(&state,port+1,list[index]>>8);
            CHECK(vbe_enabled(&state));
        }
    }
    CHECK(state.vbe_regs[VBE_DISPI_INDEX_XRES]==800 && state.vbe_regs[VBE_DISPI_INDEX_BPP]==32);
    puts("PASS LFB VBE desktop survives the Windows 98 VDD plane-2 font save/restore sequence");
}
int main(void) {
    transfer_controls();unchanged_native_trace();concrete_handoff_loss();surface_handoff_and_proposal();old_new_selector_loss();
    vbe_clear_handoff();planar_graphics_backend();chain4_planar_handoff();vbe_depth_shadow();
    vbe_lfb_survives_vdd_font_access();
    puts("PASS VGA transfers: native/planar controls and VBE clear/display-core transition regressions");
    return 0;
}
