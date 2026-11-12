#ifndef VOODOO2_GPU_H
#define VOODOO2_GPU_H
/*
 * Voodoo2 triangles on an Intel Gen8 GPU (Cherryview), portable half.
 *
 * Every triangle the 86Box core would rasterize is turned into one draw of a
 * pixel shader generated from the board's mode registers. The shader repeats
 * the core's per-pixel pipeline (vid_voodoo_render.c, voodoo_half_triangle)
 * with the same integer arithmetic: iterated colours and Z, texture fetch and
 * the texture combine units, colour/alpha combine, chroma key, fog, alpha
 * test, depth test, alpha blending and dithering. It reads the destination
 * colour and depth from the Voodoo2 frame buffer itself and writes them back
 * there, so the frame buffer in RAM stays the only copy and the CPU renderer
 * can take over for any triangle at any time (after voodoo2_gpu_finish).
 *
 * Coverage is exact too: the CPU computes each line's span as the core does
 * (vertices, edges, clipping) and the shader drops pixels outside it; the
 * GPU only rasterizes the bounding rectangle.
 *
 * Differences from the CPU renderer (documented in docs/voodoo2.md):
 * S, T and W are iterated in float (the core: 64-bit integers), the
 * perspective division and the LOD logarithm use the GPU's math unit, and
 * the per-span-part LOD of perspective textures is evaluated per pixel.
 *
 * The GPU half (efi/platform/gpu3d.c, or the simulator in tools/tests)
 * implements V2gBackend.
 */
#include <stdbool.h>
#include <stdint.h>

/* Binding table of every draw (the backend sets up these surfaces). */
enum {
    V2G_BT_COLOR = 0,      /* render target: R16_UINT, the draw buffer */
    V2G_BT_AUX = 1,        /* render target: R16_UINT, the aux (depth/alpha) buffer */
    V2G_BT_FB = 2,         /* buffer R16_UINT: the whole frame buffer, element = byte offset / 2 */
    V2G_BT_TEX = 3,        /* buffer R32_UINT: decoded textures (see tex_index) */
    V2G_BT_TABLES = 4,     /* buffer R8_UINT: voodoo2_gpu_tables() */
    V2G_BT_DATA = 5,       /* buffer R32_UINT: per-batch data (alloc_data) */
    V2G_BT_DATA4 = 6,      /* buffer R32G32B32A32_UINT over the same data, element = 16 bytes */
    V2G_BT_COUNT = 7,
};

/* Per-draw constants (push constants, 10 registers from g6). */
#define V2G_RECORD_DWORDS 80

typedef struct {
    const uint32_t *code;  /* kernel, 16 bytes per instruction */
    unsigned code_bytes;
    unsigned key_id;       /* the same kernel always has the same id */
    uint32_t record[V2G_RECORD_DWORDS];
    int x0, y0, x1, y1;    /* frame-buffer rectangle, inclusive, to rasterize */
    uint32_t draw_offset, aux_offset, row_width;   /* render targets */
    uint32_t rows;         /* render-target height */
    bool hazard;           /* reads pixels an earlier draw since the last barrier wrote */
    uint32_t cpu_lo, cpu_hi;   /* frame-buffer bytes the CPU wrote since the GPU last ran */
} V2gDraw;

typedef struct V2gBackend {
    void *ctx;
    /* room for dwords of per-batch data; *index: its element index in V2G_BT_DATA.
     * NULL when full (the frontend then finishes the batch and retries). */
    uint32_t *(*alloc_data)(void *ctx, unsigned dwords, unsigned align_dwords, uint32_t *index);
    /* element index in V2G_BT_TEX of texture_cache[tmu][entry].data[0] */
    uint32_t (*tex_index)(void *ctx, unsigned tmu, unsigned entry);
    /* false when the batch is full: finish and retry */
    bool (*draw)(void *ctx, const V2gDraw *draw);
    /* run everything queued and wait; afterwards the CPU sees the GPU's pixels */
    void (*finish)(void *ctx);
    /* optional: the CPU rewrote texture_cache[tmu][entry].data[first, end) */
    void (*texture_written)(void *ctx, unsigned tmu, unsigned entry, uint32_t first, uint32_t end);
} V2gBackend;

struct voodoo_t;
struct voodoo_params_t;
typedef struct V2gState V2gState;

V2gState *voodoo2_gpu_create(const V2gBackend *backend);
void voodoo2_gpu_destroy(V2gState *state);
/* true: the triangle is queued for the GPU; false: the CPU must render it
 * (the GPU has then been finished, so the frame buffer is current) */
bool voodoo2_gpu_triangle(V2gState *state, struct voodoo_t *voodoo, struct voodoo_params_t *params);
/* run what is queued and wait, then release the textures it used */
void voodoo2_gpu_finish(V2gState *state, struct voodoo_t *voodoo);
bool voodoo2_gpu_pending(const V2gState *state);
/* the CPU wrote frame-buffer bytes [lo, hi): the GPU must see them */
void voodoo2_gpu_cpu_wrote(V2gState *state, uint32_t lo, uint32_t hi);
void voodoo2_gpu_texture_decoded(V2gState *state, unsigned tmu, unsigned entry, uint32_t first, uint32_t end);
/* The fixed tables behind V2G_BT_TABLES. */
const uint8_t *voodoo2_gpu_tables(unsigned *bytes);

typedef struct {
    uint64_t triangles, cpu_triangles, draws, barriers, finishes, kernels, kernel_bytes;
    uint64_t spans, pixels;
    uint64_t setup_us, draw_us, finish_us;   /* frontend, backend draw, finish (incl. GPU wait) */
    uint32_t last_fallback;   /* why the last CPU triangle was not drawn here (V2G_WHY_x) */
} V2gStats;
V2gStats voodoo2_gpu_stats(const V2gState *state);

enum { V2G_WHY_NONE, V2G_WHY_STIPPLE, V2G_WHY_TILED, V2G_WHY_NO_WRITE, V2G_WHY_RANGE, V2G_WHY_LFB_COLOR,
       V2G_WHY_KERNEL, V2G_WHY_BATCH, V2G_WHY_SLI };

#endif
