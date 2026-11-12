#ifndef GPU3D_H
#define GPU3D_H
/*
 * Triangles on the Cherryview GPU's 3D engine, for the Voodoo2 back end.
 *
 * gpu3d_open() takes the render engine (legacy ring buffer, forcewake held)
 * and its own region of the GGTT (from 1.25 GB, outside the CPU's aperture);
 * gpu3d_map() enters ordinary RAM there so the GPU can draw into it or sample
 * from it. A draw builds one batch - the whole 3D state as IGT GPU Tools'
 * rendercopy_gen8.c sets it up, then the triangles - submits it and waits for
 * its completion dword. The CPU's caches are written back before and dropped
 * after, so CPU and GPU can take turns on the same memory.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float x, y;            /* pixels */
    float r, g, b, a;      /* 0..1 */
    float s, t;            /* texture coordinates, 0..1 over the texture */
} G3dVertex;

typedef struct {
    uint32_t color;        /* GPU address of an RGB565 surface */
    uint32_t pitch;        /* bytes */
    uint32_t width, height;
    uint32_t texture;      /* GPU address of an ARGB8888 texture, 0 for none */
    uint32_t tex_width, tex_height, tex_pitch;
    bool bilinear;
} G3dTarget;

bool gpu3d_open(void);
void gpu3d_close(void);
bool gpu3d_is_open(void);
/* The GPU address of cpu (identity-mapped RAM), entering its pages in the GGTT. */
uint32_t gpu3d_map(const void *cpu, uint32_t bytes);
/* count vertices, three per triangle; false when the GPU did not finish */
bool gpu3d_draw(const G3dTarget *target, const G3dVertex *vertices, unsigned count);
/* write back / drop the CPU's cached copy of memory the GPU uses */
void gpu3d_flush_cpu(const void *cpu, uint32_t bytes);
/* the same for memory only render targets and the sampler touch (snooped) */
void gpu3d_flush_data(const void *cpu, uint32_t bytes);
/* zeroed pages below 4 GB, entered in the GGTT (*gpu: their GPU address) */
void *gpu3d_alloc(uint32_t pages, uint32_t *gpu);
/* gpu3d_execute() runs a batch and waits for it (the ring stores its own
 * completion dword after a flush, so the batch's store of seqno to
 * gpu3d_status_gpu() is no longer what is waited for); false on timeout, and
 * the render engine is then reset and restarted with an empty ring. */
uint32_t gpu3d_status_gpu(void);
/* MMIO register access (opens the engine first) */
uint32_t gpu3d_read(uint32_t reg);
void gpu3d_set_coherent(bool on);
void gpu3d_write(uint32_t reg, uint32_t value);
uint32_t gpu3d_next_seqno(void);
bool gpu3d_execute(uint32_t batch_gpu, uint32_t seqno, uint32_t timeout_us);

/* The guest's render channel (pc/wgpu.h, gpu3d_guest.c): queue a batch with
 * cache maintenance and a fence write after it, without waiting; returns a
 * WGPU_SUBMIT_* code. gpu3d_poll() resets the engine when queued work has
 * completed nothing for 3 s; gpu3d_resets() counts resets. */
int gpu3d_submit_guest(uint32_t batch_gpu, uint32_t fence_gpu, uint32_t fence_value);
void gpu3d_poll(void);
bool gpu3d_busy(void);
uint32_t gpu3d_resets(void);
void gpu3d_report(void);

/* monitor commands: "gpu tri" (Gouraud), "gpu tex" (texture x colour) */
void gpu3d_test(bool textured);

/* Voodoo2 back end (gpu3d_v2.c): the GPU half for a board, made once per
 * board; "gpu v2test" compares it with the CPU renderer on a test board. */
struct voodoo_t;
struct V2gBackend;
bool gpu3d_v2_backend(struct voodoo_t *dev, struct V2gBackend *backend);
void gpu3d_v2_test(unsigned triangles, int mode);
void gpu3d_v2_report(void);

#endif
