// Called without pico.h (vga.c), so a real function as well as the inline one.
#include <stdint.h>
uint32_t get_uticks(void);
uint32_t get_uticks_fast(void) { return get_uticks(); }
