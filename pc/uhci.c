/* SPDX-License-Identifier: MIT
 * Intel UHCI 1.1 host controller, two root ports, USB mass-storage only.
 * DMA is restricted to guest RAM; malformed/cyclic schedules are bounded.
 * The physical USB controller and its firmware drivers remain host-owned.
 */
#include "uhci.h"
#include "usb_msd.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct UHCIState {
    PCIDevice *pci;
    uint8_t *ram;
    uint32_t ram_size, base, frame_base;
    uint16_t command, status, intr, frame, port[2];
    uint8_t sof, mapped, connected[2], causes;
    USBMSD *device[2];
    void *pic;
    void (*irq)(void *,int,int);
    uint64_t last_us;
    unsigned irq_line, irq_level, frames, packets, errors, bounds;
    uint32_t reglog[32]; unsigned reglog_count;
    uint32_t fault_addr;
};
enum { RUN=1, RESET=2, HALTED=32, ACTIVE=1u<<23, IOC=1u<<24, SPD=1u<<29 };
static uint32_t rd32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void wr32(uint8_t *p,uint32_t n) { for(int i=0;i<4;i++) p[i]=(uint8_t)(n>>(i*8)); }
static void irq_update(UHCIState *s) {
    unsigned line=pci_device_get_config8(s->pci,0x3c);
    unsigned level=((s->status&2)&&(s->intr&1)) || ((s->causes&1)&&(s->intr&4)) ||
                   ((s->causes&2)&&(s->intr&8)) || (s->status&0x18);
    /* PIIX USB legacy-support bit 12 mirrors the pending USB interrupt.
     * DOS host drivers use it to distinguish this controller on a shared IRQ. */
    uint8_t legacy=pci_device_get_config8(s->pci,0xc1);
    pci_device_set_config8(s->pci,0xc1,(legacy&~0x10)|(level?0x10:0));
    if (s->irq_level && (line!=s->irq_line || !level)) s->irq(s->pic,s->irq_line,0);
    if (line<16 && line!=0 && level && (!s->irq_level || line!=s->irq_line)) s->irq(s->pic,line,1);
    s->irq_line=line; s->irq_level=level && line<16 && line!=0;
}
void uhci_reset(UHCIState *s) {
    if (!s) return;
    if (s->irq_level) s->irq(s->pic,s->irq_line,0);
    s->irq_level=s->command=s->intr=s->frame=s->frame_base=s->causes=0;
    s->status=HALTED; s->sof=64; s->last_us=0;
    for(int i=0;i<2;i++) { s->port[i]=s->connected[i]?3:0; usb_msd_reset(s->device[i]); }
}
void uhci_attach(UHCIState *s,int port,BlockDev *dev,unsigned serial) {
    if (!s || port<0 || port>1) return;
    /* Reset/abort BOT before the host reuses a BlockDev binding. */
    usb_msd_attach(s->device[port],dev,serial);
    s->connected[port]=dev!=NULL; s->port[port]=(dev?1:0)|2|(s->port[port]&8);
    printf("usb: port %d %s\n",port+1,dev?"connected":"disconnected");
}
static void map(void *opaque,int bar,uint32_t addr,bool enabled) {
    UHCIState *s=opaque; (void)bar;
    s->mapped=enabled && addr && addr<=0xffe0; s->base=addr;
}
UHCIState *uhci_new(PCIBus *bus,int devfn,uint8_t *ram,uint32_t size,
                     void *pic,void (*irq)(void *,int,int)) {
    UHCIState *s=calloc(1,sizeof(*s)); if(!s) return NULL;
    s->ram=ram; s->ram_size=size; s->pic=pic; s->irq=irq;
    s->device[0]=usb_msd_new(); s->device[1]=usb_msd_new();
    if(!s->device[0]||!s->device[1]) { free(s->device[0]); free(s->device[1]); free(s); return NULL; }
    s->pci=pci_register_device(bus,"PIIX3 USB UHCI",devfn,0x8086,0x7020,1,0x0c03);
    if(!s->pci) { free(s->device[0]); free(s->device[1]); free(s); return NULL; }
    pci_register_bar(s->pci,4,32,1,s,map);
    pci_device_set_config8(s->pci,0x3d,4); /* PIIX3 USB uses INTD */
    pci_device_set_fixed_irq(s->pci,11);
    pci_device_set_config8(s->pci,0x60,0x10); /* USB 1.0 revision */
    pci_device_set_config8(s->pci,0x6a,1); /* USB clock enabled */
    uhci_reset(s); return s;
}
static uint8_t *dma(UHCIState *s,uint32_t addr,unsigned n) {
    /* No VGA, option ROM, MMIO or host memory may be a USB DMA target. */
    if(addr>s->ram_size || n>s->ram_size-addr ||
       (n && addr<0x100000 && (uint64_t)addr+n>0xa0000)) {
        s->fault_addr=addr; s->bounds++; s->status|=8|HALTED; s->command&=~RUN; return NULL;
    }
    return s->ram+addr;
}
/* Result: 1 completed, 0 inactive/NAK, -1 error, 2 short packet with SPD. */
static int transfer(UHCIState *s,uint8_t *td) {
    uint32_t st=rd32(td+4), tok=rd32(td+8);
    if(!(st&ACTIVE)) {
        if(st&IOC) { s->status|=1; s->causes|=1; }
        return 0;
    }
    unsigned n=(((tok>>21)&0x7ff)+1)&0x7ff;
    unsigned pid=tok&255, addr=(tok>>8)&127, ep=(tok>>15)&15;
    uint8_t data[64];
    if(n>64 || (pid!=0x69 && pid!=0xe1 && pid!=0x2d)) {
        st=(st&~ACTIVE)|(1u<<20); wr32(td+4,st); s->status|=2; s->errors++; return -1;
    }
    uint8_t *buffer=n?dma(s,rd32(td+12),n):NULL;
    if(n&&!buffer) return -1;
    if(pid!=0x69 && n) memcpy(data,buffer,n);
    int result=USB_RET_NODEV;
    for(int i=0;i<2;i++) {
        if(!s->connected[i] || !(s->port[i]&4) || (s->port[i]&0x1200)) continue;
        if(usb_msd_address(s->device[i])==(int)addr) {
            result=usb_msd_packet(s->device[i],pid,ep,data,n); break;
        }
    }
    s->packets++;
    st&=~(0x7ffu|(0x3fu<<17));
    if(result==USB_RET_NAK) { wr32(td+4,st|(1u<<19)|0x7ff); return 0; }
    if(result==USB_RET_NODEV) {
        unsigned retry=(st>>27)&3;
        if(retry) { st=(st&~(3u<<27))|((retry-1)<<27); }
        if(retry>1) { wr32(td+4,st|(1u<<18)|0x7ff); return 0; }
        st|=1u<<18;
    } else if(result<0) st|=1u<<22;
    st&=~ACTIVE;
    if(result<0) {
        if(st&IOC) { s->status|=1; s->causes|=1; }
        wr32(td+4,st|0x7ff); s->status|=2; s->errors++; return -1;
    }
    if(pid==0x69 && result) memcpy(buffer,data,(unsigned)result);
    st|=(result-1)&0x7ff;
    wr32(td+4,st);
    if(st&IOC) { s->status|=1; s->causes|=1; }
    if((unsigned)result<n && (st&SPD)) { s->status|=1; s->causes|=2; return 2; }
    return 1;
}
static void schedule(UHCIState *s) {
    uint8_t *fl=dma(s,s->frame_base+(s->frame&1023)*4,4); if(!fl) return;
    uint32_t link=rd32(fl), horizontal[16]; unsigned depth=0;
    uint8_t *qh=NULL;
    /* Each frame is finite, even when the guest gives us a cyclic graph. */
    for(unsigned budget=0;budget<4096;budget++) {
        if(link&1) {
next_horizontal:
            if(!depth) break;
            link=horizontal[--depth]; qh=NULL; continue;
        }
        if(link&2) {
            uint8_t *next=dma(s,link&~15u,8); if(!next) break;
            if(depth==16) break;
            horizontal[depth++]=rd32(next); qh=next; link=rd32(next+4);
            continue;
        }
        uint8_t *td=dma(s,link&~15u,16); if(!td) break;
        uint32_t next=rd32(td);
        int ret=transfer(s,td);
        if(ret<=0 || ret==2) {
            if(!qh) { link=next; continue; }
            goto next_horizontal;
        }
        if(qh) wr32(qh+4,next);
        if(qh && !(next&4)) goto next_horizontal;
        link=next;
    }
}
void uhci_poll(UHCIState *s,uint64_t now) {
    if(!s) return;
    if(!(s->command&RUN) || (s->command&8) || !(pci_device_get_config8(s->pci,4)&4)) {
        s->last_us=now; if(!(s->command&RUN)) s->status|=HALTED; irq_update(s); return;
    }
    s->status&=~HALTED;
    if(!s->last_us) s->last_us=now;
    unsigned frames=(unsigned)((now-s->last_us)/1000);
    if(frames>8) { frames=8; s->last_us=now-8000; }
    while(frames-- && (s->command&RUN)) {
        s->last_us+=1000; schedule(s); s->frame=(s->frame+1)&2047; s->frames++;
    }
    irq_update(s);
}
static uint32_t reg_read(UHCIState *s,unsigned off) {
    switch(off) {
    case 0: return s->command; case 2: return s->status; case 4: return s->intr;
    case 6: return s->frame; case 8: return s->frame_base; case 12: return s->sof;
    case 16: case 18: { unsigned i=(off-16)/2; return s->port[i]|0x80|(s->connected[i]?0x10:0); }
    default: return 0;
    }
}
static void reg_write(UHCIState *s,unsigned off,uint32_t val) {
    s->reglog[s->reglog_count++%32]=(off<<16)|(val&65535);
    switch(off) {
    case 0:
        if(val&RESET) { uhci_reset(s); return; }
        s->command=val&0xff;
        if(val&RUN) s->status&=~HALTED; else s->status|=HALTED;
        if(val&4) for(int i=0;i<2;i++) usb_msd_reset(s->device[i]);
        break;
    case 2:
        s->status&=~(val&0x1f); if(val&1) s->causes=0; break;
    case 4: s->intr=val&15; break;
    case 6: if(!(s->command&RUN)) s->frame=val&2047; break;
    case 8: s->frame_base=val&~4095u; break;
    case 12: s->sof=val&127; break;
    case 16: case 18: {
        unsigned i=(off-16)/2; uint16_t old=s->port[i];
        s->port[i]=(old&~(val&10))&~0x1244;
        s->port[i]|=val&0x1244;
        if(!s->connected[i] || (val&512)) s->port[i]&=~4;
        if((old&4)!=(s->port[i]&4)) s->port[i]|=8;
        if((val&512) && !(old&512)) usb_msd_reset(s->device[i]);
        break;
    }
    }
    irq_update(s);
}
bool uhci_io(UHCIState *s,unsigned port,unsigned size,int write,uint32_t *v) {
    if(!s||!s->mapped||port<s->base||port-s->base>=32) return false;
    unsigned off=port-s->base;
    if(size!=1 && size!=2 && size!=4) return false;
    if(off+size>32) { if(!write) *v=0xffffffffu; return true; }
    /* Registers are words, except the dword frame base and byte SOFMOD. */
    if(off>=8 && off+size<=12) {
        unsigned shift=(off-8)*8; uint32_t mask=size==4?0xffffffffu:((1u<<(size*8))-1);
        if(write) reg_write(s,8,(s->frame_base&~(mask<<shift))|((*v&mask)<<shift));
        else *v=(s->frame_base>>shift)&mask;
    } else if(size==4) {
        if(write) { reg_write(s,off,*v&65535); reg_write(s,off+2,*v>>16); }
        else *v=reg_read(s,off)|(reg_read(s,off+2)<<16);
    } else if(size==2) {
        if(write) reg_write(s,off,*v); else *v=reg_read(s,off);
    } else {
        unsigned base=off&~1u, shift=(off&1)*8;
        if(write) {
            uint32_t w=(*v&255)<<shift;
            if(base!=2) w|=reg_read(s,base)&(shift?255:0xff00);
            reg_write(s,base,w);
        } else *v=(reg_read(s,base)>>shift)&255;
    }
    return true;
}
void uhci_report(UHCIState *s) {
    if(!s) { printf("usb: UHCI disabled\n"); return; }
    printf("usb: UHCI base %x mapped %u cmd %x sts %x intr %x frame %u ports %x/%x, "
           "frames %u packets %u errors %u bounds %u irq %u/%u\n",s->base,s->mapped,s->command,
           s->status,s->intr,s->frame,reg_read(s,16),reg_read(s,18),s->frames,s->packets,s->errors,s->bounds,s->irq_line,s->irq_level);
    printf("usb: PCI command %x, writes %u:",pci_device_get_config8(s->pci,4),s->reglog_count);
    unsigned start=s->reglog_count>16?s->reglog_count-16:0;
    for(unsigned i=start;i<s->reglog_count;i++) printf(" %x=%x",s->reglog[i%32]>>16,s->reglog[i%32]&65535);
    printf("\n");
    printf("usb: frame list %x fault %x RAM %x\n",s->frame_base,s->fault_addr,s->ram_size);
    for (int port=0;port<2;port++) if (s->connected[port]) usb_msd_report(s->device[port]);
}
