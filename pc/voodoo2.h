#ifndef VOODOO2_H
#define VOODOO2_H

/*
 * 3dfx Voodoo2 (CVG) as a 3D-only PCI add-in board, the way it shipped: the
 * primary VGA keeps the desktop and the board takes the monitor only while
 * fbiInit0.VGA_PASS is set (the pass-through relay of the real card).
 *
 * One 16 MB memory BAR: registers 0x000000, LFB 0x400000, textures 0x800000.
 * 4 MB frame buffer, two TMUs with 4 MB each (a 12 MB board).  Rendering is
 * the synchronous 86Box CPU core; there is no I/O BAR, ROM or interrupt.
 */

#include <stdbool.h>
#include <stdint.h>
#include "pci.h"

#define VOODOO2_VENDOR_ID 0x121a
#define VOODOO2_DEVICE_ID 0x0002
#define VOODOO2_BAR_SIZE (16u << 20)
#define VOODOO2_FB_SIZE (4u << 20)
#define VOODOO2_TEX_SIZE (4u << 20)
#define VOODOO2_COMMAND_HISTORY 16

typedef struct Voodoo2State Voodoo2State;

typedef struct {
    uint64_t reads, writes, unsupported_reads, unsupported_writes;
    /* Microseconds spent handling guest accesses (includes synchronous
     * rendering) and converting the front buffer for scanout. */
    uint64_t busy_us, scanout_us, scanouts;
    /* Asynchronous mode: times the guest found the write queue full. */
    uint64_t ring_full_waits;
    /* Guest-core reads that waited for the queue, the time they waited, the
     * register most of them read (0x1000 = LFB, 0x2000 = texture), and live
     * status reads that saw pending swaps. */
    uint64_t sync_reads, sync_wait_us, status_pending_reads;
    uint32_t sync_top_offset, sync_top_count;
    uint64_t split_helped, split_local;   /* triangles whose odd lines the helper / this core drew */
    uint64_t live_counts[5];   /* status (outside/inside retrace), vRetrace, hvRetrace, FIFO */
    uint32_t triangles, frame_count, swaps;
    uint32_t front_offset, draw_offset, aux_offset, row_width;
    uint32_t h_disp, v_disp, fbi_init0, init_enable;
    uint32_t pixels_in, pixels_out, depth_fail, alpha_fail, chroma_fail;
    uint32_t cmdfifo_packets, cmdfifo_faults;
    uint32_t recompiles;
    bool pass_through, cmdfifo_enabled, jit, async, split;
} Voodoo2Stats;

typedef struct {
    uint16_t before, after;
    uint16_t cs;
    uint32_t eip;
} Voodoo2CommandChange;

/* Returns the guest instruction pointer (and CS) behind the current access. */
typedef uint32_t Voodoo2CallerSource(uint16_t *cs);

/* memory_mb: 12 (4 MB frame buffer, 4 MB per texture unit) or 8 (2 MB per
 * texture unit), the two boards that were sold; anything else is 12. */
Voodoo2State *voodoo2_init(PCIBus *bus, unsigned memory_mb);
unsigned voodoo2_memory_mb(Voodoo2State *state);
void voodoo2_free(Voodoo2State *state);
bool voodoo2_reset(Voodoo2State *state);
PCIDevice *voodoo2_pci_device(Voodoo2State *state);

/* MMIO: true when the address belongs to the enabled BAR (value filled). */
bool voodoo2_decode(Voodoo2State *state, uint32_t address, unsigned width, uint32_t *offset);
bool voodoo2_read(Voodoo2State *state, uint32_t address, unsigned width, uint32_t *value);
bool voodoo2_write(Voodoo2State *state, uint32_t address, unsigned width, uint32_t value);
bool voodoo2_write_buffer(Voodoo2State *state, uint32_t address, const uint8_t *buffer, unsigned length);

/* Scanout of the front buffer while VGA pass-through is on.  With pixels NULL
 * only reports whether the board currently owns the monitor and its size. */
bool voodoo2_frame(Voodoo2State *state, uint32_t *pixels, unsigned capacity,
                   unsigned *width, unsigned *height);
/* Like voodoo2_frame, but converts only when the visible image may have changed
 * since the last conversion (swap, LFB write, drawing into the front buffer,
 * CLUT or video timing change, new geometry) or when force is set.  *updated
 * tells whether pixels were rewritten. */
bool voodoo2_frame_update(Voodoo2State *state, uint32_t *pixels, unsigned capacity,
                          unsigned *width, unsigned *height, bool force, bool *updated);

Voodoo2Stats voodoo2_stats(Voodoo2State *state);
/* Whether the board drives the monitor now (fbiInit0 VGA pass-through); a
 * lock-free look for the platform's once-a-tick policies. */
bool voodoo2_owns_display(Voodoo2State *state);
bool voodoo2_uses_gpu(Voodoo2State *state);
/* Switch between the x86-64 span recompiler and the C renderer; returns
 * whether the recompiler is active (false when it is unavailable). */
bool voodoo2_set_jit(Voodoo2State *state, bool enabled);

/* Intel GPU back end (pc/voodoo2_gpu.h): triangles it can draw are queued
 * there, the others drawn here (NULL: CPU only). */
struct V2gState;
bool voodoo2_set_gpu(Voodoo2State *state, struct V2gState *gpu);
/* the same, but triangles stay on the CPU until the board's next swap:
 * Glide's start-up draws test triangles without swapping and reads them back
 * (the DOS Glide game crashed at start with the GPU on from the first one) */
bool voodoo2_set_gpu_after_swap(Voodoo2State *state, struct V2gState *gpu);
struct voodoo_t *voodoo2_device(Voodoo2State *state);

/* Asynchronous mode: guest writes are queued and run by a worker core that
 * calls voodoo2_worker_poll() in a loop (returns whether it did work).  Only
 * enable it once that worker runs.  The worker also converts finished frames
 * into the buffer the guest core passes to voodoo2_frame_update().  Progress
 * registers (status, retrace, command FIFO pointers) are answered without
 * waiting for the queue.  Strict mode (tests) makes every guest-core read and
 * frame query wait for the queue, as in synchronous mode. */
bool voodoo2_set_async(Voodoo2State *state, bool enabled);
bool voodoo2_worker_poll(Voodoo2State *state);
void voodoo2_set_strict_reads(Voodoo2State *state, bool strict);
/* Whether work still queued shows as busy in status (default: no). */
void voodoo2_set_queue_busy(Voodoo2State *state, bool busy);

/* Split rendering: each triangle's odd lines are drawn separately (86Box's
 * second render thread).  With helper, another core calling
 * voodoo2_helper_poll() in its idle loop may draw them; the rendering core
 * draws them itself whenever the helper has not taken them in time. */
bool voodoo2_set_split(Voodoo2State *state, bool split, bool helper);
bool voodoo2_helper_poll(Voodoo2State *state);
void voodoo2_set_caller_source(Voodoo2CallerSource *source);
unsigned voodoo2_command_changes(Voodoo2State *state, Voodoo2CommandChange *out, unsigned capacity);

#endif
