/* clang -O2 -ffunction-sections -DCIRCLE_BUILD -DWYSE_BUILD=1
 * -DSOUND_FREQUENCY=48000 -Ipc -Iefi/platform tools/tests/sb_pcm_rate.c
 * '-Wl,/opt:ref' -o work/sb_pcm_rate.exe */
#include "../../pc/sb16.c"
#include "../../pc/i8257.c"
uint32_t time_us_32(void) { return 1000000; }
int get_core_num(void) { return 0; }
void cpu_prefetch_invalidate(CPUI386 *cpu) { (void)cpu; }
void *pcmalloc(size_t size) { return calloc(1,size); }
uint32_t i8259_debug_master(PicState2 *pic) { (void)pic; return 0; }
static int sample(unsigned n) { return (int)(n%200)*200-20000; }
int main(void) {
    SB16State s={0};
    s.active_out=1;s.freq=44000;s.bytes_per_second=176000;
    s.fmt=AUDIO_FORMAT_S16;s.fmt_stereo=1;s.align=3;
    unsigned produced=0,previous_consumed=0;
    for (unsigned i=0;i<48000;i++) {
        while (s.audio_q-s.audio_p<1024) {
            int16_t l=sample(produced),r=-l;
            unsigned p=s.audio_q%AUDIO_BUF_LEN;
            memcpy(s.audio_buf+p,&l,2);memcpy(s.audio_buf+p+2,&r,2);
            s.audio_q+=4;produced++;
        }
        int l=0,r=0;sb16_getsample(&s,&r,&l);
        /* Check exact input consumption and the decoded frame before
         * interpolation and DC blocking, without production capture hooks. */
        uint64_t phase=(uint64_t)(i+1)*(((uint32_t)44000<<16)/48000);
        unsigned consumed=(unsigned)(phase>>16);
        if (s.audio_p!=consumed*4) { puts("FAIL: PCM consumption rate");return 1; }
        int expected=sample(previous_consumed);
        if (sb16_last_l!=expected || sb16_last_r!=-expected) {
            puts("FAIL: decoded input frame or stereo channel order");return 1;
        }
        previous_consumed=consumed;
    }
    printf("PCM input consumed %u frames / 48000 output frames, exact decode verified\n",s.audio_p/4);
    puts("PASS: 44kHz stereo frame rate, channel order and ring wrap");return 0;
}