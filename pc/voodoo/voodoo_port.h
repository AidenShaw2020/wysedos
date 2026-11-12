#ifndef VOODOO_PORT_H
#define VOODOO_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The 86Box x86-64 span recompiler is used unless VOODOO_PORT_NO_JIT is set;
 * its code buffer comes from voodoo2_platform_alloc_exec(). */
#if defined VOODOO_PORT_NO_JIT || !(defined __amd64__ || defined _M_X64)
#define NO_CODEGEN 1
#endif
#define VOODOO_PORT_SYNCHRONOUS 1
#define ATOMIC_INT int
#define ATOMIC_UINT unsigned
#define UNUSED(value) value __attribute__((unused))
#define MIN(left, right) ((left) < (right) ? (left) : (right))
#define MAX(left, right) ((left) > (right) ? (left) : (right))
#define ABS(value) ((value) < 0 ? -(value) : (value))
#define fallthrough __attribute__((fallthrough))
#define bswap32(value) __builtin_bswap32(value)
#define UNLIKELY(value) __builtin_expect(!!(value), 0)

extern int changeframecount;
uint64_t time_us_64(void);
void voodoo_port_tables_init(void);
struct voodoo_t;
void voodoo_port_render_written(struct voodoo_t *device, bool triangle);
/* Draws one set-up triangle (both line halves when the board renders split). */
struct voodoo_params_t;
void voodoo_port_triangle(struct voodoo_t *device, struct voodoo_params_t *params);
void voodoo_port_vram_written(struct voodoo_t *device, uint32_t address, unsigned length);
/* 86Box's voodoo_wait_for_render_thread_idle(): every queued triangle must be
 * in the frame buffer (the GPU back end finishes here). */
void voodoo_port_render_idle(struct voodoo_t *device);
/* texture_cache[tmu][entry].data[first, end) was decoded anew */
void voodoo_port_texture_decoded(struct voodoo_t *device, int tmu, int entry, uint32_t first, uint32_t end);
uint8_t *voodoo_port_vram_pointer(struct voodoo_t *device, uint32_t address, unsigned width);
void voodoo_port_packet(struct voodoo_t *device, uint32_t header);
uint32_t voodoo_port_fifo_word(struct voodoo_t *device);
float voodoo_port_fifo_float(struct voodoo_t *device);
void voodoo_port_fifo_reg(struct voodoo_t *device, uint32_t address, uint32_t value);
void voodoo_port_fifo_2d(struct voodoo_t *device, uint32_t address, uint32_t value);
void voodoo_port_fifo_raw(struct voodoo_t *device, uint32_t address, uint32_t value);
void banshee_cmd_write(void *opaque, uint32_t address, uint32_t value);

typedef struct { uintptr_t unused; } mem_mapping_t;
typedef struct { uintptr_t unused; } pc_timer_t;
typedef struct event_t { bool signaled; } event_t;
typedef struct mutex_t { bool locked; } mutex_t;
typedef struct thread_t thread_t;
typedef struct device_t device_t;
typedef struct svga_t svga_t;

void fatal(const char *format, ...);
void pclog_ex(const char *format, va_list arguments);
void thread_set_event(event_t *event);
void thread_reset_event(event_t *event);
void thread_wait_event(event_t *event, int timeout);
void thread_wait_mutex(mutex_t *mutex);
void thread_release_mutex(mutex_t *mutex);
uint64_t plat_timer_read(void);
void *plat_mmap(size_t size, uint8_t executable, void *unused);
void plat_munmap(void *pointer, size_t size);
/* Provided by the platform: zeroed memory the CPU may execute, or NULL. */
void *voodoo2_platform_alloc_exec(size_t size);
void voodoo2_platform_free_exec(void *pointer, size_t size);

#endif