#ifndef WYSE_HOSTBLK_H
#define WYSE_HOSTBLK_H
#include "blockdev.h"
void hostblk_usb_start(void (*change)(int port, BlockDev *dev, unsigned serial));
const char *hostblk_usb_name(int port);
int hostblk_usb_eject(int port);
int hostblk_usb_insert(int port, const char *which);
void hostblk_poll(void);
/* Transfer the existing, pinned USB binding to an exclusive image reader.
 * No detach/rebind by ordinal is allowed while images remain open. */
int hostblk_image_info(const char *which,void **identity,uint32_t *media_id);
int hostblk_image_take(const char *which,void *identity,uint32_t media_id,BlockDev *target,int *return_port);
int hostblk_image_return(BlockDev *target, int return_port);
#endif
