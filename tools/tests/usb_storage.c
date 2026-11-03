/* clang -O1 -Ipc tools/tests/usb_storage.c -o work/usb-storage-test.exe
 * Real USB peripheral, UHCI scheduler and PCI config; mock disk + RAM only. */
#include "../../pc/usb_msd.c"
#include "../../pc/uhci.c"
#include "../../pc/pci.c"
#define CHECK(x) do { if(!(x)) { printf("FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint8_t disk[8192], replacement[8192];
static unsigned writes, flushes; static int write_fail, flush_fail;
static int seek_disk(BlockDev *d,uint64_t p) { d->pos=p; return p<=8192?0:-1; }
static uint64_t size_disk(BlockDev *d) { (void)d; return 8192; }
static int read_disk(BlockDev *d,void *p,unsigned n) {
    if(d->pos>8192 || n>8192-d->pos) return -1;
    memcpy(p,(uint8_t *)d->ctx+d->pos,n); d->pos+=n; return n;
}
static int write_disk(BlockDev *d,const void *p,unsigned n) {
    if(write_fail || d->pos>8192 || n>8192-d->pos) return -1;
    memcpy((uint8_t *)d->ctx+d->pos,p,n); d->pos+=n; writes++; return n;
}
static int flush_disk(BlockDev *d) { (void)d; flushes++; return flush_fail?-1:0; }
static void configure(USBMSD *s) {
    uint8_t setup[8]={0,9,1}; CHECK(usb_msd_packet(s,0x2d,0,setup,8)==8);
    CHECK(usb_msd_packet(s,0x69,0,setup,0)==0);
}
static void cbw(USBMSD *s,uint8_t op,int in,unsigned len,uint32_t lba,unsigned count) {
    uint8_t p[31]={0}; memcpy(p,"USBC",4); putle32(p+4,0xabcdef01); putle32(p+8,len);
    p[12]=in?128:0; p[14]=10; p[15]=op; putbe32(p+17,lba); p[22]=count>>8; p[23]=count;
    if(op==0x12 || op==3 || op==0x1a) { p[14]=6; p[19]=(uint8_t)count; }
    CHECK(usb_msd_packet(s,0xe1,2,p,31)==31);
}
static void csw(USBMSD *s,int status,unsigned residue) {
    uint8_t p[64]; CHECK(usb_msd_packet(s,0x69,1,p,64)==13);
    CHECK(!memcmp(p,"USBS",4) && le32(p+4)==0xabcdef01 && p[12]==status && le32(p+8)==residue);
}
static void peripheral(void) {
    BlockDev d={read_disk,write_disk,seek_disk,size_disk,flush_disk,disk,0,0};
    USBMSD *s=usb_msd_new(); usb_msd_attach(s,&d,1);
    uint8_t p[64]={0x80,6,0,1,0,0,18,0};
    CHECK(usb_msd_packet(s,0x2d,0,p,8)==8); CHECK(usb_msd_packet(s,0x69,0,p,64)==18);
    CHECK(p[0]==18 && p[7]==64);
    uint8_t product_setup[8]={0x80,6,2,3,9,4,64,0};
    const char *expected_product="USB Storage Device";
    CHECK(usb_msd_packet(s,0x2d,0,product_setup,8)==8);
    CHECK(usb_msd_packet(s,0x69,0,p,64)==2+2*(int)strlen(expected_product));
    for (unsigned index=0;expected_product[index];index++)
        CHECK(p[2+index*2]==expected_product[index] && !p[3+index*2]);
    uint8_t adr[8]={0,5,7}; CHECK(usb_msd_packet(s,0x2d,0,adr,8)==8);
    CHECK(usb_msd_address(s)==0); CHECK(usb_msd_packet(s,0x69,0,p,0)==0);
    CHECK(usb_msd_address(s)==7); configure(s);
    cbw(s,0x12,1,64,0,36); CHECK(usb_msd_packet(s,0x69,1,p,64)==36 && p[1]==0x80); csw(s,0,28);
    cbw(s,0x25,1,8,0,0); CHECK(usb_msd_packet(s,0x69,1,p,64)==8 && be32(p)==15 && be32(p+4)==512); csw(s,0,0);
    /* Windows' generic driver probes with a 252-byte allocation, while a
     * single formatted-capacity descriptor is only 12 bytes. */
    cbw(s,0x23,1,252,0,252);
    CHECK(usb_msd_packet(s,0x69,1,p,64)==12);
    CHECK(p[3]==8 && be32(p+4)==16 && p[8]==2 && p[9]==0 && p[10]==2 && p[11]==0);
    csw(s,0,240);
    for(unsigned i=0;i<8192;i++) disk[i]=(uint8_t)(i*17);
    cbw(s,0x28,1,512,15,1);
    for(unsigned i=0;i<8;i++) { CHECK(usb_msd_packet(s,0x69,1,p,64)==64); CHECK(!memcmp(p,disk+7680+i*64,64)); }
    csw(s,0,0);
    cbw(s,0x28,1,512,16,1); CHECK(usb_msd_packet(s,0x69,1,p,64)==0); csw(s,1,512);
    cbw(s,0x2a,0,512,2,1); memset(p,0x91,64);
    for(int i=0;i<8;i++) CHECK(usb_msd_packet(s,0xe1,2,p,64)==64);
    csw(s,0,0); CHECK(disk[1024]==0x91 && writes==1 && flushes==1);
    d.readonly=1; cbw(s,0x2a,0,512,3,1);
    for(int i=0;i<8;i++) CHECK(usb_msd_packet(s,0xe1,2,p,64)==64);
    csw(s,1,512); CHECK(writes==1); d.readonly=0;
    /* Removing a device in the middle of a write must drop the old packet. */
    cbw(s,0x2a,0,512,3,1); CHECK(usb_msd_packet(s,0xe1,2,p,64)==64);
    usb_msd_attach(s,NULL,0); d.ctx=replacement; usb_msd_attach(s,&d,2); configure(s);
    CHECK(usb_msd_packet(s,0xe1,2,p,64)==USB_RET_STALL && writes==1 && replacement[1536]==0);
    usb_msd_reset(s); configure(s);
    write_fail=1; cbw(s,0x2a,0,512,0,1);
    for(int i=0;i<8;i++) CHECK(usb_msd_packet(s,0xe1,2,p,64)==64);
    csw(s,1,512); CHECK(writes==1); write_fail=0;
    free(s);
}
static int irq_level, irq_line;
static void set_irq(void *p,int n,int level) { (void)p; irq_level=level; irq_line=n; }
static void out(UHCIState *s,unsigned off,unsigned size,uint32_t v) { CHECK(uhci_io(s,0x1000+off,size,1,&v)); }
static uint32_t in(UHCIState *s,unsigned off,unsigned size) { uint32_t v; CHECK(uhci_io(s,0x1000+off,size,0,&v)); return v; }
static void controller(void) {
    PCIBus *bus; int piix; I440FXState *bridge=i440fx_init(&bus,&piix);
    uint8_t *ram=calloc(1,2<<20);
    BlockDev d={read_disk,write_disk,seek_disk,size_disk,flush_disk,disk,0,0};
    UHCIState *s=uhci_new(bus,piix+2,ram,2<<20,NULL,set_irq); CHECK(s);
    i440fx_write_addr(bridge,0,0x80000000u|((piix+2)<<8)|0x20,2);
    i440fx_write_data(bridge,0,0x1001,2);
    i440fx_write_addr(bridge,0,0x80000000u|((piix+2)<<8)|4,2);
    i440fx_write_data(bridge,0,5,1);
    uhci_attach(s,0,&d,1); CHECK((in(s,16,2)&3)==3);
    out(s,8,2,0x5000); out(s,10,2,0x13); CHECK(s->frame_base==0x135000);
    out(s,16,2,0x202); out(s,16,2,4); CHECK(in(s,16,2)&4);
    for(unsigned i=0;i<1024;i++) wr32(ram+0x10000+i*4,0x11002);
    wr32(ram+0x11000,1); wr32(ram+0x11004,0x12000);
    wr32(ram+0x12000,1); wr32(ram+0x12004,ACTIVE|IOC|(3u<<27));
    wr32(ram+0x12008,0x2d|(7u<<21)); wr32(ram+0x1200c,0x13000);
    uint8_t setup[8]={0x80,6,0,1,0,0,18,0}; memcpy(ram+0x13000,setup,8);
    out(s,8,4,0x10000); out(s,4,2,4); out(s,0,2,1);
    uhci_poll(s,1000); uhci_poll(s,2000);
    CHECK(!(rd32(ram+0x12004)&ACTIVE) && (rd32(ram+0x12004)&0x7ff)==7);
    CHECK(rd32(ram+0x11004)==1 && irq_level==1 && irq_line==11);
    CHECK(pci_device_get_config8(s->pci,0xc1)&0x10);
    out(s,2,2,1); CHECK(!irq_level);
    CHECK(!(pci_device_get_config8(s->pci,0xc1)&0x10));
    wr32(ram+0x11004,0x12000); wr32(ram+0x12004,ACTIVE|SPD|(3u<<27));
    wr32(ram+0x12008,0x69|(63u<<21)); uhci_poll(s,3000);
    CHECK((rd32(ram+0x12004)&0x7ff)==17 && ram[0x13000]==18 && (in(s,2,2)&1));
    /* Short packet leaves the QH at the completed TD for software recovery. */
    CHECK(rd32(ram+0x11004)==0x12000);
    /* Inactive IOC descriptors still interrupt and do not hide later QHs. */
    out(s,2,2,1);
    for(unsigned i=0;i<1024;i++) wr32(ram+0x10000+i*4,0x12100);
    wr32(ram+0x12100,0x11002); wr32(ram+0x12104,IOC);
    uhci_poll(s,4000); CHECK(irq_level && (in(s,2,2)&1));
    uhci_attach(s,0,NULL,0); CHECK((in(s,16,2)&3)==2);
    out(s,0,2,2); CHECK(in(s,2,2)&HALTED);
    /* Reject DMA beyond guest RAM before touching the descriptor. */
    out(s,8,4,0xfffff000); out(s,0,2,1); uhci_poll(s,4000); uhci_poll(s,5000);
    CHECK(s->bounds==1 && (in(s,2,2)&8) && !(s->command&RUN));
    /* Cyclic schedules cannot spin forever. */
    out(s,0,2,2); out(s,8,4,0x10000); out(s,0,2,1);
    wr32(ram+0x11000,0x11002); wr32(ram+0x11004,1);
    uhci_poll(s,6000); uhci_poll(s,7000); CHECK(s->frames>=3);
    free(ram);
}
int main(void) { peripheral(); controller(); puts("PASS: USB enumeration/BOT/SCSI, read/write/RO/errors, unplug mid-write, real PCI/UHCI DMA/QH/IRQ/short packet, DMA bounds, cyclic schedule budget"); return 0; }
