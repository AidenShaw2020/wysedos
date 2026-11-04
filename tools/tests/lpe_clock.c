/* clang -O2 tools/tests/lpe_clock.c -o work/lpe_clock_test.exe */
#include <stdio.h>
#include <stdint.h>
#include "../../efi/platform/lpe_clock.h"
typedef struct { uint64_t now; uint32_t ht,vt,khz,base; int stopped; } Sim;
static uint32_t read_reg(void *p,uint32_t reg) {
    Sim *s=p;
    if (reg==0x60000 || reg==0x61000 || reg==0x63000) return ((s->ht-1)<<16)|1919;
    if (reg==0x6000c || reg==0x6100c || reg==0x6300c) return ((s->vt-1)<<16)|1079;
    if (reg==0x70040 || reg==0x71040 || reg==0x74040) return s->base+(s->stopped?0:
        (uint32_t)(s->now*s->khz/((uint64_t)s->ht*s->vt*1000)));
    return 0;
}
static uint64_t time_us(void *p) { return ((Sim *)p)->now++; }
#define CHECK(x) do { if (!(x)) {printf("FAIL %d\n",__LINE__);return 1;} } while (0)
int main(void) {
    const uint32_t modes[][3]={{2200,1125,148500},{4000,1111,266640},
        {4400,2250,297000},{4400,2250,594000},{2200,1125,148352}};
    for (unsigned i=0;i<sizeof(modes)/sizeof(*modes);i++) {
        for (unsigned pipe=0;pipe<3;pipe++) {
            Sim s={12345,modes[i][0],modes[i][1],modes[i][2],0xfffffffc,0};
            uint32_t k=lpe_measure_pixel_clock(read_reg,time_us,&s,pipe);
            CHECK(k+10>=s.khz && k<=s.khz+10);
            CHECK(lpe_hdmi_cts(k,48000,6144)==k);
        }
    }
    Sim s={0,2200,1125,148500,0,1};
    CHECK(lpe_measure_pixel_clock(read_reg,time_us,&s,1)==0);
    CHECK(s.now<=1000002);
    CHECK(lpe_measure_pixel_clock(read_reg,time_us,&s,3)==0);
    CHECK(lpe_hdmi_cts(148500,44100,6272)==165000);
    CHECK(lpe_hdmi_cts(297000,48000,6144)==297000);
    CHECK(lpe_hdmi_cts(594000,48000,6144)==594000);
    CHECK(lpe_hdmi_cts(148500,0,6144)==0);
    puts("PASS: scanout clock at 1080p, ultrawide, 4K30/60 and 59.94 Hz; frame-counter wrap, timeout, HDMI CTS");
    return 0;
}
