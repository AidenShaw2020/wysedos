# NT display baseline

The miniport and 2D XDDM driver are built from WINVBE revision
`25ee72d97cc6129b79329b249cea337ae88a4e25` (GPLv3). The original source tree
is kept intact. This build links `d3d_disabled.c` in place of the upstream
software Direct3D HAL. It installs only on the emulated VGA adapter
`1234:1111`, with its own `wgpuvbe` service and module names. The separate
WGPU co-processor remains owned by `wgpunt.sys`.

Initial scope is mode enumeration, 800x600x32 startup, GDI drawing and the
existing framebuffer DirectDraw paths. The wrapper adds bounded GPU color
fills as described below; the remaining 2D operations retain the CPU paths.
No hardware Direct3D capability is enabled by this baseline.

Status: all imports checked against actual Windows 2000 system binaries.
The physical VMX backend passed Windows 2000 and XP PnP installation, restart,
800x600x32 login/desktop and a live change to 1024x768x32. The independent
WGPU hardware triangle and unbuffered disk comparison also passed in both
systems with this display driver loaded.

An XP mode change left part of an existing console window undrawn until
the window was maximized and restored. This repaint issue remains open;
successful mode switching does not establish complete drawing correctness.

The next candidate adds `miniport.c`, `display.c` and `transport.c` as wrappers
around the intact upstream sources. A versioned escape forwards only a fixed
query, GPU store self-test or offscreen triangle request through
`EngDeviceIoControl` to the miniport, which sends the corresponding request
to the existing WDM owner. It opens only `\Device\WyseWgpuNT`, requires passive
IRQL and retains its file reference until synchronous or pending I/O completes.
It does not map the other PCI function or accept raw GPU batches/addresses.
Malformed requests are rejected before opening the device; failed payloads
are zeroed. The host tests cover completion, errors, buffer aliasing and
reference lifetime. The transport is built/import-checked and has passed
Windows 2000 and XP guest query, malformed-request rejection, GPU store
self-test and hardware triangle pixel comparison through the display DC.
The INF explicitly replaces both versionless project modules on update;
an earlier XP update copied the miniport while retaining the old display DLL.
The corrected package passed an XP PnP update and both installed-file hashes.

The guest-only `ntdisp` client uses a display DC and `ExtEscape`, with no direct
WDM handle. `--selftest`, `--triangle` and `--invalid` exercise this route.
This establishes a transport for later rendering, not a game Direct3D HAL.

The fill wrapper implements synchronous DirectDraw `DDBLT_COLORFILL` for
linear, unshadowed RGB565 and 32-bit RGB video surfaces. The WDM owner validates
integer surface/rectangle bounds and derives the framebuffer GPU base from
its device registers. The display callback flushes affected CPU cache lines
before and after the completed GPU operation. Other formats and operations
retain the existing CPU callback; a failed GPU submission reports an error.
It never falls back to a CPU write while a failed GPU request may be pending.

The fill build is import-checked, with host coverage of the render builder
and transport. Record guest validation and installed module identities in
the local project ledger, separately from these host checks.
`NTDFILL` requires exact pixels and exactly two GPU submissions per test pair;
it also checks CPU-written pixels sharing cache lines with a GPU rectangle.
The default tests the 32-bit desktop format; `--16` temporarily switches a
test window to 640x480x16 and restores the desktop mode on completion.
This candidate does not provide a game Direct3D HAL.

On XP an update can defer replacement of the loaded display DLL until the
next boot. A BIOS stop immediately after Windows shutdown precedes that
replacement. Complete the boot and compare both display modules and the WDM
module with their build manifests before validating the new rendering path.

The copy wrapper adds same-format, unscaled DirectDraw copies between
disjoint linear RGB565 or 32-bit RGB video allocations. The source and
destination ranges are checked independently before submitting the rectangle
to the render engine. Overlap, scaling, clipping lists and color keys still
use the existing callback. Run `NTDFILL --copy` or `--copy16` to check GPU
completion, every copied pixel, and preservation of the source and the
destination outside the rectangle. Both formats passed on Windows 2000
and XP with the v11 test package. The 32-bit GPU fill regression also
passed on both systems.

The optional `WGPU_D3D_HAL` build adds Direct3D 7 color clears and
pretransformed triangle lists, strips and fans. It checks command and
vertex lengths before taking private snapshots and submitting through
the existing miniport and WDM driver. `WGPU_NT_ENABLE_MATERIAL` adds
single-stage texturing, alpha testing and blending. `WGPU_NT_ENABLE_DEPTH`
adds Z16 surfaces with a linear CPU view and a separate Y-tiled GPU view
in a DirectDraw-managed video memory heap. The baseline uses its Direct3D
stub.

For material batching, compile the display, miniport and WDM owner with
`WGPU_NT_ENABLE_GROUPS` together with `WGPU_NT_ENABLE_MATERIAL`.
The display combines neighboring draws with identical material state.
The preview's matched build can pack up to 32 material records in a request. The WDM driver
checks the records, builds separate command and shader slots and
chains them with the same Gen8 cache transitions as the Win9x driver.
One completion fence covers the group. Target changes, clears and CPU
texture copies drain pending draws. The display also drains pending
draws before returning from the Direct3D callback in the baseline.
The preview keeps batches on eligible hidden video-memory targets within
a scene and drains them before CPU access or presentation.

`NTD3D` requests the Direct3D 7 HAL device, draws triangles and checks the
resulting pixels and completed GPU work. `--texture`, `--alpha` and
`--depth` exercise the optional paths. The Z16 probe checks occlusion and
exact CPU readback. Use the local ledger for installed versions, physical
validation and game compatibility.

The Direct3D build exposes a cumulative 32-bit presentation counter through
the read-only `WGPU_DISPLAY_PRESENT_COUNT` escape. It advances after the
display driver accepts a flip that changes the scanout address. Calculate
FPS from a counter delta and the elapsed time of the measured race segment.
Use this counter for a fullscreen flip chain. Blit-based and windowed
presentations need their own presentation measurement.
