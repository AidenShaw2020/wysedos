/* Deterministic producer/consumer interleaving at single-cycle completion.
 * clang -O2 -ffunction-sections -DCIRCLE_BUILD -DWYSE_BUILD=1
 * -DSOUND_FREQUENCY=48000 -Ipc -Iefi/platform tools/tests/sb_dma_ownership.c
 * -Wl,/opt:ref -o work/sb_dma_ownership.exe */
#define i8257_dma_hold_DREQ consumer_hold
#include "../../pc/sb16.c"
#undef i8257_dma_hold_DREQ
#include "../../pc/i8257.c"

static uint32_t test_time=1000000;
uint32_t time_us_32(void) { return test_time; }
int get_core_num(void) { return 0; }
void cpu_prefetch_invalidate(CPUI386 *cpu) { (void)cpu; }
void *pcmalloc(size_t size) { return calloc(1,size); }
uint32_t i8259_debug_master(PicState2 *pic) { (void)pic; return 0; }
static SB16State *test_card;
static int interrupt_consumer;
static void irq(void *p,int n,int level) { (void)p;(void)n;(void)level; }
void consumer_hold(IsaDma *dma,int channel) {
    /* Producer completes the block after the consumer read dma_running,
     * but before its stale DREQ assertion reaches the controller. */
    if (interrupt_consumer) { interrupt_consumer=0;control(test_card,0); }
    i8257_dma_hold_DREQ(dma,channel);
}
#define CHECK(x,msg) do {if(!(x)){puts("FAIL: " msg);return 1;}}while(0)
static int run_case(int high) {
    static unsigned char ram[4096];
    I8257State dma={0};SB16State card={0};test_card=&card;
    dma.phys_mem=(char *)ram;dma.phys_mem_size=sizeof(ram);
    dma.dshift=high;
    dma.regs[1].base[1]=high?655:1311;dma.regs[1].now[0]=256;
    dma.regs[1].mode=0x59;
    dma.regs[1].transfer_handler=SB_read_DMA;dma.regs[1].opaque=&card;
    card.dma=1;card.hdma=5;card.isa_dma=&dma;card.isa_hdma=&dma;
    card.voice=&card;card.set_irq=irq;card.irq=5;
    card.use_hdma=high;
    card.freq=high?22050:11236;card.bytes_per_second=high?44100:11236;
    card.fmt=high?AUDIO_FORMAT_S16:AUDIO_FORMAT_U8;card.align=high;
    card.block_size=card.left_till_irq=1312;card.audio_q=8;
    memset(card.audio_buf,128,sizeof(card.audio_buf));memset(ram,144,sizeof(ram));
    control(&card,1);
    interrupt_consumer=1;
    int l=0,r=0;sb16_getsample(&card,&r,&l);
    /* If the consumer no longer touches DREQ, complete the producer's stop
     * here instead. Both schedules end with the DSP stopped. */
    if (card.dma_running) control(&card,0);
    unsigned before=card.audio_q;i8257_dma_run(&dma);
    CHECK(card.audio_q==before,"stopped DSP read another block after consumer raced with stop");
    CHECK(interrupt_consumer,"consumer still accessed the DMA controller");
    interrupt_consumer=0;
    /* A stale external request must not restart the stopped peripheral. */
    i8257_dma_hold_DREQ(&dma,high?5:1);i8257_dma_run(&dma);
    CHECK(card.audio_q==before,"stale DREQ copied PCM while DSP inactive");
    i8257_dma_release_DREQ(&dma,high?5:1);
    control(&card,1);i8257_dma_run(&dma);
    CHECK(card.audio_q>before,"producer's active DREQ did not transfer PCM");
    before=card.audio_q;
    for(int i=0;i<480;i++){l=r=0;sb16_getsample(&card,&r,&l);}
    i8257_dma_run(&dma);
    CHECK(card.audio_q>before,"DMA failed to refill without consumer reassertion");
    card.dma_auto=1;card.owed_head=1;card.owed_tail=0;
    card.irq_fallback_us=time_us_32()+250000;
    before=card.audio_q;i8257_dma_run(&dma);
    CHECK(card.audio_q==before,"DMA crossed pending completion with IRQ barrier enabled");
    card.owed_tail=1;card.owed_unacked=1;i8257_dma_run(&dma);
    CHECK(card.audio_q==before,"DMA crossed unacknowledged completion");
    card.mixer_regs[0x82]=high?2:1;
    card.port=0x220;
    sb16_dsp_read(&card,high?0x22f:0x22e);
    CHECK(!card.owed_unacked,"guest acknowledge did not release completion");
    card.audio_p=card.audio_q;
    i8257_dma_run(&dma);
    CHECK(card.audio_q>before,"DMA failed to resume after guest IRQ acknowledge");
    before=card.audio_q;card.audio_p=card.audio_q;
    card.owed_unacked=1;card.owed_head=card.owed_tail;
    test_time=card.irq_fallback_us+1;
    i8257_dma_run(&dma);
    CHECK(card.audio_q>before,"unanswered guest blocked DMA beyond recovery deadline");
    card.owed_unacked=0;test_time=1000000;
    return 0;
}
int main(void) {
    if (run_case(0) || run_case(1)) return 1;
    puts("PASS: 8/16-bit DMA ownership, auto-init IRQ barrier, acknowledge resume and timeout recovery");
    return 0;
}
