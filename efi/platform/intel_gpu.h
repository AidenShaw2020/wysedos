#ifndef INTEL_GPU_H
#define INTEL_GPU_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t identity, command, mmio_low, mmio_high, aperture_low, aperture_high, ggc;
} IntelGpuPCI;

typedef struct {
    uint32_t identity, command, ggc;
    uint64_t mmio, aperture, ggtt_first, ggtt_vga_window;
    bool bus_master_enabled;
} IntelGpuInfo;

bool intel_gpu_decode_pci(const IntelGpuPCI *pci, IntelGpuInfo *info);
void intel_gpu_log_probe(void);
/* Monitor commands: "gpu status" reports the GT, "gpu blt" has the blitter fill
 * 32x32 pixels in RAM away from the screen and checks them. */
void intel_gpu_status(void);
void intel_gpu_blt_test(void);
void intel_gpu_copy_test(bool fill);   /* "gpu copy": the 3D engine copies a 64x64 texture */

#endif