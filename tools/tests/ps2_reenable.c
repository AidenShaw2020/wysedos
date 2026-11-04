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
static int idle_clock_regression(void) {
    PS2KbdState *k; PS2MouseState *m;
    now=0x80000010u;
    KBDState *s=i8042_init(&k,&m,1,12,NULL,irq,NULL,reset);
    ps2_put_keycode(k,1,30);
    CHECK(lines[1]); CHECK(kbd_read_data(s,0)==30);
    kbd_write_command(s,0,0xd4);kbd_write_data(s,0,0xf4);
    CHECK(kbd_read_data(s,0)==0xfa);
    now+=1000;kbd_step(s);
    /* Both devices were drained, then idle for more than the signed
     * half-range of the 32-bit microsecond clock (35m47s). */
    now+=0x80000001u;
    ps2_put_keycode(k,0,30);kbd_step(s);
    CHECK(lines[1]);CHECK(kbd_read_data(s,0)==0x9e);
    ps2_mouse_event(m,1,-1,0,0);kbd_step(s);
    CHECK(lines[12]);CHECK(kbd_read_data(s,0)==8);
    now+=1000;kbd_step(s);CHECK(kbd_read_data(s,0)==1);
    now+=1000;kbd_step(s);CHECK(kbd_read_data(s,0)==1);
    now+=1000;kbd_step(s);
    /* The real 1ms device-byte hold must still work across clock wrap. */
    now=0xffffff80u;
    ps2_put_keycode(k,1,31);kbd_step(s);
    CHECK(lines[1]);CHECK(kbd_read_data(s,0)==31);
    ps2_put_keycode(k,0,31);CHECK(!lines[1]);
    now=0x100;kbd_step(s);CHECK(!lines[1]);
    now=0x400;kbd_step(s);CHECK(lines[1]);CHECK(kbd_read_data(s,0)==0x9f);
    puts("PASS: high-uptime first input, long-idle keyboard/mouse, 1ms hold across clock wrap");
    return 0;
}
int main(void) {
    CHECK(idle_clock_regression()==0);
    memset(lines,0,sizeof(lines));now=0;
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
    int total_x=0,total_y=0;
    while (m->common.queue.count > 0 || m->mouse_dx != 0 || m->mouse_dy != 0) {
        CHECK(m->common.queue.count % 3 == 0);
        now += 1000; kbd_step(s);
        uint32_t b0 = kbd_read_data(s, 0);
        now += 1000; kbd_step(s);
        total_x+=(int8_t)kbd_read_data(s, 0);
        now += 1000; kbd_step(s);
        total_y+=(int8_t)kbd_read_data(s, 0);
        CHECK((b0 & 0x08) != 0 && (b0 & 0x07) == 0);
        now += 1000; kbd_step(s);
    }
    CHECK(total_x==-30000 && total_y==-25000);
    CHECK(m->motion_in_x==m->motion_out_x && m->motion_in_y==m->motion_out_y);
    CHECK(m->common.queue.dropped==0 && m->common.queue.peak<PS2_QUEUE_SIZE);
    /* Small, isolated vertical steps must survive in both directions. */
    for(int direction=-1;direction<=1;direction+=2) {
        for(int j=0;j<20;j++) {
            ps2_mouse_event(m,0,direction,0,0);
            now+=1000;kbd_step(s);uint32_t b=kbd_read_data(s,0);
            now+=1000;kbd_step(s);CHECK(kbd_read_data(s,0)==0);
            now+=1000;kbd_step(s);CHECK((int8_t)kbd_read_data(s,0)==-direction);
            CHECK((b&8) && !(b&7));
        }
    }
    CHECK(m->motion_in_y==m->motion_out_y && !m->mouse_dy);
    CHECK(m->common.queue.produced==m->common.queue.consumed && !m->common.queue.count);
    ps2_write_mouse(m,AUX_DISABLE_DEV);
    ps2_mouse_event(m,0,3,0,0);
    CHECK(m->motion_disabled==1 && m->motion_disabled_y==-3 && !m->mouse_dy);
    puts("PASS: PS/2 IRQ/framing; motion conserved under pressure; single-step vertical motion; disabled-device accounting");return 0;
}
