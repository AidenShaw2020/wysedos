# Architecture

[Documentation index](index.md)

## CPU and firmware boundary

UEFI loads the x86-64 monitor, which constructs a legacy PC and boots its
SeaBIOS guest. Normal CPU execution uses VT-x with unrestricted guest support
and EPT. I/O and selected memory/system operations exit to PC device models.
Interpreter helpers handle operations requiring software treatment. CPU
identities and instruction-rate limits approximate older PCs, not their cycles.

The native limiter also budgets actual guest core cycles to bound short loops.
Supported VM-entry/exit controls enable the fixed instruction and cycle
counters only in the guest; monitor/device work is not charged to its budget.
Cycle credit is capped at 50 microseconds of the selected rate. This prevents
accumulated I/O/idle credit from letting a short calibration loop run at the
Atom's full speed, but does not provide historical cycle-accurate execution.

The current monitor **keeps UEFI Boot Services active** for firmware USB I/O,
block access, networking and timer/event services alongside direct hardware
access. There is no Linux or Windows host, but firmware drivers remain runtime
dependencies. The monitor does not call `ExitBootServices`. `usbhid.c` is not
an independent physical xHCI host-controller driver.

## CPU cores

| Core | Main work |
|---|---|
| 0 | Guest execution, PC devices, firmware calls, input/storage polling |
| 1 | OPL synthesis, audio mixing and display-audio pumping |
| 2 | VGA snapshots, rendering and framebuffer output |
| 3 | General MIDI synthesis, analog FIFO feeding and hang monitoring |

`mp.c` discovers processor identities with firmware MP Services, then starts
helpers through INIT/SIPI. Helpers must not call UEFI. Device state shared
across cores needs explicit ownership; [SB DMA](sound-dma.md) illustrates why.

## Graphics

The guest sees VGA/EGA and a Bochs-compatible VBE PCI device (`1234:1111`).
DOS accesses VGA directly; Windows can use VMDisp9x. Guest drivers never
control the physical Intel display hardware.

Suitable modes use Cherry Trail's legacy VGA aperture/registers. Ordinary
16-color planar graphics use the software VGA window by default, preserving
native CPU execution and avoiding costly physical read/latch-copy transitions.
Native text, existing chain4 and 256-color Mode X choices remain separate;
the explicit native override is retained. VBE uses software memory. The display core takes
snapshots and renders/scales them into the UEFI GOP framebuffer in `wyse.c`.
Firmware establishes the physical display mode; guest resolution is separate.
There is no `chvdisp.c` implementing independent native display initialization.

Automatic planar fallback accounts for both visible width and virtual pitch.
Double-buffer requests latch start address and fine pan atomically. See
[VGA page flips](vga-page-flips.md) for hardware limits and snapshot timing.
This is not cycle-accurate CRT beam emulation.

VBE enable/fixup/clear holds the display core. A clearing mode change must not
fetch the previous physical VGA contents back over the new frame from either
the CPU handoff or renderer; `NOCLEARMEM` still preserves the existing pixels.

## Audio and input

Sound Blaster PCM/DMA, OPL, MIDI, PC speaker, Tandy and parallel-port sound
feed the mixer. `lpe.c` drives display audio using actual scanout timing.
`analog.c` directly programs the RT5672/I2S path on the qualified unit; this
is not a generic PC HDA backend. See [audio clocks](audio-clock.md).

Physical HID uses firmware `EFI_USB_IO_PROTOCOL`. Parsed reports become
guest i8042/PS/2 keyboard and mouse data. DOS and Windows can therefore use
conventional input drivers. Hotplug cleans up ownership and held keys/buttons;
firmware enumeration can still pause delivery.

Guest USB storage uses a virtual PIIX3 UHCI controller and BOT/SCSI mass-storage
device backed by firmware-accessible physical disks. The physical controller
is not passed directly to Windows. [Storage ownership](storage.md) prevents
guest and monitor filesystems from writing the same external disk concurrently.

## Source map

| Source | Responsibility |
|---|---|
| `efi/src/main.c` | UEFI entry and startup |
| `efi/platform/wyse.c`, `cpuvmx.c` | Main loop, GOP output, VMX/EPT |
| `efi/platform/hwvga.c` | Physical legacy VGA |
| `efi/platform/usbhid.c`, `firmware_events.c` | HID, firmware events/timer |
| `efi/platform/bio.c`, `hostblk.c`, `blkcache.c` | Firmware block access, discovery and caching |
| `efi/platform/native_disk.c`, `image_media.c`, `installer.c` | Guest disk bounds, image ownership, eMMC preparation |
| `efi/platform/audio.c`, `analog.c`, `lpe.c` | Mixer and physical audio |
| `pc/` | Legacy PC devices, CPU helpers, settings and disk menus |
| `efi/fatfs/` | Monitor-side FAT filesystem |
