#ifndef VOODOO2_GPU_TEST_H
#define VOODOO2_GPU_TEST_H
/*
 * The Voodoo2 GPU back end against the CPU renderer, on a private test board:
 * the same random triangles in 20 modes, once drawn by the 86Box C renderer,
 * once through the back end, and the whole frame buffer compared. Shared by
 * the host test (Gen8 simulator) and the Wyse monitor ("gpu v2test", real
 * GPU).
 */
#include <stdbool.h>
#include "voodoo2.h"
#include "voodoo2_gpu.h"

/* A fresh 12 MB board at 640x480 with test textures in both units; the
 * recompiler is off (the reference is the C renderer). NULL on failure. */
Voodoo2State *voodoo2_gpu_test_board(void);

typedef struct {
    unsigned modes, passed, exact;
    unsigned long long pixels, differ;
} V2gTestResult;

/* only_mode < 0: every mode. verbose: details of differences. */
V2gTestResult voodoo2_gpu_test_run(Voodoo2State *board, V2gState *gpu, unsigned triangles, int only_mode, bool verbose);

#endif
