# Voodoo2 add-in board (experimental)

The 3D clone models an 8 or 12 MB 3dfx Voodoo2 (CVG) PCI add-in board next to the
standard VGA. It replaced an earlier Voodoo3 experiment: Voodoo3 Glide on
Windows 9x finds the board only as an attached Windows display, while a
Voodoo2 is owned directly by Glide and switches the monitor itself.

## Enabling

`config.ini`, section `[pc]`:

```ini
voodoo2=1
voodoo2_mem=12
```

Optional keys: `voodoo2_async=0` renders on the main core instead of core 3
(core 3 otherwise renders, core 2 helps with every other line, and the
headphones and General MIDI move to core 1); `voodoo2_turbo=0` keeps the set
processor speed while the board shows its picture (by default the guest runs
unrestricted while fbiInit0 VGA pass-through is on, and the set speed returns
when the game releases the monitor).

The guest then sees PCI `121a:0002`, revision 2, class 0400, with one 16 MB
memory BAR. Use a Voodoo2 Glide driver kit in the guest (DOS `GLIDE2X.OVL`,
Windows 9x Glide2/Glide3 DLLs). Proprietary 3dfx drivers are not bundled.
For Windows, the bundled [Velocity9x package](../drivers/windows9x/velocity9x/README.md)
uses its own WGPU path and does not require a Voodoo2 Windows driver.

GPU settings keep hardware on/off separate from `CPU Boost DOS: Glide`.
The DOS boost follows Voodoo2 display ownership. Windows has a separate
`CPU Boost Windows: DirectX/Glide/OpenGL` switch, activated by actual 3D
submissions in the Windows driver. Neither switch installs a Windows helper.

## What is modelled

- PCI identity, BAR sizing/decoding, initEnable (config 0x40) with fbiInit
  write gating and DAC readback; config registers above 0x43 read as zero
  (Glide's silicon-process probe would otherwise poll forever).
- fbiInit0-7, video timing (hSync/vSync/backPorch/videoDimensions), retrace
  status and vRetrace from a nominal 60 Hz clock, DAC/PLL identification,
  33-entry gamma CLUT.
- SST-1/CVG frame buffer layout, linear frame buffer reads/writes, texture
  downloads for both TMUs, triangles, fast fill, swaps (completed at once).
- Command FIFO in frame buffer memory with hole tracking, packet types 0-5 and
  JSR/RET.
- Scanout: while fbiInit0.VGA_PASS is set the monitor shows the Voodoo2 front
  buffer through the gamma CLUT instead of the VGA picture. The front buffer
  is converted only when the visible image may have changed (swap, front
  buffer LFB write or drawing, CLUT/timing change).

Rendering uses the pinned 86Box core (`pc/voodoo/UPSTREAM.md`). On x86-64 the
86Box span recompiler is used; its code buffer is allocated as EFI loader code
pages. The C renderer remains available (`v2jit 0`) and the host suite checks
that both produce identical pixels.

## Developer monitor commands

The following commands require a private local UDP development rig. That rig
is not distributed as a public remote-control toolkit; ordinary users use the
GPU settings menu. These commands are implementation diagnostics.

| Command | Purpose |
| --- | --- |
| `v2stats` | access/triangle/swap counters, time spent rendering and converting, PCI state, command history |
| `v2jit 0/1` | switch the span recompiler off/on |
| `v2async 0/1`, `v2split 0/1/2`, `v2turbo 0/1`, `fastmmio 0/1` | rendering placement, line split, processor turbo, exit-handler decoding |
| `cpuspeed N` | processor speed for this session (MHz, 0 unrestricted) |
| `gregs` | guest CS:EIP, flags, segments and general registers |

## Limits

- The default DOS renderer runs on CPU cores 3 and 2 with the span JIT.
  The replacement 640x480 Tomb Raider level-start sample gives 21.71 FPS
  after the idle Windows GPU ring suspension fix; this is not a whole-game average. See the [recording and method](video/README.md).
- A manual experimental Intel GPU backend exists in source but is not enabled
  by the shipped defaults or exposed as a supported DOS mode. Windows
  Velocity9x uses Intel Gen8 separately; its performance does not establish
  DOS Glide acceleration.
- No SLI or interrupts; emulated swaps complete without waiting for real retrace.
- Hardware validation so far covers driver detection, DOS Glide2 and Windows
  9x Glide3 start, rendering, menu/game transitions and normal exit; it is not
  a compatibility claim.

## Tests

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/tests/voodoo2.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/tests/voodoo2.ps1 -Optimization -O0 -Sanitize
```
