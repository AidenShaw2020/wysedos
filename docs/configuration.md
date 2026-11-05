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

## CPU and memory

`[pc] cpu=3`, `4`, `5` selects 386, 486 or Pentium identity. The installer
starts with 16 MB RAM. `[wyseDOS] speed` selects approximate MHz; `0` means
unrestricted and omission uses the generation default. Choose the menu's
supported values. Identity, FPU and rate limiting are separate settings, not
cycle-accurate CPU models. Wrong audio clocks can also accelerate game timers;
see [audio-clock.md](audio-clock.md).

## Common settings

| Section/key | Meaning |
|---|---|
| `[pc] mem=16M` | Guest RAM, not physical Wyse RAM |
| `[pc] vga_mem=8192K` | VGA/VBE memory |
| `[pc] cpu=5`, `fpu=1` | CPU identity and FPU |
| `[pc] bios`, `vga_bios` | Runtime ROM filenames |
| `[pc] hda=host:emmc` | Native bounded system disk |
| `[pc] fda`, `fdb` | Floppy sources; prefer Disk Manager |
| `[pc] usb_storage=1` | Automatic guest USB disk exposure |
| `[pc] timezone=cet` | Central European RTC conversion including summer time; `utc` disables conversion |
| `[pc] firmware_timer_us=5000` | Requested firmware timer period; `0` preserves firmware timing |
| `[wyseDOS] speed` | Approximate execution-rate profile |
| `[wyseDOS] volume`, `vol_*` | Master and per-source levels, 0–16 |
| `[wyseDOS] pcspeaker`, `adlib`, `soundblaster`, `mpu401` | Audio enable flags |
| `[wyseDOS] tandy`, `covox`, `dss` | Optional audio sources |
| `[wyseDOS] mouse`, `mouse_speed`, `mouse_invert_y` | Mouse enable, percent scaling, Y inversion |
| `[wyseDOS] picture` | Layout: choose aspect/fill/integer options in Settings |

`[pc] hwvga=0` is a diagnostic startup override, not a prerequisite for
automatic safe per-mode fallback. Not every debug key survives menu saving.
`devctl` enables development control; the installer removes it from defaults.

## Guest devices

For SB16 use base 220h, IRQ 5, DMA 1 (8-bit) and DMA 5 (16-bit). MPU-401 is
at 330h. Do not select 8-bit DMA 1 for 16-bit PCM just because an 8-bit test works.
NE2000 ISA PnP advertises 300h–31Fh, IRQ 3. Guest Windows detects this model,
not the host Ethernet adapter. Install [VMDisp9x](../drivers/windows9x/vmdisp9x/README.md)
for high-colour Windows modes and CuteMouse for DOS mouse applications.

Master **Volume** affects display audio and analog sound. **Headphones** adjusts
only the analog jack through the codec volume stage. See [headphone routing](headphone-volume.md).
