# 86Box Voodoo Core

Source: https://github.com/86Box/86Box

Pinned revision: `a1e92633baf178ff0791cb5624e406da3e8eb257`.

The upstream README states GPL version 2 or later. The original license is
preserved in `86box/COPYING`; this port is used under GPLv3 together with the
project. Original source copyright notices are preserved.

`tools/voodoo/import-86box.ps1` imports the rendering, texture, setup, register,
framebuffer and blitter modules. It mechanically replaces platform includes
with `voodoo_port.h`, removes unused wchar includes, and extracts the original
NCC conversion, color-table initialization and FIFO packet decoder with their
copyright notices. No ROM images, guest drivers or upstream host emulator
are included. Platform-specific threading, display and card construction are
not imported; the board front end provides synchronous ordering instead.

The x86-64 span recompiler header (`vid_voodoo_codegen_x86-64.h`) is used
unchanged; `plat_mmap` is provided by the board front end and returns
platform-allocated executable memory. Defining `VOODOO_PORT_NO_JIT` selects
the C renderer only.

Port deltas submit triangles
synchronously, route packet register/raw writes through the front end, and
reject frame buffer reads/writes that straddle the final allocation byte.
FIFO payload size and supported packet types are checked before entering the
decoder.

`pc/voodoo2.c` is the Voodoo2 (CVG) board: PCI identity and initEnable,
fbiInit gating, status/retrace, DAC/PLL readback, CLUT, video timing, command
FIFO hole tracking and the SST-1/CVG buffer layout are adapted from upstream
`src/video/vid_voodoo.c` and `vid_voodoo_display.c` at the pinned revision
(GPL-2.0-or-later). It does not import the upstream SVGA, PCI, timer or thread
implementation. Swaps complete synchronously and scanout is a bounded CPU copy
of the front buffer through the gamma CLUT while fbiInit0.VGA_PASS is set.

This is an in-progress port, not a claim of guest or hardware compatibility.
