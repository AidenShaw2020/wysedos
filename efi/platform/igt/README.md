# Intel render headers from IGT GPU Tools

`gen4_render.h`, `gen6_render.h`, `gen7_render.h`, `gen8_render.h` and
`surfaceformat.h` are copied unchanged from IGT GPU Tools
(https://gitlab.freedesktop.org/drm/igt-gpu-tools, `lib/`, revision
`b91fe3bd6c3d9b11746fa2cda7aaa0cd37bc2546`). They describe the Gen4-Gen8 3D
command opcodes and state structures. IGT is MIT licensed; its `COPYING` file is
kept here.

`efi/platform/intel_gpu.c` (`gpu copy`) follows the command sequence and the
pixel-shader kernel of IGT's `lib/rendercopy_gen8.c` (`gen8_render_copyfunc`,
kernel from `lib/i915/shaders/ps/blit.g7a`), adapted to submit through a legacy
ring buffer with absolute GGTT addresses instead of i915 relocations.
