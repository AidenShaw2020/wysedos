/*
 * 3dfx Voodoo2 PCI add-in board around the synchronous 86Box Voodoo core.
 *
 * The register front end (fbiInit/initEnable gating, status, DAC/PLL, CLUT,
 * video timing, command FIFO hole tracking and the V1/V2 buffer layout) is
 * adapted from 86Box src/video/vid_voodoo.c and vid_voodoo_display.c at
 * revision a1e92633baf178ff0791cb5624e406da3e8eb257 (GPL-2.0-or-later, see
 * pc/voodoo/86box/COPYING).  Rendering, textures and LFB access are the
 * imported core under pc/voodoo/86box.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voodoo2.h"
#include "voodoo2_gpu.h"
#include "voodoo/voodoo_port.h"
#include <86box/vid_voodoo_common.h>
#include <86box/vid_voodoo_regs.h>
#include <86box/vid_voodoo_reg.h>
#include <86box/vid_voodoo_render.h>
#include <86box/vid_voodoo_texture.h>
#include <86box/vid_voodoo_fb.h>

int tris;
int changeframecount = 1;
extern int voodoo_recomp;

#define FB_BYTES (VOODOO2_FB_SIZE + 8192)
#define FRAME_US 16667u
#define RETRACE_US 1200u

/* One guest write waiting for the worker core: initEnable is captured when
 * the guest writes, because Glide opens and closes it around fbiInit writes. */
typedef struct {
    uint32_t offset, value, init;
    uint8_t width, swap, pad[2];
} Voodoo2Command;

#define RING_ENTRIES (1u << 17)
#define SCANOUT_US 16000u

struct Voodoo2State {
    voodoo_t device;
    PCIDevice *pci;
    uint32_t base;
    bool enabled;
    uint8_t *fb, *fb_block, *tex[2];   /* fb: page-aligned inside fb_block (GPU render targets) */
    uint8_t changed[FB_BYTES / 4096 + 1];
    mutex_t swap_mutex;
    event_t wake_main;
    uint8_t lock;
    const uint32_t *packet_words;
    unsigned packet_count, packet_index;
    bool packet_fault, fifo_running, fifo_fault;
    uint32_t discarded;
    Voodoo2Stats stats;
    uint16_t last_command;
    uint32_t command_changes;
    Voodoo2CommandChange changes[VOODOO2_COMMAND_HISTORY];
    /* Scanout cache: what the last conversion showed. */
    bool display_dirty;
    uint32_t shown_front;
    unsigned shown_width, shown_height;
    /* Span recompiler code buffer (survives resets) and whether it is used. */
    void *codegen;
    bool jit;
    uint32_t tex_bytes;        /* per texture unit: 2 or 4 MB */
    /* Asynchronous mode: the guest core queues writes, a worker core runs
     * them (voodoo2_worker_poll) and converts finished frames for scanout.
     * ring_head is written only by the guest core, ring_tail only by the
     * worker. */
    Voodoo2Command *ring;
    volatile uint32_t ring_head, ring_tail;
    volatile bool async, want_lock, reconvert;
    volatile uint32_t queued_swaps;
    bool strict_reads, command_init_valid, queue_busy;
    uint32_t command_init;
    uint64_t guest_reads, guest_writes, ring_full_waits;
    uint64_t sync_reads, sync_wait_us, status_pending_reads;
    uint32_t sync_by_register[258];
    /* live reads: status outside/inside retrace, vRetrace, hvRetrace, FIFO */
    uint64_t live_counts[5];
    uint32_t *volatile target;
    volatile unsigned target_capacity;
    volatile bool published_valid;
    volatile unsigned published_width, published_height;
    volatile uint32_t published_seq;
    uint32_t main_seq;
    uint64_t last_convert_us;
    /* Split rendering: the odd lines of each triangle are offered to a helper
     * core (voodoo2_helper_poll).  helper_job: 0 idle, 1 offered, 2 taken,
     * 3 done. */
    volatile bool helper_online;
    bool split;
    volatile int helper_job;
    voodoo_params_t *volatile helper_params;
    uint64_t split_helped, split_local;
    /* Intel GPU back end (voodoo2_set_gpu): triangles it can draw go there */
    V2gState *gpu;
    /* GPU only once the board has swapped since it was attached: Glide's
     * start-up draws test triangles without swapping and reads them back
     * through the LFB, where the CPU renderer's exact pixels and counters are
     * what it checks (the DOS Glide game crashed at start with the GPU on) */
    bool gpu_wait_swap;
    int gpu_from_frame;
};

void *plat_mmap(size_t size, uint8_t executable, void *unused)
{
    (void)unused;
    return executable ? voodoo2_platform_alloc_exec(size) : NULL;
}

void plat_munmap(void *pointer, size_t size)
{
    if (pointer) voodoo2_platform_free_exec(pointer, size);
}

static Voodoo2CallerSource *caller_source;

/* ---- port hooks required by the imported core ---- */

void fatal(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    pclog_ex(format, arguments);
    va_end(arguments);
    __builtin_trap();
}

void pclog_ex(const char *format, va_list arguments)
{
    char message[256];
    vsnprintf(message, sizeof(message), format, arguments);
    printf("Voodoo2 core: %s", message);
}

void thread_set_event(event_t *event) { if (event) event->signaled = true; }
void thread_reset_event(event_t *event) { if (event) event->signaled = false; }

void thread_wait_event(event_t *event, int timeout)
{
    (void)timeout;
    if (!event || !event->signaled)
        fatal("unexpected asynchronous wait in synchronous renderer\n");
}

void thread_wait_mutex(mutex_t *mutex)
{
    if (!mutex || mutex->locked) fatal("invalid synchronous renderer lock\n");
    mutex->locked = true;
}

void thread_release_mutex(mutex_t *mutex)
{
    if (!mutex || !mutex->locked) fatal("invalid synchronous renderer unlock\n");
    mutex->locked = false;
}

uint64_t plat_timer_read(void)
{
    return time_us_64();
}

/* Buffer layout of SST-1/CVG (86Box voodoo_recalc). */
void voodoo_recalc(voodoo_t *voodoo)
{
    uint32_t buffer_offset = ((voodoo->fbiInit2 >> 11) & 511) * 4096;
    voodoo->params.front_offset = voodoo->disp_buffer * buffer_offset;
    voodoo->back_offset = voodoo->draw_buffer * buffer_offset;
    voodoo->buffer_cutoff = TRIPLE_BUFFER ? buffer_offset * 4 : buffer_offset * 3;
    voodoo->params.aux_offset = buffer_offset * (TRIPLE_BUFFER ? 3 : 2);
    switch (voodoo->lfbMode & LFB_WRITE_MASK) {
    case LFB_WRITE_BACK:
        voodoo->fb_write_offset = voodoo->back_offset;
        voodoo->fb_write_buffer = voodoo->draw_buffer;
        break;
    case LFB_WRITE_FRONT:
        voodoo->fb_write_buffer = voodoo->disp_buffer;
        /* fallthrough */
    default:
        voodoo->fb_write_offset = voodoo->params.front_offset;
        break;
    }
    switch (voodoo->lfbMode & LFB_READ_MASK) {
    case LFB_READ_FRONT: voodoo->fb_read_offset = voodoo->params.front_offset; break;
    case LFB_READ_BACK: voodoo->fb_read_offset = voodoo->back_offset; break;
    case LFB_READ_AUX: voodoo->fb_read_offset = voodoo->params.aux_offset; break;
    default: voodoo->fb_read_offset = voodoo->fb_mask + 1; break;
    }
    if ((voodoo->params.fbzMode & FBZ_DRAW_MASK) == FBZ_DRAW_FRONT) {
        voodoo->params.draw_offset = voodoo->params.front_offset;
        voodoo->fb_draw_buffer = voodoo->disp_buffer;
    } else {
        voodoo->params.draw_offset = voodoo->back_offset;
        voodoo->fb_draw_buffer = voodoo->draw_buffer;
    }
    voodoo->block_width = ((voodoo->fbiInit1 >> 4) & 15) * 2;
    if (voodoo->fbiInit6 & (1u << 30)) voodoo->block_width += 1;
    if (voodoo->fbiInit1 & (1u << 24)) voodoo->block_width += 32;
    voodoo->row_width = voodoo->block_width * 32 * 2;
    voodoo->params.row_width = voodoo->row_width;
    voodoo->aux_row_width = voodoo->row_width;
    voodoo->params.aux_row_width = voodoo->aux_row_width;
}

void banshee_set_overlay_addr(void *opaque, uint32_t address)
{
    (void)opaque; (void)address;
    fatal("Banshee overlay requested on a Voodoo2\n");
}

/* Swaps complete at once: there is no asynchronous retrace in this model. */
void voodoo_wait_for_swap_complete(voodoo_t *device)
{
    if (!device->swap_pending) return;
    device->front_offset = device->swap_offset;
    device->swap_pending = 0;
    if (device->swap_count > 0) device->swap_count--;
    device->frame_count++;
}

void voodoo_port_vram_written(voodoo_t *device, uint32_t address, unsigned length)
{
    Voodoo2State *state = device->priv;
    if (state->gpu && length) voodoo2_gpu_cpu_wrote(state->gpu, address, address + length);
    /* Only writes into the visible front buffer change the picture. */
    uint64_t front = device->front_offset;
    uint64_t visible = (uint64_t)device->v_disp * device->row_width;
    if (length && address < front + visible && (uint64_t)address + length > front)
        state->display_dirty = true;
    for (uint32_t page = address >> 12; length && page <= (address + length - 1) >> 12 &&
         page < sizeof(state->changed); page++)
        state->changed[page] = changeframecount;
}

uint8_t *voodoo_port_vram_pointer(voodoo_t *device, uint32_t address, unsigned width)
{
    if (address <= device->fb_mask && width && width - 1 <= device->fb_mask - address)
        return device->fb_mem + address;
    Voodoo2State *state = device->priv;
    state->discarded = UINT32_MAX;
    return (uint8_t *)&state->discarded;
}

void voodoo_triangle(voodoo_t *voodoo, voodoo_params_t *params, int odd_even);

/* The CPU is about to write (or has written) the draw and aux buffers. */
static void cpu_draws(Voodoo2State *state)
{
    if (!state->gpu) return;
    voodoo_t *device = &state->device;
    uint32_t size = (uint32_t)(device->v_disp > 0 ? device->v_disp : 1) * device->row_width;
    voodoo2_gpu_cpu_wrote(state->gpu, device->params.draw_offset, device->params.draw_offset + size);
    voodoo2_gpu_cpu_wrote(state->gpu, device->params.aux_offset, device->params.aux_offset + size);
}

void voodoo_port_render_idle(voodoo_t *device)
{
    Voodoo2State *state = device->priv;
    if (state && state->gpu) voodoo2_gpu_finish(state->gpu, device);
}

void voodoo_port_texture_decoded(voodoo_t *device, int tmu, int entry, uint32_t first, uint32_t end)
{
    Voodoo2State *state = device->priv;
    if (state && state->gpu) voodoo2_gpu_texture_decoded(state->gpu, (unsigned)tmu, (unsigned)entry, first, end);
}

/* One triangle.  Split: the helper core may take the odd lines while this core
 * draws the even ones; if it has not taken them by then, this core draws them
 * too.  Either way both halves are done before the next command. */
void voodoo_port_triangle(voodoo_t *device, voodoo_params_t *params)
{
    Voodoo2State *state = device->priv;
    if (state->gpu && state->gpu_wait_swap && device->frame_count != state->gpu_from_frame)
        state->gpu_wait_swap = false;
    if (state->gpu && !state->gpu_wait_swap &&
        voodoo2_gpu_triangle(state->gpu, device, params)) {
        voodoo_port_render_written(device, true);
        return;
    }
    /* drawn here (after the GPU has finished): the GPU must see these pixels */
    if (state->gpu) {
        voodoo_port_render_idle(device);
        cpu_draws(state);
    }
    if (!device->odd_even_mask) {
        voodoo_triangle(device, params, 0);
    } else {
        /* voodoo_triangle counts every half, from both cores */
        int counted = device->tri_count;
        bool offered = state->helper_online;
        if (offered) {
            state->helper_params = params;
            __atomic_store_n(&state->helper_job, 1, __ATOMIC_RELEASE);
        }
        voodoo_triangle(device, params, 0);
        int expected = 1;
        if (!offered || __atomic_compare_exchange_n(&state->helper_job, &expected, 0, false,
                                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            voodoo_triangle(device, params, 1);
            state->split_local++;
        } else {
            while (__atomic_load_n(&state->helper_job, __ATOMIC_ACQUIRE) != 3)
                __asm__ volatile("pause");
            __atomic_store_n(&state->helper_job, 0, __ATOMIC_RELEASE);
            state->split_helped++;
        }
        device->tri_count = counted + 1;
    }
    voodoo_port_render_written(device, true);
}

bool voodoo2_helper_poll(Voodoo2State *state)
{
    if (!state || __atomic_load_n(&state->helper_job, __ATOMIC_ACQUIRE) != 1) return false;
    int expected = 1;
    if (!__atomic_compare_exchange_n(&state->helper_job, &expected, 2, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return false;
    voodoo_triangle(&state->device, state->helper_params, 1);
    __atomic_store_n(&state->helper_job, 3, __ATOMIC_RELEASE);
    return true;
}

void voodoo_port_render_written(voodoo_t *device, bool triangle)
{
    (void)triangle;
    if (device->params.draw_offset == device->front_offset)
        ((Voodoo2State *)device->priv)->display_dirty = true;
}

/* ---- locking ---- */

static void lock(Voodoo2State *state)
{
    while (__atomic_exchange_n(&state->lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}

static void unlock(Voodoo2State *state)
{
    __atomic_store_n(&state->lock, 0, __ATOMIC_RELEASE);
}

/* Guest core: wait until the worker has run every queued write, then hold the
 * device.  The guest core is the only producer, so the queue stays empty. */
static void quiesce(Voodoo2State *state)
{
    if (state->async)
        while (__atomic_load_n(&state->ring_tail, __ATOMIC_ACQUIRE) != state->ring_head)
            __asm__ volatile("pause");
    state->want_lock = true;
    lock(state);
    state->want_lock = false;
}

/* ---- command FIFO ---- */

static bool register_write(Voodoo2State *state, uint32_t address, uint32_t value, bool from_fifo);

static unsigned packet_length(uint32_t header)
{
    switch (header & 7) {
    case 0: {
        unsigned operation = (header >> 3) & 7;
        return operation <= 3 ? 1 : 0;
    }
    case 1: return 1 + (header >> 16);
    case 2: return 1 + __builtin_popcount(header >> 3);
    case 3: {
        unsigned mask = (header >> 10) & 255;
        unsigned width = 2;
        if (header & (1u << 28))
            width += !!(mask & (SETUPMODE_RGB | SETUPMODE_ALPHA));
        else
            width += (mask & SETUPMODE_RGB ? 3 : 0) + !!(mask & SETUPMODE_ALPHA);
        width += __builtin_popcount(mask & (SETUPMODE_Z | SETUPMODE_Wb | SETUPMODE_W0 | SETUPMODE_W1));
        width += (mask & SETUPMODE_S0_T0 ? 2 : 0) + (mask & SETUPMODE_S1_T1 ? 2 : 0);
        return 1 + width * ((header >> 6) & 15) + (header >> 29);
    }
    case 4: return 1 + __builtin_popcount((header >> 15) & 0x3fff) + (header >> 29);
    case 5: {
        unsigned space = header >> 30;
        if (space < 2) return 0;
        return 2 + (((header >> 3) & 0x7ffff) ? ((header >> 3) & 0x7ffff) : 1);
    }
    default: return 0;
    }
}

uint32_t voodoo_port_fifo_word(voodoo_t *device)
{
    Voodoo2State *state = device->priv;
    if (state->packet_index >= state->packet_count)
        fatal("command FIFO payload underflow\n");
    device->cmdfifo_rp += 4;
    if (!device->cmdfifo_in_sub) device->cmdfifo_depth_rd++;
    return state->packet_words[state->packet_index++];
}

float voodoo_port_fifo_float(voodoo_t *device)
{
    uint32_t bits = voodoo_port_fifo_word(device);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

void voodoo_port_fifo_reg(voodoo_t *device, uint32_t address, uint32_t value)
{
    register_write(device->priv, address, value, true);
}

void voodoo_port_fifo_2d(voodoo_t *device, uint32_t address, uint32_t value)
{
    (void)address; (void)value;
    ((Voodoo2State *)device->priv)->packet_fault = true;
}

void voodoo_port_fifo_raw(voodoo_t *device, uint32_t address, uint32_t value)
{
    (void)address; (void)value;
    ((Voodoo2State *)device->priv)->packet_fault = true;
}

void banshee_cmd_write(void *opaque, uint32_t address, uint32_t value)
{
    (void)address; (void)value;
    ((Voodoo2State *)opaque)->packet_fault = true;
}

/* Runs one complete packet that sits in frame buffer memory at words. */
static bool submit_packet(Voodoo2State *state, const uint32_t *words, unsigned count)
{
    voodoo_t *device = &state->device;
    if ((words[0] & 7) == 0) {
        unsigned operation = (words[0] >> 3) & 7;
        if ((operation == 1 && device->cmdfifo_in_sub) ||
            (operation == 2 && !device->cmdfifo_in_sub)) return false;
    }
    state->packet_fault = false;
    state->packet_words = words;
    state->packet_count = count;
    state->packet_index = 1;
    device->cmdfifo_rp += 4;
    if (!device->cmdfifo_in_sub) device->cmdfifo_depth_rd++;
    voodoo_port_packet(device, words[0]);
    bool complete = state->packet_index == count && !state->packet_fault;
    state->packet_words = NULL;
    state->packet_count = state->packet_index = 0;
    if (complete) state->stats.cmdfifo_packets++;
    return complete;
}

static void drain_cmdfifo(Voodoo2State *state)
{
    voodoo_t *device = &state->device;
    if (state->fifo_running || state->fifo_fault || !device->cmdfifo_enabled) return;
    state->fifo_running = true;
    for (unsigned budget = 0; budget < 65536 &&
         (device->cmdfifo_depth_rd != device->cmdfifo_depth_wr || device->cmdfifo_in_sub); budget++) {
        uint32_t address = (uint32_t)device->cmdfifo_rp;
        unsigned available = device->cmdfifo_depth_wr - device->cmdfifo_depth_rd;
        if ((address & 3) || address > VOODOO2_FB_SIZE - 4) {
            state->fifo_fault = true;
            break;
        }
        uint32_t header;
        memcpy(&header, state->fb + address, 4);
        unsigned length = packet_length(header);
        if (!length || length > (VOODOO2_FB_SIZE - address) / 4) {
            state->fifo_fault = true;
            break;
        }
        if (!device->cmdfifo_in_sub && length > available) break;
        if (!submit_packet(state, (const uint32_t *)(state->fb + address), length)) {
            state->fifo_fault = true;
            break;
        }
    }
    if (state->fifo_fault) {
        state->stats.cmdfifo_faults++;
        printf("Voodoo2 command FIFO stopped at %08x: invalid or unsupported packet\n",
               (unsigned)device->cmdfifo_rp);
    }
    state->fifo_running = false;
}

/* 86Box voodoo_cmdfifo_write: sequential writes publish words, gaps count
 * as holes until filled. */
static void cmdfifo_write(Voodoo2State *state, uint32_t offset, uint32_t value)
{
    voodoo_t *device = &state->device;
    uint32_t address = (device->cmdfifo_base + (offset & 0x3fffc)) & device->fb_mask;
    if (address > VOODOO2_FB_SIZE - 4) return;
    memcpy(state->fb + address, &value, 4);
    if (address == device->cmdfifo_base && !device->cmdfifo_holecount) {
        device->cmdfifo_amin = address;
        device->cmdfifo_amax = address;
        device->cmdfifo_depth_wr++;
    } else if (device->cmdfifo_holecount) {
        device->cmdfifo_holecount--;
        if (!device->cmdfifo_holecount) {
            device->cmdfifo_depth_wr += (device->cmdfifo_amax - device->cmdfifo_amin) >> 2;
            device->cmdfifo_amin = device->cmdfifo_amax;
        }
    } else if (address == device->cmdfifo_amax + 4) {
        device->cmdfifo_amin = address;
        device->cmdfifo_amax = address;
        device->cmdfifo_depth_wr++;
    } else {
        if (address < device->cmdfifo_amin)
            device->cmdfifo_amin = device->cmdfifo_base - 4;
        device->cmdfifo_amax = address;
        device->cmdfifo_holecount = ((device->cmdfifo_amax - device->cmdfifo_amin) >> 2) - 1;
    }
    drain_cmdfifo(state);
}

/* ---- video ---- */

static void calc_clut(voodoo_t *device)
{
    for (unsigned c = 0; c < 256; c++) {
        device->clutData256[c].r = (device->clutData[c >> 3].r * (8 - (c & 7)) + device->clutData[(c >> 3) + 1].r * (c & 7)) >> 3;
        device->clutData256[c].g = (device->clutData[c >> 3].g * (8 - (c & 7)) + device->clutData[(c >> 3) + 1].g * (c & 7)) >> 3;
        device->clutData256[c].b = (device->clutData[c >> 3].b * (8 - (c & 7)) + device->clutData[(c >> 3) + 1].b * (c & 7)) >> 3;
    }
    for (unsigned c = 0; c < 65536; c++) {
        unsigned r = (c >> 8) & 0xf8, g = (c >> 3) & 0xfc, b = (c << 3) & 0xf8;
        device->video_16to32[c] = (device->clutData256[r].r << 16) |
            (device->clutData256[g].g << 8) | device->clutData256[b].b;
    }
    device->clutData_dirty = 0;
}

static bool in_retrace(void)
{
    return time_us_64() % FRAME_US >= FRAME_US - RETRACE_US;
}

static unsigned current_line(const voodoo_t *device)
{
    unsigned total = device->v_total > 0 ? (unsigned)device->v_total : 525;
    return (unsigned)(time_us_64() % FRAME_US * total / FRAME_US);
}

/* ---- registers ---- */

static uint32_t init_enable(Voodoo2State *state)
{
    uint32_t value = 0;
    for (unsigned byte = 0; byte < 4; byte++)
        value |= (uint32_t)pci_device_get_config8(state->pci, 0x40 + byte) << (byte * 8);
    return value;
}

static uint32_t status(Voodoo2State *state)
{
    voodoo_t *device = &state->device;
    voodoo_wait_for_swap_complete(device);
    drain_cmdfifo(state);
    bool busy = device->cmdfifo_enabled &&
        (device->cmdfifo_depth_rd != device->cmdfifo_depth_wr || device->cmdfifo_in_sub);
    uint32_t value = (0xffffu << 12) | 0x3f;
    value |= (uint32_t)MIN(device->swap_count, 7) << 28;
    if (!in_retrace()) value |= 0x40;
    if (busy) value |= 0x380;
    return value;
}

static uint32_t register_read(Voodoo2State *state, uint32_t address)
{
    voodoo_t *device = &state->device;
    device->initEnable = init_enable(state);
    switch (address & 0x3fc) {
    case SST_status: return status(state);
    case SST_intrCtrl: return device->intrCtrl & VOODOO_INTRCTRL_READ_MASK;
    case SST_fbzColorPath: return device->params.fbzColorPath;
    case SST_fogMode: return device->params.fogMode;
    case SST_alphaMode: return device->params.alphaMode;
    case SST_fbzMode: return device->params.fbzMode;
    case SST_lfbMode: return device->lfbMode;
    case SST_clipLeftRight: return device->params.clipRight | (device->params.clipLeft << 16);
    case SST_clipLowYHighY: return device->params.clipHighY | (device->params.clipLowY << 16);
    case SST_stipple: return device->params.stipple;
    case SST_color0: return device->params.color0;
    case SST_color1: return device->params.color1;
    case SST_fbiPixelsIn: return device->fbiPixelsIn & 0xffffff;
    case SST_fbiChromaFail: return device->fbiChromaFail & 0xffffff;
    case SST_fbiZFuncFail: return device->fbiZFuncFail & 0xffffff;
    case SST_fbiAFuncFail: return device->fbiAFuncFail & 0xffffff;
    case SST_fbiPixelsOut: return device->fbiPixelsOut & 0xffffff;
    case SST_fbiInit0: return device->fbiInit0;
    case SST_fbiInit1: return device->fbiInit1;
    case SST_fbiInit2: return (device->initEnable & 4) ? device->dac_readdata : device->fbiInit2;
    case SST_fbiInit3: return device->fbiInit3;
    case SST_fbiInit4: return device->fbiInit4;
    case SST_fbiInit5: return (device->fbiInit5 & ~0x1ffu) | ((device->board_id & 0xf) << 5);
    case SST_fbiInit6: return device->fbiInit6;
    case SST_fbiInit7: return (device->fbiInit7 & ~0xffu) | ((device->board_id >> 4) & 1);
    case SST_vRetrace: return current_line(device) & 0x1fff;
    case SST_hvRetrace: {
        unsigned total = device->h_total > 0 ? (unsigned)device->h_total : 800;
        uint64_t within = time_us_64() % FRAME_US;
        unsigned lines = device->v_total > 0 ? (unsigned)device->v_total : 525;
        unsigned position = (unsigned)((within * lines % FRAME_US) * total / FRAME_US);
        return (current_line(device) & 0x1fff) | (position << 16);
    }
    case SST_cmdFifoBaseAddr: return (device->cmdfifo_base >> 12) | ((device->cmdfifo_end >> 12) << 16);
    case SST_cmdFifoRdPtr: return device->cmdfifo_rp;
    case SST_cmdFifoAMin: return device->cmdfifo_amin;
    case SST_cmdFifoAMax: return device->cmdfifo_amax;
    case SST_cmdFifoDepth: return device->cmdfifo_depth_wr - device->cmdfifo_depth_rd;
    case SST_cmdFifoHoles: return (uint32_t)device->cmdfifo_holecount;
    default:
        state->stats.unsupported_reads++;
        return UINT32_MAX;
    }
}

/* Registers that report the board's progress.  In asynchronous mode they are
 * read while the worker runs, as a real Voodoo2 answers them while it draws.
 * Queued swaps show as pending swaps, which bounds how far the guest runs
 * ahead.  Work still in the host queue does not show as busy (unless
 * queue_busy): every guest access is ordered behind it and every read that
 * could see its result waits for it, so a guest idling the board before LFB
 * access - Glide's grLfbLock - only loses time by waiting. */
static bool live_register(uint32_t offset)
{
    if (offset & 0xc00000) return false;
    switch (offset & 0x3fc) {
    case SST_status: case SST_vRetrace: case SST_hvRetrace:
    case SST_cmdFifoBaseAddr: case SST_cmdFifoRdPtr: case SST_cmdFifoAMin:
    case SST_cmdFifoAMax: case SST_cmdFifoDepth: case SST_cmdFifoHoles:
        return true;
    default:
        return false;
    }
}

static uint32_t live_read(Voodoo2State *state, uint32_t offset)
{
    const voodoo_t *device = &state->device;
    switch (offset & 0x3fc) {
    case SST_status: {
        state->live_counts[in_retrace() ? 1 : 0]++;
        bool queued = __atomic_load_n(&state->ring_tail, __ATOMIC_ACQUIRE) != state->ring_head;
        bool fifo = device->cmdfifo_enabled &&
            (device->cmdfifo_depth_rd != device->cmdfifo_depth_wr || device->cmdfifo_in_sub);
        unsigned pending = (unsigned)device->swap_count + __atomic_load_n(&state->queued_swaps, __ATOMIC_ACQUIRE);
        if (pending) state->status_pending_reads++;
        uint32_t value = (0xffffu << 12) | 0x3f;
        value |= (uint32_t)MIN(pending, 7u) << 28;
        if (!in_retrace()) value |= 0x40;
        if (fifo || (queued && state->queue_busy)) value |= 0x380;
        return value;
    }
    case SST_vRetrace: state->live_counts[2]++; return current_line(device) & 0x1fff;
    case SST_hvRetrace: {
        state->live_counts[3]++;
        unsigned total = device->h_total > 0 ? (unsigned)device->h_total : 800;
        uint64_t within = time_us_64() % FRAME_US;
        unsigned lines = device->v_total > 0 ? (unsigned)device->v_total : 525;
        unsigned position = (unsigned)((within * lines % FRAME_US) * total / FRAME_US);
        return (current_line(device) & 0x1fff) | (position << 16);
    }
    default: break;
    }
    state->live_counts[4]++;
    switch (offset & 0x3fc) {
    case SST_cmdFifoBaseAddr: return (device->cmdfifo_base >> 12) | ((device->cmdfifo_end >> 12) << 16);
    case SST_cmdFifoRdPtr: return device->cmdfifo_rp;
    case SST_cmdFifoAMin: return device->cmdfifo_amin;
    case SST_cmdFifoAMax: return device->cmdfifo_amax;
    case SST_cmdFifoDepth: return device->cmdfifo_depth_wr - device->cmdfifo_depth_rd;
    default: return (uint32_t)device->cmdfifo_holecount;
    }
}

static void dac_write(voodoo_t *device, uint32_t value)
{
    device->dac_reg = (value >> 8) & 7;
    device->dac_readdata = 0xff;
    if (value & 0x800) {
        if (device->dac_reg == 5) {
            switch (device->dac_data[7]) {
            case 0x01: device->dac_readdata = 0x55; break;
            case 0x07: device->dac_readdata = 0x71; break;
            case 0x0b: device->dac_readdata = 0x79; break;
            default: break;
            }
        } else {
            device->dac_readdata = device->dac_data[device->dac_readdata & 7];
        }
    } else if (device->dac_reg == 5) {
        uint16_t *pll = &device->dac_pll_regs[device->dac_data[4] & 0xf];
        if (!device->dac_reg_ff) *pll = (*pll & 0xff00) | (value & 0xff);
        else *pll = (*pll & 0xff) | ((value & 0xff) << 8);
        device->dac_reg_ff = !device->dac_reg_ff;
        if (!device->dac_reg_ff) device->dac_data[4]++;
    } else {
        device->dac_data[device->dac_reg] = value & 0xff;
        device->dac_reg_ff = 0;
    }
}

/* from_fifo: register writes carried by command FIFO packets (always applied
 * to the 3D register file, as 86Box voodoo_cmdfifo_reg_writel does). */
static bool register_write(Voodoo2State *state, uint32_t address, uint32_t value, bool from_fifo)
{
    voodoo_t *device = &state->device;
    device->initEnable = state->command_init_valid ? state->command_init : init_enable(state);
    bool init = (device->initEnable & 1) != 0;
    bool fifo_mode = (device->fbiInit7 & FBIINIT7_CMDFIFO_ENABLE) != 0;
    switch (address & 0x3fc) {
    case SST_fastfillCMD:
        if (device->params.draw_offset == device->front_offset) state->display_dirty = true;
        cpu_draws(state);
        break;
    case SST_bltCommand: case SST_bltData:
        if (state->gpu) voodoo2_gpu_cpu_wrote(state->gpu, 0, device->fb_mask + 1);
        break;
    case SST_swapbufferCMD:
    case SST_videoDimensions: case SST_hSync: case SST_vSync: case SST_backPorch:
    case SST_clutData: case SST_fbiInit0: case SST_fbiInit1: case SST_fbiInit2:
    case SST_fbiInit3: case SST_fbiInit4: case SST_fbiInit5: case SST_fbiInit6:
    case SST_fbiInit7:
        state->display_dirty = true;
        break;
    default:
        break;
    }
    switch (address & 0x3fc) {
    case SST_intrCtrl:
        if (value & VOODOO_INTRCTRL_INTERRUPT_ACTIVE)
            device->intrCtrl &= ~VOODOO_INTRCTRL_PENDING_MASK;
        device->intrCtrl = (device->intrCtrl & ~VOODOO_INTRCTRL_ENABLE_MASK) |
            (value & VOODOO_INTRCTRL_ENABLE_MASK);
        return true;
    case SST_userIntrCMD:
        return true;
    case SST_swapbufferCMD:
        if (fifo_mode && !from_fifo) return true;
        device->swap_count++;
        voodoo_reg_writel(address, value, device);
        state->stats.swaps++;
        return true;
    case SST_fbiInit4: if (init) device->fbiInit4 = value; return true;
    case SST_backPorch: device->backPorch = value; return true;
    case SST_videoDimensions:
        device->videoDimensions = value;
        device->h_disp = (value & 0xfff) + 1;
        device->v_disp = (value >> 16) & 0xfff;
        if (device->v_disp == 386 || device->v_disp == 402 ||
            device->v_disp == 482 || device->v_disp == 602)
            device->v_disp -= 2;
        if (device->h_disp > 1984) device->h_disp = 1984;
        if (device->v_disp > 2016) device->v_disp = 2016;
        return true;
    case SST_fbiInit0:
        if (!init) return true;
        device->fbiInit0 = value;
        if (value & FBIINIT0_GRAPHICS_RESET) {
            device->disp_buffer = 0;
            device->draw_buffer = 1;
            voodoo_recalc(device);
            device->front_offset = device->params.front_offset;
        }
        return true;
    case SST_fbiInit1:
        if (!init) return true;
        if ((device->fbiInit1 & FBIINIT1_VIDEO_RESET) && !(value & FBIINIT1_VIDEO_RESET)) {
            device->line = 0;
            device->swap_count = 0;
            device->retrace_count = 0;
        }
        device->fbiInit1 = (value & ~5u) | (device->fbiInit1 & 5u);
        voodoo_recalc(device);
        return true;
    case SST_fbiInit2:
        if (!init) return true;
        device->fbiInit2 = value;
        voodoo_recalc(device);
        return true;
    case SST_fbiInit3: if (init) device->fbiInit3 = value; return true;
    case SST_hSync:
        device->hSync = value;
        device->h_total = (value & 0xffff) + (value >> 16);
        return true;
    case SST_vSync:
        device->vSync = value;
        device->v_total = (value & 0xffff) + (value >> 16);
        return true;
    case SST_clutData: {
        unsigned index = (value >> 24) & 0x3f;
        if (index > 32) return true;
        rgbvoodoo_t *entry = &device->clutData[index];
        entry->b = value & 0xff;
        entry->g = (value >> 8) & 0xff;
        entry->r = (value >> 16) & 0xff;
        if (value & 0x20000000) entry->r = entry->g = entry->b = 255;
        device->clutData_dirty = 1;
        return true;
    }
    case SST_dacData: dac_write(device, value); return true;
    case SST_scrFilter: return true;
    case SST_fbiInit5:
        if (init) device->fbiInit5 = (value & ~0x41e6u) | (device->fbiInit5 & 0x41e6u);
        return true;
    case SST_fbiInit6:
        if (init) { device->fbiInit6 = value; voodoo_recalc(device); }
        return true;
    case SST_fbiInit7:
        if (init) {
            device->fbiInit7 = value;
            device->cmdfifo_enabled = value & 0x100;
            if (!device->cmdfifo_enabled) state->fifo_fault = false;
        }
        return true;
    case SST_cmdFifoBaseAddr:
        device->cmdfifo_base = (value & 0x3ff) << 12;
        device->cmdfifo_end = ((value >> 16) & 0x3ff) << 12;
        return true;
    case SST_cmdFifoRdPtr: device->cmdfifo_rp = value & ~3u; return true;
    case SST_cmdFifoAMin: device->cmdfifo_amin = value; return true;
    case SST_cmdFifoAMax: device->cmdfifo_amax = value; return true;
    case SST_cmdFifoDepth:
        device->cmdfifo_depth_rd = 0;
        device->cmdfifo_depth_wr = value & 0xffff;
        drain_cmdfifo(state);
        return true;
    case SST_cmdFifoHoles: device->cmdfifo_holecount = value & 0xffff; return true;
    default:
        if (fifo_mode && !from_fifo) {
            state->stats.unsupported_writes++;
            return true;
        }
        voodoo_reg_writel(address, value, device);
        return true;
    }
}

/* ---- device ---- */

static void initialize_device(Voodoo2State *state)
{
    voodoo_t *device = &state->device;
    device->type = VOODOO_2;
    device->priv = state;
    device->fb_mem = state->fb;
    device->fb_mask = VOODOO2_FB_SIZE - 1;
    device->fb_size = VOODOO2_FB_SIZE >> 20;
    device->changedvram = state->changed;
    device->texture_size = state->tex_bytes >> 20;
    device->texture_mask = state->tex_bytes - 1;
    device->dual_tmus = 1;
    device->render_threads = state->split ? 2 : 1;
    device->odd_even_mask = state->split ? 1 : 0;
    device->bilinear_enabled = 1;
    device->codegen_data = state->codegen;
    device->use_recompiler = state->jit;
    device->swap_mutex = &state->swap_mutex;
    device->wake_main_thread = &state->wake_main;
    state->wake_main.signaled = true;
    device->fbiInit0 = 0x00001c10;
    device->fbiInit1 = 0x00201102;
    device->fbiInit2 = 0x80000040;
    device->fbiInit3 = 0x001e4040;
    device->fbiInit4 = 0x00000001;
    device->fbiInit5 = 0x08408000;
    device->disp_buffer = 0;
    device->draw_buffer = 1;
    for (unsigned tmu = 0; tmu < 2; tmu++) {
        device->tex_mem[tmu] = state->tex[tmu];
        device->tex_mem_w[tmu] = (uint16_t *)state->tex[tmu];
        device->trexInit0[tmu] = TREXINIT0_TEXTURE_MEMORY_SECOND_RAS_ADDRESS_18 |
                                 TREXINIT0_TEXTURE_MEMORY_SECOND_RAS_ENABLE;
        for (unsigned entry = 0; entry < TEX_CACHE_MAX; entry++)
            device->texture_cache[tmu][entry].base = UINT32_MAX;
        voodoo_recalc_tex12(device, tmu);
    }
    /* Linear gamma until Glide loads its own table. */
    for (unsigned entry = 0; entry < 33; entry++) {
        unsigned level = entry * 8 > 255 ? 255 : entry * 8;
        device->clutData[entry].r = device->clutData[entry].g = device->clutData[entry].b = level;
    }
    calc_clut(device);
    voodoo_recalc(device);
    device->front_offset = device->params.front_offset;
    static bool tables;
    if (!tables) {
        voodoo_port_tables_init();
        voodoo_lod_table_init();
        tables = true;
    }
}

static void note_command(Voodoo2State *state)
{
    uint16_t command = pci_device_get_config8(state->pci, PCI_COMMAND) |
        (uint16_t)pci_device_get_config8(state->pci, PCI_COMMAND + 1) << 8;
    if (command == state->last_command) return;
    Voodoo2CommandChange *change =
        &state->changes[state->command_changes++ % VOODOO2_COMMAND_HISTORY];
    *change = (Voodoo2CommandChange){state->last_command, command, 0, 0};
    if (caller_source) change->eip = caller_source(&change->cs);
    state->last_command = command;
}

static void set_bar(void *opaque, int bar, uint32_t address, bool enabled)
{
    Voodoo2State *state = opaque;
    if (bar != 0) return;
    state->base = address;
    state->enabled = enabled;
    note_command(state);
}

/* initEnable bits 15:12 read as 5 on a CVG (86Box voodoo_pci_read).
 * Like 86Box, the registers after initEnable (busSnoop, cfgStatus, cfgScratch,
 * siProcess) are not modelled and read as zero. Glide's silicon-process probe
 * loads a PCI-clock count into siProcess (0x54) bits 27:16 and polls until the
 * board counts it down; a stored write would keep it spinning forever. */
static void config_written(void *opaque, uint8_t address)
{
    Voodoo2State *state = opaque;
    if (address == 0x41)
        pci_device_set_config8(state->pci, 0x41, 0x50 | (pci_device_get_config8(state->pci, 0x41) & 0x0f));
    else if (address >= 0x44)
        pci_device_set_config8(state->pci, address, 0);
}

static bool allocate_textures(Voodoo2State *state)
{
    for (unsigned tmu = 0; tmu < 2; tmu++)
        for (unsigned entry = 0; entry < TEX_CACHE_MAX; entry++) {
            state->device.texture_cache[tmu][entry].data = calloc(
                256 * 256 + 256 * 256 + 128 * 128 + 64 * 64 + 32 * 32 +
                16 * 16 + 8 * 8 + 4 * 4 + 2 * 2, sizeof(uint32_t));
            if (!state->device.texture_cache[tmu][entry].data) return false;
        }
    return true;
}

void voodoo2_free(Voodoo2State *state)
{
    if (!state) return;
    for (unsigned tmu = 0; tmu < 2; tmu++) {
        for (unsigned entry = 0; entry < TEX_CACHE_MAX; entry++)
            free(state->device.texture_cache[tmu][entry].data);
        free(state->tex[tmu]);
    }
#ifndef NO_CODEGEN
    if (state->codegen) {
        state->device.codegen_data = state->codegen;
        voodoo_codegen_close(&state->device);
    }
#endif
    free(state->ring);
    free(state->fb_block);
    free(state);
}

bool voodoo2_set_jit(Voodoo2State *state, bool enabled)
{
    if (!state) return false;
    quiesce(state);
    state->jit = enabled && state->codegen;
    state->device.use_recompiler = state->jit;
    bool active = state->jit;
    unlock(state);
    return active;
}

unsigned voodoo2_memory_mb(Voodoo2State *state)
{
    return state ? 4u + 2u * (state->tex_bytes >> 20) : 0;
}

Voodoo2State *voodoo2_init(PCIBus *bus, unsigned memory_mb)
{
    if (!bus) return NULL;
    Voodoo2State *state = calloc(1, sizeof(*state));
    if (!state) return NULL;
    state->tex_bytes = memory_mb == 8 ? 2u << 20 : VOODOO2_TEX_SIZE;
    state->fb_block = calloc(1, FB_BYTES + 4096);
    state->fb = state->fb_block ? (uint8_t *)(((uintptr_t)state->fb_block + 4095) & ~(uintptr_t)4095) : NULL;
    state->tex[0] = calloc(1, VOODOO2_TEX_SIZE);
    state->tex[1] = calloc(1, VOODOO2_TEX_SIZE);
    state->ring = calloc(RING_ENTRIES, sizeof(Voodoo2Command));
    if (!state->fb || !state->tex[0] || !state->tex[1] || !state->ring || !allocate_textures(state)) {
        voodoo2_free(state);
        return NULL;
    }
    state->pci = pci_register_device(bus, "Voodoo2", -1, VOODOO2_VENDOR_ID,
                                     VOODOO2_DEVICE_ID, 2, 0x0400);
    if (!state->pci) {
        voodoo2_free(state);
        return NULL;
    }
    pci_device_set_config8(state->pci, 0x41, 0x50);
    pci_register_bar(state->pci, 0, VOODOO2_BAR_SIZE, PCI_ADDRESS_SPACE_MEM, state, set_bar);
    pci_device_set_config_hook(state->pci, config_written, state);
#ifndef NO_CODEGEN
    voodoo_codegen_init(&state->device);
    state->codegen = state->device.codegen_data;
    state->jit = state->codegen != NULL;
#endif
    initialize_device(state);
    return state;
}

bool voodoo2_reset(Voodoo2State *state)
{
    if (!state) return false;
    quiesce(state);
    void *textures[2][TEX_CACHE_MAX];
    for (unsigned tmu = 0; tmu < 2; tmu++)
        for (unsigned entry = 0; entry < TEX_CACHE_MAX; entry++)
            textures[tmu][entry] = state->device.texture_cache[tmu][entry].data;
    memset(&state->device, 0, sizeof(state->device));
    for (unsigned tmu = 0; tmu < 2; tmu++) {
        for (unsigned entry = 0; entry < TEX_CACHE_MAX; entry++)
            state->device.texture_cache[tmu][entry].data = textures[tmu][entry];
        memset(state->tex[tmu], 0, VOODOO2_TEX_SIZE);
    }
    memset(state->fb, 0, FB_BYTES);
    memset(&state->stats, 0, sizeof(state->stats));
    state->guest_reads = state->guest_writes = state->ring_full_waits = 0;
    state->queued_swaps = 0;
    state->display_dirty = false;
    state->shown_front = state->shown_width = state->shown_height = 0;
    if (state->published_valid) {
        state->published_valid = false;
        __atomic_add_fetch(&state->published_seq, 1, __ATOMIC_RELEASE);
    }
    state->fifo_fault = state->packet_fault = false;
    initialize_device(state);
    pci_device_set_config16(state->pci, PCI_COMMAND, 0);
    for (unsigned byte = 0; byte < 4; byte++) {
        pci_device_set_config8(state->pci, 0x10 + byte, 0);
        pci_device_set_config8(state->pci, 0x40 + byte, byte == 1 ? 0x50 : 0);
    }
    set_bar(state, 0, 0, false);
    unlock(state);
    return true;
}

PCIDevice *voodoo2_pci_device(Voodoo2State *state)
{
    return state ? state->pci : NULL;
}

bool voodoo2_decode(Voodoo2State *state, uint32_t address, unsigned width, uint32_t *offset)
{
    if (!state || !offset || !state->enabled || !state->base ||
        (width != 1 && width != 2 && width != 4)) return false;
    uint32_t relative = address - state->base;
    if (relative >= VOODOO2_BAR_SIZE || width > VOODOO2_BAR_SIZE - relative) return false;
    *offset = relative;
    return true;
}

static uint32_t read_locked(Voodoo2State *state, uint32_t offset, unsigned width)
{
    voodoo_t *device = &state->device;
    uint32_t aligned = offset & ~3u;
    if (offset & 0x800000) {
        state->stats.unsupported_reads++;
        return UINT32_MAX;
    }
    if (offset & 0x400000) {
        drain_cmdfifo(state);
        voodoo_port_render_idle(device);
        if (width == 2 && !(offset & 1))
            return voodoo_fb_readw(offset, device);
        uint32_t value = voodoo_fb_readl(aligned, device);
        return value >> ((offset & 3) * 8);
    }
    return register_read(state, aligned) >> ((offset & 3) * 8);
}

bool voodoo2_read(Voodoo2State *state, uint32_t address, unsigned width, uint32_t *value)
{
    uint32_t offset;
    if (!value || !voodoo2_decode(state, address, width, &offset)) return false;
    state->guest_reads++;
    uint32_t mask = UINT32_MAX >> ((4 - width) * 8);
    if (state->async && !state->strict_reads && live_register(offset)) {
        *value = (live_read(state, offset & ~3u) >> ((offset & 3) * 8)) & mask;
        return true;
    }
    if (state->async) {
        uint64_t waited = time_us_64();
        quiesce(state);
        state->sync_wait_us += time_us_64() - waited;
        state->sync_reads++;
        state->sync_by_register[(offset & 0x800000) ? 257 : (offset & 0x400000) ? 256 : (offset & 0x3fc) >> 2]++;
    } else {
        quiesce(state);
    }
    uint64_t start = time_us_64();
    *value = read_locked(state, offset, width) & mask;
    state->stats.busy_us += time_us_64() - start;
    unlock(state);
    return true;
}

static void write_locked(Voodoo2State *state, uint32_t offset, unsigned width, uint32_t value)
{
    voodoo_t *device = &state->device;
    if (width != 4 && !((offset & 0x400000) && !(offset & 0x800000) && width == 2 && !(offset & 1))) {
        state->stats.unsupported_writes++;
        return;
    }
    if (offset & 0x800000) {
        voodoo_tex_writel(offset, value, device);
    } else if (offset & 0x400000) {
        voodoo_port_render_idle(device);
        if (width == 2) voodoo_fb_writew(offset, value, device);
        else voodoo_fb_writel(offset, value, device);
    } else if ((offset & 0x200000) && (device->fbiInit7 & FBIINIT7_CMDFIFO_ENABLE)) {
        cmdfifo_write(state, offset, value);
    } else {
        register_write(state, offset, value, false);
    }
}

bool voodoo2_write(Voodoo2State *state, uint32_t address, unsigned width, uint32_t value)
{
    uint32_t offset;
    if (!voodoo2_decode(state, address, width, &offset)) return false;
    value &= UINT32_MAX >> ((4 - width) * 8);
    state->guest_writes++;
    if (state->async) {
        uint32_t head = state->ring_head;
        if (head - __atomic_load_n(&state->ring_tail, __ATOMIC_ACQUIRE) >= RING_ENTRIES) {
            state->ring_full_waits++;
            while (head - __atomic_load_n(&state->ring_tail, __ATOMIC_ACQUIRE) >= RING_ENTRIES)
                __asm__ volatile("pause");
        }
        Voodoo2Command *command = &state->ring[head & (RING_ENTRIES - 1)];
        command->offset = offset;
        command->value = value;
        command->width = (uint8_t)width;
        command->init = (offset & 0xc00000) ? 0 : init_enable(state);
        command->swap = !(offset & 0xc00000) && (offset & 0x3fc) == SST_swapbufferCMD;
        if (command->swap) __atomic_add_fetch(&state->queued_swaps, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&state->ring_head, head + 1, __ATOMIC_RELEASE);
        return true;
    }
    lock(state);
    uint64_t start = time_us_64();
    state->stats.writes++;
    write_locked(state, offset, width, value);
    state->stats.busy_us += time_us_64() - start;
    unlock(state);
    return true;
}

/* Front buffer through the gamma CLUT into pixels (w x h, packed). */
static void convert_frame(Voodoo2State *state, uint32_t *pixels, unsigned w, unsigned h)
{
    voodoo_t *device = &state->device;
    voodoo_port_render_idle(device);     /* the GPU's pixels first (its own time) */
    uint64_t start = time_us_64();
    state->display_dirty = false;
    state->shown_front = device->front_offset;
    state->shown_width = w;
    state->shown_height = h;
    if (device->clutData_dirty) calc_clut(device);
    for (unsigned row = 0; row < h; row++) {
        const uint8_t *source = state->fb + device->front_offset + row * device->row_width;
        for (unsigned column = 0; column < w; column++) {
            uint16_t packed = source[column * 2] | (source[column * 2 + 1] << 8);
            pixels[row * w + column] = device->video_16to32[packed];
        }
    }
    state->stats.scanout_us += time_us_64() - start;
    state->stats.scanouts++;
}

static bool frame_geometry(Voodoo2State *state, unsigned *width, unsigned *height);

/* Worker, device held: publish the picture when it changed (at most every
 * SCANOUT_US unless the guest core asked for a fresh one). */
static void worker_scanout(Voodoo2State *state)
{
    voodoo_t *device = &state->device;
    unsigned w, h;
    uint32_t *target = state->target;
    bool valid = target && frame_geometry(state, &w, &h) && state->target_capacity >= w * h;
    if (!valid) {
        state->reconvert = false;
        if (state->published_valid) {
            state->published_valid = false;
            __atomic_add_fetch(&state->published_seq, 1, __ATOMIC_RELEASE);
        }
        return;
    }
    bool changed = state->reconvert || state->display_dirty || device->clutData_dirty ||
                   !state->published_valid || state->shown_front != device->front_offset ||
                   state->shown_width != w || state->shown_height != h;
    if (!changed) return;
    uint64_t now = time_us_64();
    if (!state->reconvert && state->published_valid && now - state->last_convert_us < SCANOUT_US) return;
    state->reconvert = false;
    convert_frame(state, target, w, h);
    state->last_convert_us = now;
    state->published_width = w;
    state->published_height = h;
    state->published_valid = true;
    __atomic_add_fetch(&state->published_seq, 1, __ATOMIC_RELEASE);
}

bool voodoo2_worker_poll(Voodoo2State *state)
{
    if (!state || !state->async || state->want_lock) return false;
    voodoo_t *device = &state->device;
    bool queued = __atomic_load_n(&state->ring_head, __ATOMIC_ACQUIRE) != state->ring_tail;
    bool fifo = device->cmdfifo_enabled && !state->fifo_fault &&
        (device->cmdfifo_depth_rd != device->cmdfifo_depth_wr || device->cmdfifo_in_sub);
    bool picture = state->display_dirty || state->reconvert || device->clutData_dirty ||
                   state->published_valid != (state->target != NULL && (device->fbiInit0 & FBIINIT0_VGA_PASS));
    if (!queued && !fifo && !picture) return false;
    lock(state);
    uint64_t start = time_us_64();
    unsigned done = 0;
    while (done < 256 && !state->want_lock) {
        uint32_t tail = state->ring_tail;
        if (tail == __atomic_load_n(&state->ring_head, __ATOMIC_ACQUIRE)) break;
        Voodoo2Command command = state->ring[tail & (RING_ENTRIES - 1)];
        state->command_init = command.init;
        state->command_init_valid = !(command.offset & 0xc00000);
        state->stats.writes++;
        write_locked(state, command.offset, command.width, command.value);
        state->command_init_valid = false;
        voodoo_wait_for_swap_complete(device);
        if (command.swap) __atomic_sub_fetch(&state->queued_swaps, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&state->ring_tail, tail + 1, __ATOMIC_RELEASE);
        done++;
    }
    /* packets left behind by the drain budget or a JSR still need running */
    if (!done && fifo) drain_cmdfifo(state);
    state->stats.busy_us += time_us_64() - start;
    worker_scanout(state);
    unlock(state);
    return done != 0 || fifo;
}

bool voodoo2_set_gpu_after_swap(Voodoo2State *state, V2gState *gpu)
{
    if (!voodoo2_set_gpu(state, gpu)) return false;
    state->gpu_wait_swap = gpu != NULL;
    return true;
}

bool voodoo2_set_gpu(Voodoo2State *state, V2gState *gpu)
{
    if (!state) return false;
    quiesce(state);
    voodoo_port_render_idle(&state->device);
    state->gpu = gpu;
    state->gpu_wait_swap = false;
    state->gpu_from_frame = state->device.frame_count;
    if (gpu) voodoo2_gpu_cpu_wrote(gpu, 0, state->device.fb_mask + 1);
    state->reconvert = true;
    unlock(state);
    return true;
}

static V2gStats voodoo2_gpu_stats_of(Voodoo2State *state)
{
    V2gStats none = { 0 };
    return state && state->gpu ? voodoo2_gpu_stats(state->gpu) : none;
}

voodoo_t *voodoo2_device(Voodoo2State *state)
{
    return state ? &state->device : NULL;
}

bool voodoo2_set_async(Voodoo2State *state, bool enabled)
{
    if (!state || !state->ring) return false;
    quiesce(state);
    state->async = enabled;
    state->reconvert = true;
    unlock(state);
    return true;
}

void voodoo2_set_strict_reads(Voodoo2State *state, bool strict)
{
    if (state) state->strict_reads = strict;
}

void voodoo2_set_queue_busy(Voodoo2State *state, bool busy)
{
    if (state) state->queue_busy = busy;
}

bool voodoo2_set_split(Voodoo2State *state, bool split, bool helper)
{
    if (!state) return false;
    quiesce(state);
    /* A texture cache entry stays in use until every render worker has counted
     * it (voodoo_texture_in_use).  Workers that did not run while the board
     * rendered with one thread lag behind; nothing renders now, so all entries
     * are free: bring every worker's count up to date before switching. */
    for (unsigned tmu = 0; tmu < 2; tmu++)
        for (unsigned entry = 0; entry < TEX_CACHE_MAX; entry++) {
            texture_t *texture = &state->device.texture_cache[tmu][entry];
            for (unsigned worker = 0; worker < 4; worker++)
                texture->refcount_r[worker] = texture->refcount;
        }
    state->split = split;
    state->device.render_threads = split ? 2 : 1;
    state->device.odd_even_mask = split ? 1 : 0;
    state->helper_online = split && helper;
    unlock(state);
    return true;
}

bool voodoo2_write_buffer(Voodoo2State *state, uint32_t address, const uint8_t *buffer, unsigned length)
{
    uint32_t offset;
    if (!buffer || !length || !voodoo2_decode(state, address, 1, &offset) ||
        length > VOODOO2_BAR_SIZE - offset) return false;
    for (unsigned index = 0; index < length;) {
        unsigned width = length - index >= 4 && !((address + index) & 3) ? 4 :
                         length - index >= 2 && !((address + index) & 1) ? 2 : 1;
        uint32_t value = 0;
        for (unsigned byte = 0; byte < width; byte++)
            value |= (uint32_t)buffer[index + byte] << (byte * 8);
        voodoo2_write(state, address + index, width, value);
        index += width;
    }
    return true;
}

static bool frame_geometry(Voodoo2State *state, unsigned *width, unsigned *height)
{
    voodoo_t *device = &state->device;
    if (!(device->fbiInit0 & FBIINIT0_VGA_PASS) || (device->fbiInit1 & FBIINIT1_VIDEO_RESET))
        return false;
    unsigned w = (unsigned)device->h_disp, h = (unsigned)device->v_disp;
    if (w < 320 || h < 200 || w > 1920 || h > 1200 || !device->row_width) return false;
    uint64_t end = device->front_offset + (uint64_t)(h - 1) * device->row_width + w * 2u;
    if (end > FB_BYTES) return false;
    *width = w;
    *height = h;
    return true;
}

/* Guest core: convert here, after every queued write has run. */
static bool frame_now(Voodoo2State *state, uint32_t *pixels, unsigned capacity,
                      unsigned *width, unsigned *height, bool force, bool *updated)
{
    *updated = false;
    quiesce(state);
    voodoo_wait_for_swap_complete(&state->device);
    unsigned w, h;
    bool valid = frame_geometry(state, &w, &h);
    if (valid && pixels && capacity < w * h) valid = false;
    if (valid) {
        *width = w;
        *height = h;
        voodoo_t *device = &state->device;
        bool same = !force && !state->display_dirty && !device->clutData_dirty &&
                    state->shown_front == device->front_offset &&
                    state->shown_width == w && state->shown_height == h;
        if (pixels && !same) {
            *updated = true;
            convert_frame(state, pixels, w, h);
        }
    }
    unlock(state);
    return valid;
}

bool voodoo2_frame(Voodoo2State *state, uint32_t *pixels, unsigned capacity,
                   unsigned *width, unsigned *height)
{
    bool updated;
    if (!state || !width || !height) return false;
    return frame_now(state, pixels, capacity, width, height, true, &updated);
}

bool voodoo2_frame_update(Voodoo2State *state, uint32_t *pixels, unsigned capacity,
                          unsigned *width, unsigned *height, bool force, bool *updated)
{
    if (!state || !width || !height || !updated) return false;
    if (!state->async || !pixels || state->strict_reads)
        return frame_now(state, pixels, capacity, width, height, force, updated);
    /* The worker converts finished frames into pixels; report what it published. */
    *updated = false;
    if (state->target != pixels || state->target_capacity != capacity) {
        state->target_capacity = capacity;
        state->target = pixels;
    }
    if (force) state->reconvert = true;
    uint32_t seq = __atomic_load_n(&state->published_seq, __ATOMIC_ACQUIRE);
    if (!state->published_valid) {
        state->main_seq = seq;
        return false;
    }
    *width = state->published_width;
    *height = state->published_height;
    *updated = seq != state->main_seq;
    state->main_seq = seq;
    return true;
}

Voodoo2Stats voodoo2_stats(Voodoo2State *state)
{
    if (!state) return (Voodoo2Stats){0};
    quiesce(state);
    voodoo_t *device = &state->device;
    Voodoo2Stats stats = state->stats;
    stats.reads = state->guest_reads;
    stats.async = state->async;
    stats.ring_full_waits = state->ring_full_waits;
    stats.sync_reads = state->sync_reads;
    stats.sync_wait_us = state->sync_wait_us;
    stats.status_pending_reads = state->status_pending_reads;
    stats.split_helped = state->split_helped;
    stats.split_local = state->split_local;
    stats.split = state->device.odd_even_mask != 0;
    memcpy(stats.live_counts, state->live_counts, sizeof(stats.live_counts));
    for (unsigned index = 0; index < 258; index++)
        if (state->sync_by_register[index] > stats.sync_top_count) {
            stats.sync_top_count = state->sync_by_register[index];
            stats.sync_top_offset = index == 257 ? 0x2000 : index == 256 ? 0x1000 : index << 2;
        }
    stats.triangles = device->tri_count;
    stats.frame_count = device->frame_count;
    stats.front_offset = device->front_offset;
    stats.draw_offset = device->params.draw_offset;
    stats.aux_offset = device->params.aux_offset;
    stats.row_width = device->row_width;
    stats.h_disp = device->h_disp;
    stats.v_disp = device->v_disp;
    stats.fbi_init0 = device->fbiInit0;
    stats.init_enable = init_enable(state);
    stats.pixels_in = device->fbiPixelsIn;
    stats.pixels_out = device->fbiPixelsOut;
    stats.depth_fail = device->fbiZFuncFail;
    stats.alpha_fail = device->fbiAFuncFail;
    stats.chroma_fail = device->fbiChromaFail;
    stats.pass_through = (device->fbiInit0 & FBIINIT0_VGA_PASS) != 0;
    stats.cmdfifo_enabled = device->cmdfifo_enabled != 0;
    stats.jit = state->jit;
    stats.recompiles = (uint32_t)voodoo_recomp;
    unlock(state);
    return stats;
}

bool voodoo2_owns_display(Voodoo2State *state)
{
    if (!state) return false;
    uint32_t init0 = __atomic_load_n(&state->device.fbiInit0, __ATOMIC_RELAXED);
    uint32_t init1 = __atomic_load_n(&state->device.fbiInit1, __ATOMIC_RELAXED);
    return (init0 & FBIINIT0_VGA_PASS) && !(init1 & FBIINIT1_VIDEO_RESET);
}

void voodoo2_set_caller_source(Voodoo2CallerSource *source)
{
    caller_source = source;
}

unsigned voodoo2_command_changes(Voodoo2State *state, Voodoo2CommandChange *out, unsigned capacity)
{
    if (!state) return 0;
    uint32_t total = state->command_changes;
    unsigned kept = total < VOODOO2_COMMAND_HISTORY ? total : VOODOO2_COMMAND_HISTORY;
    unsigned count = kept < capacity ? kept : capacity;
    for (unsigned index = 0; index < count; index++)
        out[index] = state->changes[(total - count + index) % VOODOO2_COMMAND_HISTORY];
    return total;
}
