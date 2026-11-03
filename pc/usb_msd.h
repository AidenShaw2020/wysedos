/* USB 1.1 mass-storage peripheral, independent of the physical USB bus. */
#ifndef WYSE_USB_MSD_H
#define WYSE_USB_MSD_H
#include "blockdev.h"
#define USB_RET_STALL (-1)
#define USB_RET_NAK   (-2)
#define USB_RET_NODEV (-3)
typedef struct USBMSD USBMSD;
USBMSD *usb_msd_new(void);
void usb_msd_reset(USBMSD *s);
void usb_msd_attach(USBMSD *s, BlockDev *dev, unsigned serial);
int usb_msd_address(const USBMSD *s);
void usb_msd_report(USBMSD *s);
int usb_msd_packet(USBMSD *s, unsigned pid, unsigned endpoint, uint8_t *data, unsigned len);
#endif
