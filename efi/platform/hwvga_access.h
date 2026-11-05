#ifndef WYSE_HWVGA_ACCESS_H
#define WYSE_HWVGA_ACCESS_H
#include <stdint.h>

/* Cherry Trail's legacy VGA aperture misreads unaligned words/dwords and
 * drops stores crossing an eight-byte boundary. Volatile byte accesses
 * preserve x86 text-memory semantics without bypassing the VGA hardware. */
static inline uint32_t hwvga_load_bytes(uintptr_t address, unsigned size) {
    const volatile uint8_t *p = (const volatile uint8_t *)address;
    uint32_t value = 0;
    for (unsigned i = 0; i < size; i++) value |= (uint32_t)p[i] << (8 * i);
    return value;
}
static inline void hwvga_store_bytes(uintptr_t address, uint32_t value, unsigned size) {
    volatile uint8_t *p = (volatile uint8_t *)address;
    for (unsigned i = 0; i < size; i++) p[i] = (uint8_t)(value >> (8 * i));
}
#endif
