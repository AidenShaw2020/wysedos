# NT WGPU bridge

Work in progress for 32-bit Windows 2000/XP. The bridge and console client build
as PE32 targeting NT 5.0. Their imports have been checked against Windows 2000
SP4 binaries. On the physical VMX backend, Windows 2000 and XP have completed PnP
installation, device queries and the fixed GPU store/completion test without
an engine reset. The offscreen Gouraud triangle and its RGB565 pixel checks
have also passed in both systems. Windows XP additionally passed sixteen
consecutive additional draws without an engine reset.
This is separate from the XDDM display driver
and Direct3D HAL.

The first interface queries the assigned WGPU PCI device and runs a fixed
`MI_STORE_DATA_IMM` maintenance batch. A successful real guest self-test must
show the expected GPU write and completion fence. It does not establish
accelerated Direct3D support.

Only the emulator's full PCI subsystem ID may match the installation INF.
The driver must use PnP-assigned resources, serialize access and stop using
the mappings before device removal. It must not expose arbitrary physical
memory or raw privileged GPU batches to applications.

The INF restricts the device to administrators and SYSTEM. This uses the
[per-device registry properties](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/setting-device-object-registry-properties-during-installation)
described by Microsoft. `ntprobe` only queries by default; `ntprobe --selftest`
requests the fixed maintenance batch. Device removal and suspend/resume remain
untested in a real NT guest.

`wgpu_batch.c` is shared with a host test of the exact descriptor and batch
layout. The protocol authority remains `pc/wgpu.h`; existing Velocity9x GPU
state and shader builders are candidates for the later NT rendering backend.

`ntprobe --triangle` requests a fixed 128x128 RGB565 Gouraud triangle in the
WGPU aperture. The kernel builds all commands, shader code and addresses;
the caller cannot supply raw GPU commands. After completion, the client
compares interior colours with an integer barycentric reference and checks
the untouched exterior. It saves a successful result to `C:\NTGPU.BMP` using
`CREATE_NEW`, preserving any earlier image. Presentation of that bitmap uses
ordinary Windows drawing; it is not an accelerated desktop or Direct3D HAL.
The draw builder matches the existing DOS GPU diagnostic byte for byte on
the host. The linked kernel image is checked for FPU/SIMD instructions.

The guest-only `ntinstall --install` helper uses the adjacent `WGPUNT.INF`
and the exact subsystem ID through `UpdateDriverForPlugAndPlayDevicesA`.
It uses normal ranking and signature prompts, without forced binding or
automatic restart. It refuses OS versions outside NT 5.0/5.1. Its build and
Windows 2000 import checks have passed, and it has updated the driver in
physical Windows 2000 and XP without requesting a restart. The separate
`--display` option targets only the emulated VGA adapter with the adjacent
`WGPUDSP.INF`; its framebuffer/2D package is described in [display/README.md](display/README.md).
Both systems have passed display installation, restart and live mode switching;
the display README records the remaining repaint limitation.
For manual installation, point Device Manager's **Have Disk** source to the
directory containing the INF, not the INF filename.

The bounded draw request accepts up to 96 screen-space vertices with packed
colors and a validated linear RGB target. The WDM driver derives the GPU
address from its own framebuffer registers and constructs every command.
`NTDISP --draw` uses two fixed triangles in the private aperture to test this
builder, independently of the Direct3D runtime. The result includes the
completed fence and a pixel comparison. The accelerated Direct3D display
build and its separate validation are documented in the
[NT5 support notes](../../docs/nt5-support.md).

## OpenGL development build

The NT5 OpenGL ICD uses Velocity's OpenGL front end with a DirectDraw 7
surface adapter and the NT Direct3D 7 HAL. The firmware receives the same
validated application 3D submissions as it does for Direct3D. OpenGL
uses the configured guest CPU speed.

Run `tools/installer/build_nt5_opengl.ps1 -Watcom <Open-Watcom-path>` from
the project root to build `WGGLNT.DLL`. The build verifies the pinned
Velocity source archive and applies the NT surface patch in its build
directory. Build the XDDM display driver with `WGPU_NT_OPENGL_ICD` and
install both modules from an INF that registers `WGGLNT` under
`OpenGLDrivers`. The shared driver image builder checks the INF and ICD
together against the NT package manifest.

`NTGLCHK.EXE` reports the display driver's ICD identity and checks rendered
pixels and SwapBuffers. It also checks Z16 occlusion, single-level texture
sampling, scissored drawing and drawing after a window resize in the same
context. The experimental XDDM build with `WGPU_NT_ENABLE_MIPS`, the
material and depth paths, and `WGPU_NT_SCENE_GROUPS` gives each mip chain
contiguous video memory in the Gen8 layout. Use the matched transport
driver and ICD. The ICD keeps a scene open while submitting consecutive
draws and ends it before presenting, reading pixels, or updating a texture.
The display driver can group work on an offscreen VRAM target when its
allocation is outside the primary framebuffer and scanout.
`NTGLCHK.EXE --mip` checks the base level and lower-level sampling with
both point and linear mip filters. `--lightmap` checks RGB multiplication
and an in-place texture update. Additional texture units, display-mode
recovery and game compatibility require further work.

The experimental `-SingleBuffer` build adds a format for applications
that draw and read the front buffer. `NTGLCHK.EXE --single` verifies
its defaults and front pixels. Full-screen compatibility is still under
test. Large 32-bit drawables can exceed the current video memory budget
for their colour and depth surfaces.
