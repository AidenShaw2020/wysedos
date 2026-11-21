# Configuration and controls

[Documentation index](index.md)

## Menus

The left Windows key is the host key. Implemented platform shortcuts are:

| Shortcut | Action |
|---|---|
| Win+F11 | Settings: CPU, RAM, sound, input, display |
| Win+F12 | Disk Manager: media, ownership and boot selection |
| Win+Delete | Reset guest PC |
| Win+R | Reboot physical Wyse |

Save in the menu to persist changes in `386/config.ini` on the firmware
partition. Identity/RAM changes can require a guest restart; the menu reports
this. Reset/reboot loses unsaved guest work: shut down Windows normally first.

The current section name is `[wyseDOS]`. Older `[X86Pi]` sections remain
readable for backward compatibility. Saving from either menu rewrites the
settings under `[wyseDOS]` with the same values; no manual migration is needed.

## Display output

Under **Win+F11 → GPU → Display output**, choose **Automatic** to use the
display that the firmware selected at boot. **Monitor 1 only**, **Monitor 2
only**, and **Mirror** take effect immediately. DOS or Windows resumes when
you close Settings. Press **Enter** to keep the selection for the next boot.

Mirror uses monitor 1's 1080p60 timing and digital audio. Both outputs scan
the same framebuffer. This mode requires two HDMI or DVI (TMDS) sinks,
such as monitors connected through passive DisplayPort-to-HDMI adapters,
with 1080p60 support on both. Connect them before starting the Wyse. If
mirror is unavailable at boot, the monitor uses the first output when
available.

## CPU and memory

Choose the processor separately from its speed in the CPU settings page.
Save and restart to apply an identity or RAM change. The image defaults
to the Pentium/133 profile and 128 MB RAM.

| `[pc] cpu` | Menu profile | Clock limit presets | Default |
|---|---|---|---|
| 3 | 386 | 16–40 MHz | 40 MHz |
| 4 | 486 | 25–120 MHz | 66 MHz |
| 5 | Pentium | 60–400 MHz (includes existing compatibility profiles) | 133 MHz |
| 6 | Pentium II | 233–450 MHz | 400 MHz |
| 7 | Pentium III | 450–1400 MHz | 800 MHz |

Each list ends in full speed (`[wyseDOS] speed=0`). The saved value is an
approximate execution-rate limit. It does not model the original CPU bus
or change the underlying hardware clock. Pentium II and III advertise
different family-6 CPUID signatures. Pentium III advertises SSE when the
backend supports it. These profiles expose one CPU and omit unsupported
extensions. Existing 386, 486 and Pentium identities keep the previous
backend CPUID behavior. [Intel Pentium II clock range](https://www.intel.com/pressroom/archive/releases/1998/dp082498.htm)
[Intel Pentium III 1.40 GHz](https://www.intel.com/pressroom/archive/releases/2002/20020108comp.htm)

## Common settings

| Section/key | Meaning |
|---|---|
| `[pc] mem=16M` | Guest RAM, not physical Wyse RAM |
| `[pc] vga_mem=8192K` | VGA/VBE memory |
| `[pc] cpu=5`, `fpu=1` | Default Pentium profile and enabled FPU |
| `[pc] bios`, `vga_bios` | Runtime ROM filenames |
| `[pc] hda=host:emmc` | Native bounded system disk |
| `[pc] fda`, `fdb` | Floppy sources; prefer Disk Manager |
| `[pc] usb_storage=1` | Automatic guest USB disk exposure |
| `[pc] timezone=cet` | Central European RTC conversion including summer time; `utc` disables conversion |
| `[pc] firmware_timer_us=5000` | Requested firmware timer period; `0` preserves firmware timing |
| `[wyseDOS] speed` | Approximate execution-rate profile |
| `[wyseDOS] graphics_turbo=1` | Full CPU speed during accepted Windows 3D work from the matching Velocity driver |
| `[wyseDOS] voodoo2_turbo=1` | Independent full CPU speed while DOS Voodoo2 owns the display |
| `[wyseDOS] volume`, `vol_*` | Master and per-source levels, 0–16 |
| `[wyseDOS] pcspeaker`, `adlib`, `soundblaster`, `mpu401` | Audio enable flags |
| `[wyseDOS] tandy`, `covox`, `dss` | Optional audio sources |
| `[wyseDOS] mouse`, `mouse_speed`, `mouse_invert_y` | Mouse enable, percent scaling, Y inversion |
| `[wyseDOS] picture` | Layout: choose aspect/fill/integer options in Settings |
| `[wyseDOS] display_output` | `0` automatic, `1` first DisplayPort, `2` second DisplayPort, `3` mirror |

`[pc] hwvga=0` is a diagnostic startup override, not a prerequisite for
automatic safe per-mode fallback. Not every debug key survives menu saving.
`devctl` enables development control; the installer removes it from defaults.

## Automatic CPU boost

The GPU menu offers **CPU Boost DOS: Glide** under Voodoo2 and **CPU Boost
Windows: DirectX/Glide/OpenGL** under the Windows accelerator. These are independent
CPU boost switches; the selected row explains their effect. The CPU menu's base **Speed**
setting stays unchanged. Neither requires a booster executable, AUTOEXEC
command, Windows startup program or fixed C: path.

The matching Velocity driver marks actual Direct3D, Glide and OpenGL render
submissions. The firmware boosts only after accepting that work. Windows
startup, VBE activation, capability queries and ordinary GDI fills do not
activate it. Three seconds without further marked work restore base speed;
a paused game that keeps drawing remains boosted. Text-mode transitions and
guest reset revoke activity. An older driver without marking leaves Windows
at base speed. Existing DOS Voodoo2 boost remains independent.

The NT5 WGPU driver uses the same Windows boost for accelerated Direct3D.
DirectDraw copies, fills and the bridge's fixed test patterns leave the
processor at its configured base speed when no application 3D is active.
The NT5 OpenGL development build routes hardware draws through that same
Direct3D HAL and inherits its boost behavior.
[NT5 OpenGL build](../drivers/nt/README.md#opengl-development-build)

Voodoo2 memory offers **8 or 12 MB**. **Windows 3D memory** independently
selects the WGPU workspace: **32, 64 or 128 MB**. The separate **Windows 3D
accelerator** switch enables or disables the channel, retaining its memory
selection. This workspace is allocated from host RAM; it is separate from guest
RAM and VGA framebuffer memory. It does not change the VGA VRAM reported to
Windows. Resizing takes effect after restarting the monitor, not while a game
is running. The default remains 64 MB; changing capacity is not a promise of
higher FPS.

The persisted keys are `[pc] wgpu` (workspace MB) and `wgpu_enabled` (on/off).
Older configurations with `wgpu=0` still disable the channel.

Fresh firmware and the installer both enable Voodoo2 (12 MB), the Windows
accelerator (64 MB), and both CPU Boost switches by default. Missing keys use
these defaults; an explicit saved OFF value is preserved during an upgrade.

VBE activation alone does not establish that an application is rendering
3D. A fixed boot delay and a separate Windows helper are not used.

## Guest devices

For SB16 use base 220h, IRQ 5, DMA 1 (8-bit) and DMA 5 (16-bit). MPU-401 is
at 330h. Do not select 8-bit DMA 1 for 16-bit PCM just because an 8-bit test works.
NE2000 ISA PnP advertises 300h–31Fh, IRQ 3. Guest Windows detects this model,
not the host Ethernet adapter. Install [Velocity9x](../drivers/windows9x/velocity9x/README.md)
for high-colour Windows modes and CuteMouse for DOS mouse applications.

Master **Volume** affects display audio and analog sound. **Headphones** adjusts
only the analog jack through the codec volume stage. See [headphone routing](headphone-volume.md).
