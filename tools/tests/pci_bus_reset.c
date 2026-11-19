/* Real PCI config transactions and BAR callbacks; no hardware access. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef PCI_SOURCE_PATH
#define PCI_SOURCE_PATH "../../pc/pci.c"
#endif
#include PCI_SOURCE_PATH
#ifdef PCI_RESET_NEGATIVE
#define pci_bus_save_reset_state(bus) ((void)(bus))
#define pci_bus_reset(bus) ((void)(bus))
#endif

struct Mapping { unsigned on, off; uint32_t address; bool enabled; };
static void map(void *opaque, int bar, uint32_t address, bool enabled)
{
    struct Mapping *m = opaque;
    assert(bar == 0);
    if (enabled) ++m->on; else ++m->off;
    m->address = address; m->enabled = enabled;
}
static void write_config(I440FXState *host, PCIDevice *d,
                         unsigned offset, uint32_t value, int size)
{
    i440fx_write_addr(host, 0, 0x80000000u | (pci_device_get_devfn(d) << 8) |
                      (offset & ~3u), 2);
    i440fx_write_data(host, offset & 3u, value, size);
}
int main(void)
{
    PCIBus *bus;
    int slot;
    I440FXState *host = i440fx_init(&bus, &slot);
    PCIDevice *vga = pci_register_device(bus,"test VGA",-1,0x1234,0x1111,0,0x0300);
    PCIDevice *io = pci_register_device(bus,"test IO",-1,0x1234,0x2222,0,0x0200);
    struct Mapping vm = {0}, im = {0};
    uint8_t initial[256], io_initial[256];
    uint8_t cap[4] = {1,0,0,0};
    pci_register_bar(vga,0,8u<<20,PCI_ADDRESS_SPACE_MEM,&vm,map);
    pci_register_bar(io,0,32,PCI_ADDRESS_SPACE_IO,&im,map);
    pci_device_set_fixed_irq(io,3);
    pci_device_set_config16(vga,PCI_SUBSYSTEM_VENDOR_ID,0x1234);
    pci_device_set_config16(vga,PCI_SUBSYSTEM_ID,0x5747);
    assert(pci_add_capability(io,cap,sizeof(cap))==0x40);
    memcpy(initial,vga->config,256); memcpy(io_initial,io->config,256);
    pci_bus_save_reset_state(bus);
    for(unsigned cycle=0;cycle<3;++cycle) {
        write_config(host,vga,0x10,0xfe000000,2);
        write_config(host,vga,PCI_COMMAND,6,1);
        write_config(host,io,0x10,0x301,2);
        write_config(host,io,PCI_COMMAND,5,1);
        write_config(host,io,0x40,0xdead0001,2);
        assert(vm.enabled && vm.address==0xfe000000);
        assert(im.enabled && im.address==0x300);
        bus->irq_state[0][0]=0xffffffff;
        unsigned voff=vm.off, ioff=im.off;
        pci_bus_reset(bus);
        assert(!vm.enabled && !im.enabled);
        assert(vm.off==voff+1 && im.off==ioff+1);
        assert(!memcmp(initial,vga->config,256));
        assert(!memcmp(io_initial,io->config,256));
        assert(bus->irq_state[0][0]==0);
        /* The next BIOS size probe must report the mask without decoding
         * 0xff800000 over the high BIOS ROM address range. */
        unsigned von=vm.on;
        write_config(host,vga,0x10,0xffffffff,2);
        assert(get_le32(vga->config+0x10)==0xff800000);
        assert(!vm.enabled && vm.on==von);
        write_config(host,vga,0x10,0xfd000000,2);
        write_config(host,vga,PCI_COMMAND,2,1);
        assert(vm.enabled && vm.address==0xfd000000);
        pci_bus_reset(bus);
        assert(!vm.enabled && !memcmp(initial,vga->config,256));
    }
    pci_bus_reset(NULL); pci_bus_save_reset_state(NULL);
    puts("PASS: PCI reset unmaps BARs, restores board defaults and isolates BIOS sizing probes");
    return 0;
}
