#ifndef WYSE_NATIVE_DISK_H
#define WYSE_NATIVE_DISK_H
#include "bio.h"
#include "blockdev.h"
/* A private GPT container. Its contents, including its own MBR, belong to DOS.
 * Neither the host GPT nor the ESP is ever addressable through this device. */
#define WYSE_GUEST_TYPE {0xD05D3040, 0xA11C, 0x4D98, {0xB2,0x40,0x57,0x59,0x53,0x45,0x44,0x53}}
int native_disk_bind(BlockDev *d);
void native_disk_test(void);
#endif
