/* Decode the actual host bridge PAM write permissions, not a second model. */
#include "../../pc/pci.c"
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n",__LINE__);return 1; } } while(0)
int main(void) {
    PCIDevice d={0};I440FXState s={0};s.pci_dev=&d;
    i440fx_pam_reset(&s);
    CHECK(i440fx_pam_write_enabled(&s,0xf0000));
    CHECK(i440fx_pam_write_enabled(&s,0xfffff));
    CHECK(!i440fx_pam_write_enabled(&s,0xc0000));
    for (unsigned b=0;b<12;b++) {
        memset(d.config+0x5a,0,6);
        d.config[0x5a+b/2]=2u<<((b&1)*4);
        for (unsigned k=0;k<12;k++) {
            CHECK(i440fx_pam_write_enabled(&s,0xc0000+k*0x4000)==(b==k));
            CHECK(i440fx_pam_write_enabled(&s,0xc3fff+k*0x4000)==(b==k));
        }
    }
    d.config[0x59]=0x10;CHECK(!i440fx_pam_write_enabled(&s,0xf0000));
    d.config[0x59]=0x20;CHECK(i440fx_pam_write_enabled(&s,0xf0000));
    CHECK(i440fx_pam_write_enabled(&s,0xbffff) && i440fx_pam_write_enabled(&s,0x100000));
    puts("PASS: all PAM low/high nibbles, boundaries, F segment reset/open/closed");return 0;
}
