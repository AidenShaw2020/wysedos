/* SPDX-License-Identifier: MIT
 * USB-IF Mass Storage Bulk-Only Transport 1.0, SCSI transparent command set.
 * Only the BlockDev is shared with the host. Guest requests never reach the
 * physical host controller, HID devices, floppy or CD protocols.
 */
#include "usb_msd.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct USBMSD {
    BlockDev *dev;
    unsigned serial, address, configuration, pending_address;
    uint8_t control[256], setup[8];
    unsigned ctrl_len, ctrl_pos, ctrl_in, set_address;
    uint8_t cbw[31], csw[13], reply[256], sector[512];
    unsigned cbw_pos, csw_pos, stage, direction, host_len, expected, done;
    unsigned reply_len, disk_io, writing, sector_pos, processed;
    uint64_t offset;
    uint8_t status, sense, asc, locked, halt_in, halt_out;
    uint32_t setup_log[16][2];
    uint32_t scsi_log[16][6];
    unsigned setup_count, scsi_count, last_scsi;
};
enum { CBW, DATA, STATUS, RECOVERY };
static uint32_t le32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint16_t be16(const uint8_t *p) { return p[0]*256u+p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p)<<16|be16(p+2); }
static void putle32(uint8_t *p, uint32_t n) { for (int i=0;i<4;i++) p[i]=(uint8_t)(n>>(i*8)); }
static void putbe32(uint8_t *p, uint32_t n) { for (int i=0;i<4;i++) p[3-i]=(uint8_t)(n>>(i*8)); }
static unsigned minimum(unsigned a, unsigned b) { return a<b?a:b; }
static void bot_reset(USBMSD *s) {
    s->stage=CBW; s->cbw_pos=s->csw_pos=s->sector_pos=0;
    s->host_len=s->done=s->expected=0; s->disk_io=s->status=0;
}
USBMSD *usb_msd_new(void) { return calloc(1,sizeof(USBMSD)); }
void usb_msd_reset(USBMSD *s) {
    if (!s) return;
    s->address=s->configuration=s->set_address=0;
    s->ctrl_len=s->ctrl_pos=s->halt_in=s->halt_out=s->locked=0;
    bot_reset(s);
}
void usb_msd_attach(USBMSD *s, BlockDev *dev, unsigned serial) {
    if (!s) return;
    usb_msd_reset(s); s->dev=dev; s->serial=serial; s->sense=s->asc=0;
}
int usb_msd_address(const USBMSD *s) { return s ? (int)s->address : -1; }
static void fail(USBMSD *s, unsigned sense, unsigned asc) {
    s->status=1; s->sense=sense; s->asc=asc; s->expected=0; s->disk_io=0;
}
static void status_start(USBMSD *s) {
    memcpy(s->csw,"USBS",4); memcpy(s->csw+4,s->cbw+4,4);
    putle32(s->csw+8,s->host_len-s->processed); s->csw[12]=s->status;
    s->csw_pos=0; s->stage=STATUS;
}
static int medium(USBMSD *s) {
    if (blk_size(s->dev)>=512) return 1;
    fail(s,2,0x3a); return 0;
}
static void scsi(USBMSD *s) {
    const uint8_t *c=s->cbw+15;
    unsigned op=c[0], allocation=0;
    s->last_scsi=op; s->scsi_count++;
    memset(s->reply,0,sizeof(s->reply));
    s->expected=s->reply_len=s->done=s->disk_io=s->sector_pos=0;
    s->processed=0;
    s->status=0; s->writing=0;
    switch(op) {
    case 0x00: medium(s); break; /* TEST UNIT READY */
    case 0x03: /* REQUEST SENSE */
        s->reply[0]=0x70; s->reply[2]=s->sense; s->reply[7]=10; s->reply[12]=s->asc;
        s->reply_len=18; allocation=c[4]; s->sense=s->asc=0; break;
    case 0x12: /* INQUIRY */
        if (c[1]&1) { fail(s,5,0x24); break; }
        s->reply[0]=0; s->reply[1]=0x80; s->reply[2]=2; s->reply[3]=2; s->reply[4]=31;
        memcpy(s->reply+8,"wyseDOS ",8); memcpy(s->reply+16,"USB Storage     ",16);
        memcpy(s->reply+32,"1.00",4); s->reply_len=36; allocation=c[4]; break;
    case 0x1a: case 0x5a: { /* MODE SENSE: write protection + all pages */
        if (!medium(s)) break;
        unsigned ten=op==0x5a, n=ten?8:4;
        if (ten) { s->reply[1]=6; s->reply[3]=s->dev->readonly?0x80:0; allocation=be16(c+7); }
        else { s->reply[0]=3; s->reply[2]=s->dev->readonly?0x80:0; allocation=c[4]; }
        s->reply_len=n; break;
    }
    case 0x23: /* READ FORMAT CAPACITIES */
        if (!medium(s)) break;
        s->reply[3]=8; putbe32(s->reply+4,(uint32_t)(blk_size(s->dev)/512));
        s->reply[8]=2; s->reply[10]=2; s->reply_len=12; allocation=be16(c+7); break;
    case 0x25: /* READ CAPACITY(10) */
        if (!medium(s)) break;
        putbe32(s->reply,(uint32_t)(blk_size(s->dev)/512-1));
        putbe32(s->reply+4,512); s->reply_len=allocation=8; break;
    case 0x28: case 0xa8: case 0x2a: case 0xaa: {
        if (!medium(s)) break;
        uint64_t lba=be32(c+2), count=(op==0xa8||op==0xaa)?be32(c+6):be16(c+7);
        uint64_t total=blk_size(s->dev)/512;
        if (lba>total || count>total-lba || count>0x7fffff) { fail(s,5,0x21); break; }
        s->writing=op==0x2a||op==0xaa;
        if (s->writing && s->dev->readonly) { fail(s,7,0x27); break; }
        s->offset=lba*512; s->expected=(unsigned)(count*512); s->disk_io=1; break;
    }
    case 0x35: /* SYNCHRONIZE CACHE */
        if (medium(s) && !s->dev->readonly && blk_sync(s->dev)<0) fail(s,3,0x0c);
        break;
    case 0x1b: /* START STOP UNIT: flush on stop/eject */
        if (!(c[4]&1) && medium(s) && !s->dev->readonly && blk_sync(s->dev)<0) fail(s,3,0x0c);
        break;
    case 0x1e: s->locked=c[4]&1; break;
    default: fail(s,5,0x20); break;
    }
    if (s->reply_len) { s->reply_len=minimum(s->reply_len,allocation); s->expected=s->reply_len; }
    if (s->expected && (s->direction ? s->writing : !s->writing)) {
        s->status=2; s->expected=0; s->disk_io=0;
    }
    if (s->host_len<s->expected) { s->expected=s->host_len; s->status=2; }
    uint32_t *entry=s->scsi_log[(s->scsi_count-1)%16];
    entry[0]=le32(c); entry[1]=le32(c+4); entry[2]=le32(c+8);
    entry[3]=s->host_len; entry[4]=s->expected;
    entry[5]=s->status|(s->direction<<8)|((unsigned)s->sense<<16)|((unsigned)s->asc<<24);
    s->stage=s->host_len?DATA:STATUS;
    if (!s->host_len) status_start(s);
}
static int bulk_out(USBMSD *s, const uint8_t *p, unsigned n) {
    if (s->halt_out || s->stage==RECOVERY) return USB_RET_STALL;
    if (s->stage==CBW) {
        if (n>31-s->cbw_pos) goto invalid;
        memcpy(s->cbw+s->cbw_pos,p,n); s->cbw_pos+=n;
        if (s->cbw_pos<31) return n;
        if (memcmp(s->cbw,"USBC",4) || (s->cbw[12]&0x7f) || s->cbw[13] ||
            s->cbw[14]<1 || s->cbw[14]>16) goto invalid;
        s->host_len=le32(s->cbw+8); s->direction=s->cbw[12]>>7;
        scsi(s); return n;
    }
    if (s->stage!=DATA || s->direction) return USB_RET_STALL;
    if (n>s->host_len-s->done) goto invalid;
    /* A failed OUT command drains the host's remaining data, without writes. */
    if (s->disk_io && s->writing && s->done<s->expected) {
        unsigned take=minimum(n,s->expected-s->done);
        unsigned pos=0;
        while(pos<take && s->disk_io) {
            unsigned part=minimum(take-pos,512-s->sector_pos);
            memcpy(s->sector+s->sector_pos,p+pos,part); s->sector_pos+=part; pos+=part;
            if (s->sector_pos==512) {
                if (blk_pwrite(s->dev,s->offset,s->sector,512)!=512) fail(s,3,0x0c);
                else { s->offset+=512; s->processed+=512; }
                s->sector_pos=0;
            }
        }
    }
    s->done+=n;
    if (s->done==s->host_len) {
        if (s->sector_pos) { s->sector_pos=0; s->status=2; }
        if (s->disk_io && s->writing && blk_sync(s->dev)<0) fail(s,3,0x0c);
        status_start(s);
    }
    return n;
invalid:
    s->stage=RECOVERY; s->halt_in=s->halt_out=1; return USB_RET_STALL;
}
static int bulk_in(USBMSD *s, uint8_t *p, unsigned n) {
    if (s->halt_in || s->stage==RECOVERY) return USB_RET_STALL;
    if (s->stage==STATUS) {
        unsigned take=minimum(n,13-s->csw_pos); memcpy(p,s->csw+s->csw_pos,take); s->csw_pos+=take;
        if (s->csw_pos==13) bot_reset(s);
        return take;
    }
    if (s->stage!=DATA || !s->direction) return USB_RET_NAK;
    unsigned take=minimum(n,s->expected-s->done);
    if (s->disk_io && take) {
        if (!s->sector_pos && blk_pread(s->dev,s->offset,s->sector,512)!=512) {
            fail(s,3,0x11); status_start(s); return 0;
        }
        take=minimum(take,512-s->sector_pos); memcpy(p,s->sector+s->sector_pos,take);
        s->sector_pos+=take;
        if (s->sector_pos==512) { s->sector_pos=0; s->offset+=512; }
    } else if (take) memcpy(p,s->reply+s->done,take);
    s->done+=take;
    s->processed+=take;
    if (s->done==s->expected && (s->done==s->host_len || take<n)) status_start(s);
    return take;
}
static unsigned string_descriptor(USBMSD *s, unsigned index) {
    char serial[24]; snprintf(serial,sizeof(serial),"WYSEUSB%08u",s->serial);
    const char *v=index==1?"wyseDOS":index==2?"USB Storage Device":index==3?serial:NULL;
    if (!index) { s->control[0]=4; s->control[1]=3; s->control[2]=9; s->control[3]=4; return 4; }
    if (!v) return 0;
    unsigned n=(unsigned)strlen(v); s->control[0]=2+n*2; s->control[1]=3;
    for (unsigned i=0;i<n;i++) { s->control[2+i*2]=v[i]; s->control[3+i*2]=0; }
    return 2+n*2;
}
static int setup(USBMSD *s, const uint8_t *p, unsigned n) {
    static const uint8_t device[]={18,1,0x10,1,0,0,0,64,0x34,0x12,0x50,0xd0,0,1,1,2,3,1};
    static const uint8_t config[]={9,2,32,0,1,1,0,0x80,50,
        9,4,0,0,2,8,6,0x50,0, 7,5,0x81,2,64,0,0, 7,5,2,2,64,0,0};
    if (n!=8) return USB_RET_STALL;
    s->setup_log[s->setup_count%16][0]=le32(p);
    s->setup_log[s->setup_count++%16][1]=le32(p+4);
    memcpy(s->setup,p,8); s->ctrl_pos=s->ctrl_len=s->set_address=0;
    s->ctrl_in=p[0]>>7;
    unsigned value=p[2]|p[3]<<8, index=p[4]|p[5]<<8, length=p[6]|p[7]<<8;
    if (p[0]==0x80 && p[1]==6) {
        switch(value>>8) {
        case 1: memcpy(s->control,device,sizeof(device)); s->ctrl_len=sizeof(device); break;
        case 2: memcpy(s->control,config,sizeof(config)); s->ctrl_len=sizeof(config); break;
        case 3: s->ctrl_len=string_descriptor(s,value&255); break;
        default: return USB_RET_STALL;
        }
        if (!s->ctrl_len) return USB_RET_STALL;
    } else if (!p[0] && p[1]==5 && value<128 && !length) {
        s->pending_address=value; s->set_address=1;
    } else if (!p[0] && p[1]==9 && value<=1 && !length) {
        s->configuration=value; bot_reset(s);
    } else if (p[0]==0x80 && p[1]==8) {
        s->control[0]=s->configuration; s->ctrl_len=1;
    } else if ((p[0]==0x80 || p[0]==0x81 || p[0]==0x82) && p[1]==0) {
        s->control[0]=p[0]==0x82?((index&0x80)?s->halt_in:s->halt_out):0;
        s->control[1]=0; s->ctrl_len=2;
    } else if (p[0]==2 && p[1]==1 && !value && !length) {
        if (index==0x81) s->halt_in=0; else if (index==2) s->halt_out=0; else return USB_RET_STALL;
    } else if (p[0]==0xa1 && p[1]==0xfe && !value && !index && length==1) {
        s->control[0]=0; s->ctrl_len=1;
    } else if (p[0]==0x21 && p[1]==0xff && !value && !index && !length) {
        bot_reset(s);
    } else if (p[0]==0x81 && p[1]==10) { s->control[0]=0; s->ctrl_len=1;
    } else if (p[0]==1 && p[1]==11 && !value && !index && !length) {
        /* only alternate setting zero */
    } else return USB_RET_STALL;
    s->ctrl_len=minimum(s->ctrl_len,length);
    return 8;
}
int usb_msd_packet(USBMSD *s, unsigned pid, unsigned ep, uint8_t *p, unsigned n) {
    if (!s || !s->dev) return USB_RET_NODEV;
    if (n>64) return USB_RET_STALL;
    if (!ep) {
        if (pid==0x2d) return setup(s,p,n);
        if (pid==0x69) {
            unsigned take=minimum(n,s->ctrl_len-s->ctrl_pos);
            memcpy(p,s->control+s->ctrl_pos,take); s->ctrl_pos+=take;
            if (!s->ctrl_in && !take && s->set_address) { s->address=s->pending_address; s->set_address=0; }
            return take;
        }
        if (pid==0xe1 && !n) return 0;
        return USB_RET_STALL;
    }
    if (!s->configuration) return USB_RET_STALL;
    if (ep==1 && pid==0x69) return bulk_in(s,p,n);
    if (ep==2 && pid==0xe1) return bulk_out(s,p,n);
    return USB_RET_STALL;
}

void usb_msd_report(USBMSD *s) {
    if (!s) return;
    printf("usb: device addr %u config %u stage %u SCSI %u last %x sense %x/%x, setup %u:\n",
           s->address,s->configuration,s->stage,s->scsi_count,s->last_scsi,s->sense,s->asc,s->setup_count);
    unsigned first=s->setup_count>16?s->setup_count-16:0;
    for (unsigned index=first;index<s->setup_count;index++)
        printf("usb: setup %08x %08x\n",s->setup_log[index%16][0],s->setup_log[index%16][1]);
    first=s->scsi_count>16?s->scsi_count-16:0;
    for (unsigned index=first;index<s->scsi_count;index++) {
        uint32_t *entry=s->scsi_log[index%16];
        printf("usb: scsi %08x %08x %08x host %u expected %u flags %x\n",
               entry[0],entry[1],entry[2],entry[3],entry[4],entry[5]);
    }
}
