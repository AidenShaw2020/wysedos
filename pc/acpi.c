/* ACPI 1.0 fixed hardware and read-only fw_cfg for the guest SeaBIOS.
 * Tables are allocated/reserved/relocated by the BIOS table-loader, never
 * inserted into guest-owned RAM or arbitrary padding inside its ROM. */
#include "pc.h"
#include "acpi.h"
#include <stdio.h>
#include <string.h>
#include "acpi_tables.h"

static PC *machine;
static uint16_t selector,pm_status,pm_enable,pm_control;
static uint32_t offset,last_us,timer_value;
static uint64_t elapsed_us;
static uint8_t apm_command;
static unsigned enables,sleep_writes,fw_reads;
typedef struct { const char *name;const uint8_t *data;unsigned len; } fw_file;
static const fw_file files[]={
    {"etc/acpi/tables",acpi_tables,sizeof(acpi_tables)},
    {"etc/acpi/rsdp",acpi_rsdp,sizeof(acpi_rsdp)},
    {"etc/table-loader",acpi_loader,sizeof(acpi_loader)}
};
static uint8_t directory_byte(unsigned p) {
    if (p<4) return p==3?3:0; // big-endian count
    p-=4;unsigned i=p/64,k=p%64;if (i>=3) return 0;
    if (k<4) return files[i].len>>(8*(3-k));
    if (k<6) return k==5?0x20+i:0;
    if (k<8) return 0;
    k-=8;return k<strlen(files[i].name)?files[i].name[k]:0;
}
static uint8_t fw_byte(void) {
    unsigned p=offset++;fw_reads++;
    if (selector==0) return p<4?"QEMU"[p]:0;
    if (selector==1) return p==0?1:0; // PIO supported, DMA not advertised
    if (selector==3) return p<8?(uint64_t)machine->phys_mem_size>>(p*8):0;
    if (selector==5 || selector==0x0f || selector==0x0e) return p==0?1:0;
    if (selector==0x19) return directory_byte(p);
    if (selector>=0x20 && selector<0x23) {
        const fw_file *f=&files[selector-0x20];return p<f->len?f->data[p]:0;
    }
    return 0;
}
static void sci_update(void) {
    if (machine) i8259_set_irq(machine->pic,9,(pm_control&1) && (pm_status&pm_enable&0x0501));
}
void acpi_step(void) {
    if (!machine) return;
    uint32_t now=get_uticks();elapsed_us+=(uint32_t)(now-last_us);last_us=now;
    uint32_t next=(elapsed_us*3579545ull/1000000ull)&0xffffff;
    if ((next^timer_value)&0x800000) pm_status|=1;
    timer_value=next;sci_update();
}
void acpi_reset(PC *pc) {
    machine=pc;selector=offset=0;pm_status=pm_enable=pm_control=0;
    last_us=get_uticks();elapsed_us=timer_value=0;apm_command=0;
    enables=sleep_writes=fw_reads=0;sci_update();
}
bool acpi_io_read(uint16_t port,unsigned size,uint32_t *value) {
    if (port==0x511 && size==1) { *value=fw_byte();return true; }
    if (port==0xb2 && size==1) { *value=apm_command;return true; }
    if (port<0x600 || (unsigned)port+size>0x640 || (size!=1 && size!=2 && size!=4)) return false;
    acpi_step();uint32_t v=0;
    for (unsigned i=0;i<size;i++) {
        unsigned p=port-0x600+i;uint8_t b=0;
        if (p<2) b=pm_status>>(8*p);
        else if (p<4) b=pm_enable>>(8*(p-2));
        else if (p<6) b=pm_control>>(8*(p-4));
        else if (p>=8 && p<12) b=timer_value>>(8*(p-8));
        v|=(uint32_t)b<<(8*i);
    }
    *value=v;return true;
}
bool acpi_io_write(uint16_t port,unsigned size,uint32_t value) {
    if (port==0x510 && size==2) { selector=value;offset=0;return true; }
    if (port==0x511 && size==1) return true;
    if (port==0xb2 && size==1) {
        apm_command=value;
        if (value==0xf1) { pm_control|=1;enables++;printf("acpi: OS enabled SCI\n"); }
        if (value==0xf0) pm_control&=~1;
        sci_update();return true;
    }
    if (port<0x600 || (unsigned)port+size>0x640 || (size!=1 && size!=2 && size!=4)) return false;
    acpi_step();uint16_t cnt=pm_control;bool cnt_written=false;
    for (unsigned i=0;i<size;i++) {
        unsigned p=port-0x600+i;uint8_t b=value>>(8*i);
        if (p<2) pm_status&=~((uint16_t)b<<(8*p)); // W1C
        else if (p<4) { unsigned sh=8*(p-2);pm_enable=(pm_enable&~(0xffu<<sh))|((uint16_t)b<<sh); }
        else if (p<6) { unsigned sh=8*(p-4);cnt=(cnt&~(0xffu<<sh))|((uint16_t)b<<sh);cnt_written=true; }
    }
    if (cnt_written) {
        pm_control=(cnt&0x1c00)|(pm_control&1); // SLP_EN write-only, SCI_EN read-only
        if (cnt&0x2000) {
            unsigned type=(cnt>>10)&7;sleep_writes++;
            /* Match _S5 and the PIIX4 fixed-hardware interface used by
             * SeaBIOS APM. Both ACPI and legacy APM reach the same S5. */
            printf("acpi: SLP_EN, type %u%s\n",type,type==0?" (S5 shutdown)":" unsupported");
            if (type==0) machine->shutdown_state=8;
        }
    }
    sci_update();return true;
}
void acpi_report(void) {
    acpi_step();
    printf("acpi: PM 0600, status %04x enable %04x control %04x timer %06x, SCI enables %u sleep writes %u fw reads %u\n",
           pm_status,pm_enable,pm_control,timer_value,enables,sleep_writes,fw_reads);
    if (!machine) return;
    /* Read-only verification of the RSDP actually installed by SeaBIOS. */
    for (unsigned p=0xe0000;p+20<=0x100000;p+=16) {
        const uint8_t *r=(const uint8_t *)machine->phys_mem+p;
        if (memcmp(r,"RSD PTR ",8)) continue;
        unsigned sum=0;for (unsigned i=0;i<20;i++) sum+=r[i];
        uint32_t rsdt;memcpy(&rsdt,r+16,4);
        printf("acpi: RSDP at %05x checksum %s, RSDT %08x\n",p,(sum&255)?"BAD":"PASS",rsdt);
    }
}
