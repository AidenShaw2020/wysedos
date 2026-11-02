/* Exercise the actual ISA decoder as a firmware/OS enumerator would. */
#include "../../pc/ne2000.c"
void *pcmalloc(long n) { return calloc(1, n); }
uint32_t get_uticks(void) { return 0; }
static int irq3;
static void irq(void *p, int n, int level) { (void)p; if (n == 3) irq3 = level; }
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x);exit(1); } } while(0)
static NE2000State *card;
static void out(unsigned p, unsigned v) { CHECK(ne2000_isa_write(card,p,1,v)); }
static unsigned in(unsigned p) { uint32_t v;
    if (!ne2000_isa_read(card,p,1,&v)) {
        printf("Unclaimed read port %x state %u reg %x\n",p,card->pnp_state,card->pnp_reg);exit(1);
    } return v;
}
static void reg(unsigned r,unsigned v) { out(0x279,r);out(0xa79,v); }
static unsigned rd(unsigned r) { out(0x279,r);return in(0x203); }
static void key(void) {
    /* Published 1.0a appendix B sequence, independent of the implementation. */
    const uint8_t bytes[]={0x6a,0xb5,0xda,0xed,0xf6,0xfb,0x7d,0xbe,
        0xdf,0x6f,0x37,0x1b,0x0d,0x86,0xc3,0x61,
        0xb0,0x58,0x2c,0x16,0x8b,0x45,0xa2,0xd1,
        0xe8,0x74,0x3a,0x9d,0xce,0xe7,0x73,0x39};
    out(0x279,0);out(0x279,0);
    for (unsigned i=0;i<sizeof bytes;i++) out(0x279,bytes[i]);
}
int main(void) {
    card=isa_ne2000_init(0x300,3,NULL,irq);
    uint32_t value;
    CHECK(!ne2000_isa_read(card,0x203,1,&value));
    out(0x300,0x21);CHECK(in(0x300)==0x21); /* DOS, before any PnP */
    reg(0x30,0);CHECK(in(0x300)==0x21); /* locked interface ignores writes */
    key();CHECK(card->pnp_state==PNP_SLEEP);
    reg(2,5);reg(3,0);reg(0,0x80);out(0x279,1);
    uint8_t serial[9]={0};
    for (unsigned bit=0;bit<72;bit++) {
        unsigned a=in(0x203),b=in(0x203);
        CHECK((a==0 && b==0) || (a==0x55 && b==0xaa));
        if(a==0x55) serial[bit/8]|=1u<<(bit%8);
    }
    CHECK(!memcmp(serial,(uint8_t[]){0x41,0xd0,0x80,0xd6,1,0,0,0},8));
    /* Independent bit-at-a-time checksum over the decoded wire identifier. */
    unsigned sum=0x6a;
    for(unsigned i=0;i<64;i++) {
        unsigned feedback=((sum&1)^((sum>>1)&1)^((serial[i/8]>>(i%8))&1));
        sum=(sum>>1)|(feedback<<7);
    }
    CHECK(sum==serial[8]);
    reg(6,1);CHECK(rd(6)==1);
    /* Isolation consumed the serial bytes; resource stream begins at version. */
    CHECK(rd(5)==1);CHECK(rd(4)==0x0a);
    reg(3,0);out(0x279,1);
    CHECK(!ne2000_isa_read(card,0x203,1,&value)); /* no second card */
    reg(3,1); /* direct wake must prepend all nine serial bytes */
    for(unsigned i=0;i<9;i++) CHECK(rd(4)==serial[i]);
    const uint8_t descriptors[]={0x0a,0x10,0,0x15,0x41,0xd0,0x80,0xd6,3,
        0x47,1,0,3,0,3,1,32,0x22,8,0,0x79,0};
    for(unsigned i=0;i<sizeof descriptors;i++) { CHECK(rd(5)==1);CHECK(rd(4)==descriptors[i]); }
    CHECK(rd(5)==1);CHECK(rd(4)==0xff);
    CHECK(rd(7)==0);reg(7,9);CHECK(rd(7)==0);
    CHECK(rd(0x60)==3 && rd(0x61)==0 && rd(0x70)==3 && rd(0x71)==2);
    reg(0x61,0x40);reg(0x70,5);CHECK(rd(0x61)==0 && rd(0x70)==3);
    card->imr=ENISR_RX;atomic_store(&card->isr,ENISR_RX);
    ne2000_update_irq(card);CHECK(irq3);
    reg(0x30,0);CHECK(!irq3 && in(0x300)==0xff);
    CHECK(ne2000_isa_read(card,0x310,2,&value) && value==0xffff);
    out(0x300,0x42);CHECK(card->cmd==0x21); /* inactive writes have no effect */
    reg(0x31,2);CHECK(in(0x300)==0xaa && in(0x31f)==0xaa);
    reg(0x31,3);CHECK(in(0x311)==0x55);
    CHECK(ne2000_isa_read(card,0x310,2,&value) && value==0x5555);
    reg(0x31,0);reg(0x30,1);CHECK(irq3);out(0x300,0x21);
    reg(2,2);CHECK(!ne2000_isa_read(card,0x203,1,&value));
    CHECK(in(0x300)==0x21); /* wait-for-key does not disable DOS card */
    key();reg(2,1);out(0x279,6);
    CHECK(!ne2000_isa_read(card,0x203,1,&value)); /* Sleep floats RD_DATA */
    reg(3,1);CHECK(rd(6)==1 && rd(0x30)==1 && !irq3);
    CHECK(!ne2000_isa_write(card,0x320,1,0));
    CHECK(!ne2000_isa_read(card,0x1300,1,&value)); /* 16-bit port decode */
    puts("PASS: DOS defaults, key, isolation/checksum, CSN, resource stream, fixed resources, range check, activation/IRQ, reset");
    return 0;
}
