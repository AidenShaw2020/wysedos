/* Deterministic Blood-style ISR/producer schedule; no hardware or threads.
 * clang -O2 -ffunction-sections -DCIRCLE_BUILD -DWYSE_BUILD=1
 * -DSOUND_FREQUENCY=48000 -Ipc -Iefi/platform
 * tools/tests/sb_autoinit_blocks.c -Wl,/opt:ref -o work/sb_autoinit_blocks.exe
 * The CONTROL calls real write_audio explicitly, NOT an original-code build.
 */
#include "../../pc/sb16.c"
#include "../../pc/i8257.c"

enum { BASE = 0x12000, BUFFER = 4096, BLOCK = 1024, COMPLETIONS = 12 };
static unsigned char ram[BASE + BUFFER], expected[BUFFER];
static uint32_t clock_us = 1000000;
static unsigned irq_rises;
static int irq_level;
uint32_t time_us_32(void) { return clock_us; }
int get_core_num(void) { return 0; }
void cpu_prefetch_invalidate(CPUI386 *cpu) { (void)cpu; }
void *pcmalloc(size_t size) { return calloc(1, size); }
uint32_t i8259_debug_master(PicState2 *pic) { (void)pic; return 0; }
static void irq(void *p, int n, int level) {
    (void)p; (void)n;
    if (level && !irq_level) irq_rises++;
    irq_level = level;
}
#define CHECK(x, msg) do { if (!(x)) { puts("FAIL: " msg); return 1; } } while (0)

static void pattern(unsigned block, unsigned generation) {
    for (unsigned i = 0; i < BLOCK; i++) {
        unsigned char b = 1 + (block * 37 + generation * 13 + i * 7) % 127;
        expected[block * BLOCK + i] = ram[BASE + block * BLOCK + i] = b;
    }
}
static void setup(I8257State *dma, SB16State *s) {
    memset(dma, 0, sizeof(*dma)); memset(s, 0, sizeof(*s));
    clock_us = 1000000; irq_rises = 0; irq_level = 0;
    for (unsigned b = 0; b < 4; b++) pattern(b, 0);
    dma->phys_mem = (char *)ram; dma->phys_mem_size = sizeof(ram);
    dma->dshift = 1; dma->mask = 0x0d;
    /* Actual DMA5 address/count/page programming, including word units. */
    /* Controller handlers take offsets from 0xd0 (unlike channel ports). */
    i8257_write_cont(dma, 0x08, 0, 1); /* Guest port 0xd8. */
    i8257_write_chan(dma, 0xc4, (BASE >> 1) & 255, 1);
    i8257_write_chan(dma, 0xc4, (BASE >> 9) & 255, 1);
    i8257_write_chan(dma, 0xc6, (BUFFER / 2 - 1) & 255, 1);
    i8257_write_chan(dma, 0xc6, (BUFFER / 2 - 1) >> 8, 1);
    i8257_write_page(dma, 0x8b, BASE >> 16);
    i8257_write_cont(dma, 0x06, 0x59, 1); /* Guest port 0xd6. */
    s->dma = 1; s->hdma = 5; s->isa_dma = s->isa_hdma = dma;
    s->port = 0x220; s->irq = 5; s->set_irq = irq;
    s->freq = 44000; s->time_const = -1;
    i8257_dma_register_channel(dma, 5, SB_read_DMA, s);
    dma_cmd(s, 0xb6, 0x30, BLOCK / 2 - 1); /* S16 stereo auto-init. */
}
static void batch(SB16State *s) {
    for (unsigned i = 0; i < 512; i++) {
        int l = 0, r = 0;
        sb16_getsample(s, &r, &l);
    }
    clock_us += 512u * 1000000u / SOUND_FREQUENCY;
}
static unsigned raw_address(I8257State *dma) {
    i8257_write_cont(dma, 0x08, 0, 1);
    unsigned low = (unsigned)i8257_read_chan(dma, 0xc4, 1);
    unsigned high = (unsigned)i8257_read_chan(dma, 0xc4, 1);
    return ((unsigned)i8257_read_page(dma, 0x8b) << 16) |
           ((low | (high << 8)) << 1);
}
static int blocked(I8257State *dma, SB16State *s) {
    unsigned q = s->audio_q, bytes = g_sb16_dma_bytes;
    int pos = dma->regs[1].now[COUNT];
    uint32_t calls = g_sb16_dma_calls;
    i8257_dma_run(dma);
    CHECK(g_sb16_dma_calls == calls + 1, "barrier check did not enter real callback");
    CHECK(s->audio_q == q && g_sb16_dma_bytes == bytes &&
          dma->regs[1].now[COUNT] == pos, "pending/unacked DMA copied or moved cursor");
    return 0;
}
static int copied_pattern(SB16State *s, unsigned q, unsigned block) {
    CHECK(s->audio_q - q == BLOCK, "resume did not copy exactly one DSP block");
    for (unsigned i = 0; i < BLOCK; i++) {
        unsigned got = s->audio_buf[(q + i) % AUDIO_BUF_LEN];
        unsigned want = expected[block * BLOCK + i];
        if (got != want) {
            printf("FAIL: pattern block=%u offset=%u q=%u got=%u expected=%u\n",
                   block, i, q, got, want);
            return 1;
        }
    }
    return 0;
}
static int control_without_barrier(I8257State *dma, SB16State *s, unsigned cleared) {
    /* Isolated snapshot of the SAME clear/yield schedule. The old producer
     * could spend the batch's free-space budget across two DSP blocks.
     * Bypass ONLY the gate by calling the real memory-to-ring copy twice;
     * this reference intentionally does not model IRQ/completion accounting.
     */
    I8257State d = *dma; SB16State c = *s;
    c.isa_dma = c.isa_hdma = &d;
    int pos = d.regs[1].now[COUNT];
    unsigned q = c.audio_q, bytes = g_sb16_dma_bytes;
    CHECK(sb16_lead_bytes(&c) - (int)(c.audio_q - c.audio_p) >= 2 * BLOCK,
          "control has no legitimate two-block copy budget");
    CHECK(write_audio(&c, 5, pos, BUFFER, BLOCK) == BLOCK, "control first copy failed");
    pos = (pos + BLOCK) % BUFFER;
    CHECK((unsigned)pos / BLOCK == cleared, "control missed ISR-cleared block");
    CHECK(write_audio(&c, 5, pos, BUFFER, BLOCK) == BLOCK, "control second copy failed");
    unsigned zeros = 0;
    for (unsigned i = 0; i < BLOCK; i++)
        zeros += c.audio_buf[(q + BLOCK + i) % AUDIO_BUF_LEN] == 0;
    CHECK(c.audio_q - q == 2 * BLOCK && g_sb16_dma_bytes - bytes == 2 * BLOCK,
          "control did not use actual DMA memory copy");
    CHECK(zeros == BLOCK, "barrier-free control did not reproduce zero capture");
    printf("CONTROL (explicit write_audio, not original binary): ISR clear block=%u, copied=%u, zero bytes=%u\n",
           cleared, 2 * BLOCK, zeros);
    return 0;
}
static int streaming(void) {
    I8257State dma; SB16State s; setup(&dma, &s);
    CHECK(s.block_size == BLOCK && s.bytes_per_second == 176000 &&
          s.fmt == AUDIO_FORMAT_S16 && s.fmt_stereo && s.dma_auto,
          "Blood DSP configuration mismatch");
    unsigned versions[4] = {0}, serviced = 0;
    i8257_dma_run(&dma);
    if (copied_pattern(&s, 0, 0)) return 1;
    for (unsigned k = 0; k < COMPLETIONS; k++) {
        CHECK(s.owed_head - s.owed_tail == 1 && !s.owed_unacked,
              "completion was not pending");
        batch(&s); /* One real 512-output-frame batch frees multiple blocks. */
        if (blocked(&dma, &s)) return 1;
        sb16_poll(&s);
        CHECK(irq_level && irq_rises == k + 1 && s.owed_unacked &&
              s.owed_head == s.owed_tail, "poll did not deliver one completion IRQ");
        unsigned raw = raw_address(&dma);
        unsigned cursor = ((raw - BASE) / BLOCK) % 4;
        CHECK(raw == BASE + ((k + 1) % 4) * BLOCK, "raw low/high/page cursor or TC reload wrong");
        unsigned next = (cursor + 1) % 4; /* Guest ISR's next-block selection. */
        memset(ram + BASE + next * BLOCK, 0, BLOCK);
        if (!k && control_without_barrier(&dma, &s, next)) return 1;
        /* Yield twice to the producer while the ISR has cleared its block. */
        if (blocked(&dma, &s) || blocked(&dma, &s)) return 1;
        pattern(next, ++versions[next]);
        if (blocked(&dma, &s)) return 1; /* Restoring PCM alone is not an ack. */
        sb16_dsp_read(&s, 0x22f);
        CHECK(!s.owed_unacked && !irq_level, "16-bit DSP ack did not release IRQ");
        unsigned q = s.audio_q;
        i8257_dma_run(&dma);
        if (copied_pattern(&s, q, cursor)) return 1;
        serviced++;
    }
    printf("PASS: DMA5 buffer=4096 DSP=1024 S16 stereo=44000 batch=512; %u ISR completions, 3 wraps, 48 gated calls, 13 patterned blocks, ack resume\n", serviced);
    return 0;
}
static int fallback(int unacked) {
    I8257State dma; SB16State s; setup(&dma, &s);
    i8257_dma_run(&dma); batch(&s);
    if (unacked) sb16_poll(&s);
    CHECK(!!s.owed_unacked == unacked, "fallback setup wrong");
    unsigned q = s.audio_q;
    clock_us = s.irq_fallback_us - 1;
    if (blocked(&dma, &s)) return 1;
    clock_us++;
    i8257_dma_run(&dma);
    if (copied_pattern(&s, q, 1)) return 1;
    printf("PASS: %s barrier holds at 249999us; recovery copies at 250000us\n",
           unacked ? "unacked" : "pending");
    return 0;
}
int main(void) {
    return streaming() || fallback(0) || fallback(1);
}