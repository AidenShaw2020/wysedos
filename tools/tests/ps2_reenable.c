/* Exercise actual controller commands used when leaving Windows for DOS. */
#include "../../pc/i8042.c"
static uint32_t now;
uint32_t get_uticks(void) { return now; }
uint8_t g_kbd_typematic;
void cpu_set_a20(CPUI386 *cpu,int val) { (void)cpu;(void)val; }
int cpu_get_a20(CPUI386 *cpu) { (void)cpu;return 1; }
static int lines[16];
static void irq(void *p,int n,int on) { (void)p;lines[n]=on; }
static void reset(void *p) { (void)p;abort(); }
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n",__LINE__);return 1; } } while(0)
int main(void) {
    PS2KbdState *k;PS2MouseState *m;
    KBDState *s=i8042_init(&k,&m,1,12,NULL,irq,NULL,reset);
    kbd_write_command(s,0,0xad);CHECK(s->mode&0x10);
    kbd_write_data(s,0,0xed); // keyboard LED command must reopen the clock
    CHECK(!(s->mode&0x10));CHECK(lines[1]);
    CHECK(kbd_read_data(s,0)==0xfa);
    kbd_write_data(s,0,0);CHECK(kbd_read_data(s,0)==0xfa);
    kbd_write_command(s,0,0xa7);CHECK(s->mode&0x20);
    kbd_write_command(s,0,0xd4);kbd_write_data(s,0,0xf4);
    CHECK(!(s->mode&0x20));CHECK(lines[12]);
    CHECK(kbd_read_data(s,0)==0xfa);
    now=2000;ps2_put_keycode(k,1,30);CHECK(lines[1]);
    CHECK(kbd_read_data(s,0)==30);CHECK(!lines[1]);
    ps2_put_keycode(k,0,30);CHECK(!lines[1]);
    now+=1000;kbd_step(s);CHECK(lines[1]);CHECK(kbd_read_data(s,0)==0x9e);

    /* Fast/large mouse motion with buttons=0 must never split a 3-byte PS/2
     * packet across the queue limit or synthesize a phantom button click. */
    for (int i = 0; i < 100; i++) {
        ps2_mouse_event(m, -300, 250, 0, 0);
        CHECK(m->common.queue.count % 3 == 0);
    }
    while (m->common.queue.count > 0 || m->mouse_dx != 0 || m->mouse_dy != 0) {
        CHECK(m->common.queue.count % 3 == 0);
        now += 1000; kbd_step(s);
        uint32_t b0 = kbd_read_data(s, 0);
        now += 1000; kbd_step(s);
        (void)kbd_read_data(s, 0);
        now += 1000; kbd_step(s);
        (void)kbd_read_data(s, 0);
        CHECK((b0 & 0x08) != 0 && (b0 & 0x07) == 0);
        now += 1000; kbd_step(s);
    }
    puts("PASS: keyboard/mouse commands reopen clock, ACKs, delayed key IRQ edge, mouse framing");return 0;
}
