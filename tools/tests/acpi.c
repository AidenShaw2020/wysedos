/* Actual guest ACPI module: PIO firmware files, PM timer/SCI/W1C and S5.
 * clang -O2 -Ipc tools/tests/acpi.c -o work/acpi_test.exe */
#include "../../pc/acpi.c"
#include <stdlib.h>
static uint32_t now;static int irq_level;
uint32_t get_uticks(void) { return now; }
void i8259_set_irq(PicState2 *p,int irq,int level) { (void)p;if (irq!=9) abort();irq_level=!!level; }
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n",__LINE__);return 1; } } while(0)
static uint32_t readp(uint16_t p,unsigned n) { uint32_t v;if (!acpi_io_read(p,n,&v)) abort();return v; }
int main(void) {
    PC pc={0};pc.phys_mem_size=16<<20;acpi_reset(&pc);
    acpi_io_write(0x510,2,0);CHECK(readp(0x511,1)=='Q' && readp(0x511,1)=='E');
    acpi_io_write(0x510,2,0);CHECK(readp(0x511,1)=='Q'); // selector rewinds
    acpi_io_write(0x510,2,3);uint64_t mem=0;
    for (unsigned i=0;i<8;i++) mem|=(uint64_t)readp(0x511,1)<<(8*i);
    CHECK(mem==(16<<20) && readp(0x511,1)==0);
    acpi_io_write(0x510,2,0x19);CHECK(readp(0x511,1)==0 && readp(0x511,1)==0 && readp(0x511,1)==0 && readp(0x511,1)==3);
    acpi_io_write(0x510,2,0x22);
    for (unsigned i=0;i<sizeof(acpi_loader);i++) CHECK(readp(0x511,1)==acpi_loader[i]);
    CHECK(readp(0x511,1)==0);
    CHECK(readp(0x604,2)==0);acpi_io_write(0xb2,1,0xf1);CHECK(readp(0x604,2)==1);
    acpi_io_write(0x602,2,1);now=2400000;acpi_step();CHECK(readp(0x600,2)&1);CHECK(irq_level);
    acpi_io_write(0x600,1,1);CHECK(!(readp(0x600,2)&1) && !irq_level);
    acpi_io_write(0x604,2,0);CHECK(pc.shutdown_state==0);
    acpi_io_write(0x604,2,(1<<10)|0x2000);CHECK(pc.shutdown_state==0); // no S1
    acpi_io_write(0x604,2,(5<<10)|0x2000);CHECK(pc.shutdown_state==0);
    acpi_io_write(0x605,1,0x20);CHECK(pc.shutdown_state==8); // S5 via high byte
    CHECK(!(readp(0x604,2)&0x2000));
    acpi_reset(&pc);pc.shutdown_state=0;
    acpi_io_write(0x604,2,0x2000);CHECK(pc.shutdown_state==8); // SeaBIOS APM, SCI disabled
    acpi_reset(&pc);pc.shutdown_state=0;
    now=0xfffffff0;acpi_reset(&pc);now=100;
    CHECK(readp(0x608,4)==(uint32_t)(116ull*3579545/1000000));
    uint32_t v;CHECK(!acpi_io_read(0x640,1,&v) && !acpi_io_write(0x63f,2,0));
    puts("PASS: fw_cfg bytes/directory/EOF, SCI timer and W1C, S5 gating, byte writes, clock wrap");return 0;
}
