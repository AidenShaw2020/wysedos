/* SPDX-License-Identifier: MIT */
#ifndef WYSE_UHCI_H
#define WYSE_UHCI_H
#include <stdint.h>
#include <stdbool.h>
#include "pci.h"
#include "blockdev.h"
typedef struct UHCIState UHCIState;
UHCIState *uhci_new(PCIBus *bus, int devfn, uint8_t *ram, uint32_t ram_size,
                     void *pic, void (*irq)(void *,int,int));
void uhci_reset(UHCIState *s);
void uhci_attach(UHCIState *s, int port, BlockDev *dev, unsigned serial);
void uhci_poll(UHCIState *s, uint64_t now_us);
bool uhci_io(UHCIState *s, unsigned port, unsigned size, int write, uint32_t *value);
void uhci_report(UHCIState *s);
#endif
