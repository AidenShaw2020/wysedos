#ifndef GUEST_EDID_H
#define GUEST_EDID_H
#include <stdint.h>
#include <stdbool.h>
bool guest_edid_set(const uint8_t *data,unsigned len);
const uint8_t *guest_edid_page(void);
uint32_t guest_edid_read(unsigned offset,unsigned size);
bool guest_vga_bind(uint8_t *rom,unsigned size,uint32_t bdf);
#endif
