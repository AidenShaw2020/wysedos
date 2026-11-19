# Windows 2000 / XP support

Windows 2000 and Windows XP have completed physical NTFS installation,
restart, desktop boot and an unbuffered write, flush, reopen and read
comparison on disposable storage. Both systems have exercised GPU-backed
DirectDraw copy and fill with pixel and completion-fence checks. A fixed
offscreen Gouraud triangle also passed RGB565 pixel comparison on both.

The experimental Direct3D 7 HAL has passed physical Windows 2000 probes
for color clear, indexed and unindexed geometry, RGB565 textures,
alpha testing, framebuffer blending and Z16 occlusion. The Z16 probe
also verified exact CPU readback after GPU completion. Windows 2000 and
XP have run a legacy Direct3D 5 application through this HAL on the
physical GPU. The preview batches material draws into fenced GPU
submissions. Offscreen video-memory targets can hold work across
callbacks and synchronize before CPU access or presentation. Both
systems passed physical pixel checks and game rendering.
Windows 2000 also completed a clean installation on the native Wyse
guest disk with the monitor started through the test PXE profile.
The current display package and repaint limitations are documented separately.
[Display package](../drivers/nt/display/README.md)

The hardware OpenGL ICD passed mipmap filter selection, depth, colour
blending before and after a texture update, scissoring, window resize and
buffer presentation on both systems. In Quake II test at 640x480 RGB565/Z16,
with an unrestricted CPU, the 128-view opening-scene rotation measured
28.29–29.44 FPS on Windows 2000 and 29.53–29.71 FPS on XP. The previous
unbatched driver measured about 19–20 FPS under the same conditions.
[Video and benchmark notes](video/README.md)

NT Glide and broader OpenGL/Direct3D game compatibility are outside this
preview. Use the documented 32-bit NT versions and the matching driver set.

## Alternative installations

The same project and monitor must support both OS families, using the same
virtual devices and WGPU protocol. A target PC may be installed with either
DOS/Windows 9x or Windows NT; simultaneous installation and a shared boot menu
are not required. NT boots its own kernel through
NTLDR; it does not run on top of DOS. The monitor and its virtual devices remain
present under either OS.

Initial NT target profile is approximately 400 MHz and 64 MiB guest RAM.
64 MiB is already available in the memory menu; higher Pentium rate choices
extend through 400 MHz. This setting is an execution-rate target, not a
cycle-accurate Pentium II model. Each alternative installation can save its own
profile; automatic switching between OS families is not required. Do not resize
guest RAM live after the BIOS has handed control to an OS.

The VMX CPU backend virtualizes the guest's `IA32_SYSENTER_CS`,
`IA32_SYSENTER_ESP` and `IA32_SYSENTER_EIP` MSRs through the VMCS, matching
the advertised CPUID SEP capability. Reads return guest values; writes respect
privilege and canonical address checks. These operations do not write the
host's hardware MSRs. See [Intel SDM, VMX handling of SYSENTER/SYSEXIT](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3c-part-3-manual.pdf).
The corrected backend has passed Windows XP installation, its own restart,
desktop startup and the guest storage integration program.

The CPU backend also handles guest `IA32_PAT` reads/writes through the
VMCS. Invalid memory-type bytes and privileged accesses raise guest exceptions;
the host PAT is unchanged. CPUID hides unimplemented MTRRs and PCID, as well
as AVX-dependent features and extended-state enumeration that this backend
cannot support. Host tests exercise the actual MSR exit cases, all PAT byte
encodings, fault paths, real-mode/VM86 privilege and CPUID dependencies. The
original dispatch fails the PAT readback control. The candidate has passed
physical Windows 2000 and XP desktop startup, GPU triangle/pixel/fence checks
and the unbuffered disk comparison. Exhaustive PAT instruction checks remain
host tests. A DOS/9x regression has also passed Windows 98 startup and a
Windows-owned return to real DOS, followed by the existing DOS GPU store and
triangle diagnostics. This does not establish game performance or complete
DOS/9x compatibility. PAT rules
follow [Intel SDM volume 3A, section 13.12](https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf).

ATA data-out processing uses the same state machine for every operating
system. After receiving a block, the controller clears DRQ and reports BSY;
completion publishes the next DRQ or command-complete interrupt. Accelerated
REP OUTS accounts for the selected PIO0/1/2 bus cycle time, and slower CPU
transfers do not pay that time twice. This follows the data-out protocol and
timing tables in [ATA-3](https://courses.mpi-sws.org/os-ss11/assignments/pintos/specs/ata-3-std.pdf).
No installer detection or manual timing switch is required. The temporary
`ide_win2k_install` option and its monitor commands have been removed.
Controller tests cover state order, bus timing, reset, media replacement and
backend errors. A physical Windows 2000 test compared every DWORD of a 4 MiB
file after unbuffered write-through writes, flush, close and reopen. This is
an integration check, not proof of every ATA command or error-recovery path.
The Windows 2000 installation also passed the previously failing device phase
without an OS-specific workaround. Windows XP installation, restart and the
same 4 MiB comparison also passed on the physical VMX backend. Broader
controller and DOS/9x regression coverage is still required.

For each NT version, complete installation on disposable storage and reach the
desktop after the installer's own restart. Run the guest integration program
from `tools/tests/nt_ide_io.c` inside that guest. It creates `C:\NTIOTEST.BIN`
with `CREATE_NEW`, so an existing file causes refusal rather than replacement.
It compares all 4 MiB after unbuffered write-through writes, flush, close and
reopen; success prints `PASS` and removes only its own verified file. A failure
retains that file for investigation. Never run this guest integration program
as a host-side storage test. After testing, use a Windows-owned restart and
stop at the BIOS before copying the guest disk as a clean restore point.

The physical GPT/ESP belong to the monitor. Guest partitioning must stay inside
the native guest container. Before replacing a DOS/9x installation with NT,
retain a verified recovery copy. Native installation retains the hidden monitor
partition and gives the selected OS family all remaining guest disk space,
matching the DOS installation policy. In the physical Windows 2000 test,
formatting the existing DOS partition left its DOS bootstrap code in
place. Initializing the hosted disk before Windows Setup let Setup create
its own partition and boot sector. The first installer restart and a
later normal restart both loaded Windows from eMMC. The tested
preparation was a one-shot PXE operation and left the installed monitor
intact. The distribution installer now uses the same unpartitioned guest
disk for DOS/Windows 9x and Windows NT. Driver packages are supplied on
`drivers.iso` in the firmware partition. Physical validation of that
installer entry point is pending.
NT tests need at least 5 GiB for the
system and games; this minimum is not a native partition-size cap. Do not
resize or erase the physical eMMC until a complete verified physical backup
exists. Check actual free space before choosing partition sizes; a test layout
on a larger disposable disk is not evidence it fits the installed eMMC.

## Sound Blaster 16

The firmware exposes the enabled SB16 as ACPI device `PNPB003` when the
operating system supports the `Windows 2001` ACPI interface. The device uses
I/O at 220h, 330h and 388h, IRQ 5, and DMA channels 1 and 5. A physical XP
SP3 test bound the inbox `ctlsb16` driver automatically and played game audio.
Windows 2000 and Windows 9x discover the same fixed hardware through their
legacy setup path. The interface check prevents a duplicate ACPI device
from conflicting with that device on Windows 2000. On the same firmware
build, a physical Windows 2000 test played a WAV through its single default
Sound Blaster 16, and XP loaded the inbox driver and played startup audio.
[Microsoft ACPI operating system interfaces](https://learn.microsoft.com/en-us/windows-hardware/drivers/acpi/winacpi-osi)

## Graphics boundary

The guest sees Bochs-compatible VGA (`1234:1111`) and a separate WGPU render
channel. It does not own the physical Intel display engine. An Intel XP driver
for a different GPU cannot be made compatible just by changing its INF ID.

NT requires an XDDM display miniport and display driver, distinct from the
Win16/VxD/DirectDraw entry points used by Velocity9x. Reuse the existing neutral
Gen8 command/shader builders and WGPU protocol where compatible, while keeping
NT memory mapping, synchronization and process cleanup in the NT driver.
Basic VGA/VBE output is a boot milestone, not proof of GPU acceleration.

An existing source candidate is [WINVBE](https://github.com/ages2001/WINVBE),
GPL-3.0, reviewed at revision `25ee72d97cc6129b79329b249cea337ae88a4e25`.
It separates `SRC/nt/mini/winvbe.c`, `SRC/nt/disp/winvbe.c` and
`SRC/nt/disp/d3dnt.c`. Its NT Direct3D adapter currently calls a software
rasterizer. Reusing its NT interfaces would still require a WGPU/Gen8 backend,
NT resource ownership, synchronization and accelerated-render validation.
Its miniport and 2D driver now form the separately named display package,
with the experimental WGPU HAL handling Direct3D. Guest pixel checks and
completed monitor fences verify which tests reach the physical GPU.

References: [Microsoft display driver models](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/migrating-to-the-windows-vista-and-later-display-driver-model),
[Intel supported operating systems](https://www.intel.com/content/www/us/en/support/articles/000005526/graphics.html).

## Backup mode

`emmc-backup.ini` on a dedicated netfs root requests backup before `pc_new`,
before any guest or helper can write storage. Its exact ASCII contents are:

```text
wyseDOS full eMMC backup v1
```

The final newline is required. The mode requires exactly one fixed whole-disk
UEFI Block I/O device with a qualified existing wyseDOS GPT layout and no USB
node. The primary GPT header and table checksums, ESP and private guest
container are validated by the installer's read-only layout parser. It reads
every sector, including primary/backup GPT, ESP and the guest container, into
`emmc-full.img.partial` on the netfs server. It reopens the output, checks its
length, independently reads the entire source again and compares every byte.
Only successful comparison permits renaming to `emmc-full.img`. Existing final
or partial output is refused. Failures preserve partial evidence. No physical
disk-write API is used. The monitor halts after either result and does not boot
a guest or enter the installer. The host must then hash and inspect the image.

This mode is opt-in. Normal boot without the marker retains its existing path.
The previous `emmcbackup` diagnostic is not a full physical snapshot.

## Remaining qualification

The complete release USB-to-hidden-ESP installation path requires a fresh
physical validation with this exact combined image. Additional display
mode recovery and game coverage remain in progress.
