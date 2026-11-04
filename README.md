<p align="center">
  <img src="docs/img/logo.png" alt="wyseDOS" width="520">
</p>

<p align="center">
  <b>Almost bare-metal x86 on the Dell Wyse 3040 - a 386/486/Pentium PC monitor and emulator that runs directly on UEFI</b>
</p>

---

**wyseDOS** turns a Dell Wyse 3040 thin client into a dedicated DOS and Windows 9x
machine. There is no Linux kernel or host operating system underneath: the UEFI
firmware loads `BOOTX64.EFI`, which calls `ExitBootServices`, sets up its own
page tables, GDT/IDT and APIC timer, drives the Cherry Trail hardware directly,
and boots the guest PC into MS-DOS, FreeDOS or Windows 95/98.

The emulator core and peripheral set come from
[X86Pi](https://github.com/AidenShaw2020/x86pi) (itself built on the PC
peripherals of [tiny386](https://github.com/hchunhui/tiny386)), adapted for
x86-64 bare metal on the Intel Atom x5-Z8350 with both hardware-assisted
virtualisation (Intel VT-x unrestricted guest) and cycle-paced software
emulation.

## Game showcase

Original DOS games running on a **Dell Wyse 3040**, booted from eMMC with
**16 MB guest RAM**. All four clips have **720p60 video and captured game
audio**, at the original game speed.

| Game | Game resolution | Performance measurement |
|---|---|---|
| **Doom** | 320×200, high detail | **64.50 FPS** — separate unlocked `demo3` timedemo, sound off |
| **Quake** | 320×200 → 640×480 | **62.4 → 16.6 FPS** — separate `demo1` timedemos, sound off |
| **Duke Nukem 3D** | 320×200 | In-game **DNRATE** counter in the clip's final, separately marked scene |
| **Tyrian 2000** | 320×200 | **31.2 FPS** — separate early-level debug sample, normal speed / low detail |

Game FPS and capture FPS are different measurements. The footage has sound
enabled; Doom and Quake benchmark results come from separate no-sound runs.
Duke3D's counter is scene-dependent, and Tyrian's sample is not an unlocked
benchmark or the recorded demo's average.

<details open>
<summary><strong>Doom</strong> · 18 seconds · high-detail demo with sound</summary>

https://github.com/user-attachments/assets/93d561f5-158c-41cc-b80b-a2e4b9697752

</details>

<details>
<summary><strong>Quake</strong> · 30 seconds · 320×200 and 640×480 side-by-side in time</summary>

The resolution changes after 15 seconds. Both segments use the software
renderer with sound effects enabled.

https://github.com/user-attachments/assets/adaf09d4-b03b-4b69-a57e-363d88114b64

</details>

<details>
<summary><strong>Duke Nukem 3D</strong> · 25 seconds · action demo and live FPS counter</summary>

An 18-second built-in demo is followed by a clearly marked 7-second rooftop
scene showing the game's own FPS counter. Its values apply to that scene.

https://github.com/user-attachments/assets/7ca1a682-eaba-4a30-b432-ed5d3b593327

</details>

<details>
<summary><strong>Tyrian 2000</strong> · 25 seconds · built-in demo with music and effects</summary>

https://github.com/user-attachments/assets/5645a36b-19c8-43b2-8efc-84873c90fd31

</details>

## What is inside

| Component | Emulated hardware |
|---|---|
| **Processor** | 386 / 486 / Pentium with optional x87 FPU; Intel VT-x execution with configurable instruction-rate limits or unrestricted speed. MHz profiles approximate each generation; they are not cycle-accurate CPU models. |
| **Memory** | Configurable conventional + extended RAM (default 16 MB, up to 256 MB), LIM EMS 4.0 at `0xD000` |
| **Video** | IBM VGA (text, CGA, EGA, Mode X, 256-colour) and VESA VBE 2.0 LFB modes up to 1280x1024 in 8/15/16/24/32 bpp with 8 MB VRAM |
| **Sound** | AdLib / Dual OPL2 / OPL3 ([Nuked OPL3](https://github.com/nukeykt/Nuked-OPL3)), Sound Blaster 16 (DSP 4.05, 8-bit and 16-bit DMA, mixer, auto-init), MPU-401 General MIDI ([TinySoundFont](https://github.com/schellingb/TinySoundFont)), Tandy 3-voice (SN76489), Covox Speech Thing & Disney Sound Source on LPT1, PC speaker (PIT channel 2) |
| **Disks** | Dual IDE/ATA channels (hard disks and ATAPI CD-ROMs, either backed by the native eMMC partition or `.img`/`.iso` files), two 3.5" floppy drives (`.img`/`.ima` or a physical USB floppy drive) |
| **Network** | NE2000 ISA / ISA PnP Ethernet adapter bridged to the USB/on-board Ethernet |
| **Input** | i8042 AT/PS/2 keyboard and 3-button PS/2 Intellimouse with scroll wheel, dual analogue gameport at `0x201` |
| **Clock** | MC146818 CMOS RTC fed from the hardware RTC |

Timing notes cover [display-derived audio clocks](docs/audio-clock.md) and
[Sound Blaster DMA synchronization](docs/sound-dma.md), including hardware
validation and regression tests.
The [native VGA page-flip notes](docs/vga-page-flips.md) explain frame lifetime,
fine panning and the regression test for incomplete moving sprites.

## How the Wyse 3040 is used

The Dell Wyse 3040 has a quad-core Intel Atom x5-Z8350 (Cherry Trail, 1.44 GHz /
1.92 GHz burst), 2 GB DDR3L RAM, 8 GB on-board eMMC, DisplayPort outputs, four
USB ports (one USB 3.0, three USB 2.0), Gigabit Ethernet, and a Nuvoton NAU88L24
headphone/headset codec attached to the Intel SST audio DSP.

| Part | Role |
|---|---|
| **Core 0** | Runs the guest CPU (Intel VT-x VMX non-root mode with EPT and I/O + MMIO trapping, or the software interpreter), hardware I/O emulation, storage and USB polling |
| **Core 1** | Runs the cycle-accurate Nuked OPL3 FM synthesiser |
| **Core 2** | Runs the TinySoundFont General MIDI synthesiser (up to 48 simultaneous voices) |
| **Core 3** | Mixes OPL3, General MIDI, Sound Blaster 16 PCM, PC speaker, Tandy and Covox/DSS, and feeds the Intel SST / I2S2 DMA ring (`nau8824` headphone jack + DisplayPort audio) |
| **Display** | Native Intel Gen8 (Cherryview) display pipe (`chvdisp.c`) with hardware cursor plane and nearest/crisp scaling to the monitor's native EDID resolution (1080p, 1440p, 4K, ultrawide) |
| **Storage** | Bare-metal SDHCI eMMC driver (`sdhci.c`) with ADMA2/PIO and native partition pass-through (`host:emmc`) |
| **USB** | Bare-metal xHCI host controller (`usbhid.c`, `hostblk.c`) for keyboards, high-rate mice, gamepads, USB flash drives and USB floppy drives |

## Installing on the Wyse 3040

1. Download **`wysedos-installer.img`** from
   [Releases](https://github.com/AidenShaw2020/wysedos/releases) (or build it
   from source with `./tools/installer/build.sh`).
2. Write `wysedos-installer.img` to a USB flash drive in raw / DD mode (using
   Rufus in DD mode, balenaEtcher or `dd`).
3. Plug the USB stick into the Wyse 3040, power it on, press **F12** at the Dell
   splash screen and select the USB stick in the UEFI boot menu.
4. Confirm the installation prompt. The installer partitions the internal 8 GB
   eMMC with a dual-view hybrid MBR/GPT layout:
   - **System partition (`WYSEDOS`, 128 MB FAT32, hidden `0x1C` in MBR)** —
     stores `\EFI\BOOT\BOOTX64.EFI` and `\386\` (`bios.bin`, `vgabios.bin`,
     `config.ini`, `gm.sf2`, `logo.bin`), and registers the `wyseDOS` UEFI boot
     entry in NVRAM.
   - **Guest disk (`C:`, ~7.1 GB FAT32 LBA `0x0C` in MBR)** — exposed directly
     to the guest PC as the primary master IDE hard disk (`hda=host:emmc`), with
     the system partition hidden outside the guest disk bounds so DOS `FDISK`,
     `FORMAT` and Windows 98 Setup see a clean single-disk PC.
5. Remove the USB stick when prompted (`wyseDOS installation complete`) and press
   any key to reboot into **wyseDOS**.

## Installing DOS or Windows 9x

Because the guest `C:` drive lives on the internal eMMC (`hda=host:emmc`), you
can install an operating system onto it just as on a real PC:

- **Physical USB floppy or CD/USB stick**: plug a USB floppy drive or FAT32 USB
  stick holding `.img` / `.iso` installation media into the Wyse 3040.
- **Disk Manager (`Win + F12`)**: open the on-screen disk manager at any time to
  mount floppy images (`.img` / `.ima`) in `A:` / `B:`, CD-ROM images (`.iso`)
  on the secondary IDE channel, or pick a boot device and restart.
- **USB flash drives and external hard disks (experimental)**: with
  `usb_storage=1` (the default), unassigned USB disks attach automatically to
  the guest's virtual PIIX3 UHCI controller. Disk Manager shows their ownership
  and lets you browse image files in arbitrary directories on FAT12/16/32
  external media. Before taking a disk in Windows, close its files and use
  the system's Stop/Eject dialog. Confirm the takeover in Disk Manager:
  the entire source disk disappears from guest USB while any of its images
  are mounted. After the last image is ejected, it is flushed and returned
  automatically to USB. Image paths on external media are temporary and must
  be selected again after a monitor restart. Guest C:/eMMC is not browsable.
  A compatible, separately licensed guest USB mass-storage driver is still
  required in Windows. The installer supplies the real-DOS USB drivers in
  `C:\DRIVERS`; it does not supply Windows USB storage packages.
  Physical floppy and CD-ROM handling is unchanged.
- **Windows 95 / 98 display driver**: `drivers/windows9x/vmdisp9x/` includes
  the [VMDisp9x](https://github.com/JHRobotics/vmdisp9x) 2D VBE display driver
  floppy image (`.ima`) and archive for the emulated Bochs VBE PCI adapter
  (`1234:1111`), enabling high-colour and True Color VBE desktop modes in
  Windows 9x.

## Hotkeys

The **Left Windows** key is the emulator's host key and is not passed through to
DOS:

| Key | Action |
|---|---|
| `Win + F12` | Open the **Disk Manager** (mount/eject floppy, HDD and CD-ROM images, attach physical USB floppy, select one-shot boot drive) |
| `Win + F11` | Open the **Settings** menu (CPU speed, RAM, audio cards, per-channel volumes, mouse speed, power behaviour) |
| `Ctrl + F1` | Cycle CPU speed preset (`Native`, `386SX ~16 MHz`, `386DX ~33 MHz`, `486DX ~66 MHz`, `Pentium ~100 MHz`) |
| `Ctrl + F2` | Cycle Sound Blaster / AdLib mode |
| `Ctrl + F3` / `F4` | Fine-adjust emulated CPU speed down / up |
| `Ctrl + F9` | Mute / unmute audio |
| `Ctrl + F10` / `F11` | Master volume down / up |
| `Ctrl + F12` | Reset the guest PC |

## Configuration (`386/config.ini`)

Both the Settings menu (`Win+F11`) and the Disk Manager (`Win+F12`) save their
changes back to `\386\config.ini` on the `WYSEDOS` system partition:

```ini
[pc]
mem=16M
vga_mem=8192K
cpu=5
fpu=1
bios=bios.bin
vga_bios=vgabios.bin
hda=host:emmc
timezone=cet
firmware_timer_us=5000

[X86Pi]
volume=12
vol_pcspeaker=16
vol_adlib=16
vol_soundblaster=16
vol_midi=16
vol_tandy=16
vol_covox=16
vol_headphones=16
pcspeaker=1
adlib=1
soundblaster=1
mpu401=1
tandy=0
covox=0
dss=0
mouse=1
mouse_invert_y=0
mouse_speed=100
joystick=0
power_restore=1
power_button=0
```

`firmware_timer_us` defaults to 5000 (5 ms). It shortens a slower firmware
hardware timer so USB mouse completions are delivered more regularly; an
already faster timer is preserved. Set it to `0` to retain the firmware's
original timing. See [input timing and diagnostics](docs/input-timing.md).

## Building from source

Building requires **Clang** and **lld-link** with the `x86_64-pc-win32-coff`
target (standard LLVM installation on Windows or Linux) and **Python 3**.

### Build the UEFI monitor (`bootx64.efi`)

```sh
./efi/build.sh
```

This compiles the UEFI entry point, Cherry Trail platform drivers, FatFs, TinySoundFont,
Nuked OPL3 and the PC emulator into `efi/build/bootx64.efi`.

### Build the bootable USB installer (`wysedos-installer.img`)

```sh
./tools/installer/build.sh
```

This builds `efi/build/bootx64.efi` and packs it together with `disks/386/` into
a bootable FAT32 USB image at `tools/installer/out/wysedos-installer.img`.

The installer builder also requires Python 3. It checks pinned upstream
archive hashes and adds an unpacked `DRIVERS/` payload. After creating the
guest FAT32 partition, the installer copies VMDisp9x, Bret Johnson's DOS USB
drivers and CuteMouse 2.1b4 (including sources) to `C:\DRIVERS`. These drivers
retain their own licences; see the bundled instructions.

NUSB FE, NUSB SE and the SanDisk-based Win95 package are excluded because
redistribution permissions for their proprietary components have not been
established. This repository supplies no download links or downloader for
them. Windows USB storage requires a separately supplied compatible driver.

## Repository layout

| Directory | Contents |
|---|---|
| `efi/` | UEFI bootstrap, page tables, SMP core bring-up, Intel VT-x hypervisor (`cpuvmx.c`), Cherry Trail display (`chvdisp.c`), SST/NAU8824 audio (`audio.c`), SDHCI eMMC (`sdhci.c`), xHCI USB (`usbhid.c`, `hostblk.c`), installer (`installer.c`) |
| `pc/` | Shared PC emulator and peripherals: i386/486/Pentium core and FPU, VGA/VBE, IDE/ATAPI, floppy, i8254 PIT, i8259 PIC, i8257 DMA, i8042 PS/2 keyboard & mouse, SB16, AdLib/Nuked OPL3, MPU-401/TinySoundFont, SN76489, Covox/DSS, NE2000, PCI, ACPI, OSD menus |
| `disks/386/` | Runtime assets installed to `\386\`: SeaBIOS (`bios.bin`), SeaVGABios (`vgabios.bin`), General MIDI soundfont (`gm.sf2`), boot logo (`logo.bin`), guest MBR template (`guest_mbr.bin`) and default `config.ini` |
| `drivers/windows9x/` | [VMDisp9x](https://github.com/JHRobotics/vmdisp9x) 2D VBE display driver (`1234:1111`) for Windows 95/98/Me |
| `tools/` | USB installer builder (`tools/installer/`), ACPI table compiler (`tools/acpi/`), VGA BIOS EDID patcher (`tools/vgabios/`), boot logo converter (`tools/logo/`) and unit tests (`tools/tests/`) |

## Acknowledgements and licence

- **[tiny386](https://github.com/hchunhui/tiny386)** by `./main.c` (`hchunhui`) —
  the 386/486 core and PC peripheral models on which [X86Pi](https://github.com/AidenShaw2020/x86pi) and **wyseDOS** are built.
- **[Nuked OPL3](https://github.com/nukeykt/Nuked-OPL3)** by Alexey Khokholov (`Nuke.YKT`) — cycle-accurate Yamaha YMF262 (OPL3) emulator.
- **[TinySoundFont](https://github.com/schellingb/TinySoundFont)** by Bernhard Schelling — SoundFont2 synthesiser.
- **[FatFs](http://elm-chan.org/fsw/ff/)** by ChaN — FAT/exFAT filesystem module.
- **[SeaBIOS / SeaVGABIOS](https://www.seabios.org/)** by Kevin O'Connor,
  Julian Pidancet, the LGPL VGABios developers Team and other contributors —
  the x86 BIOS and VGA BIOS. Complete **LGPLv3 and GPLv3** licence texts and
  notices accompany the ROMs in the installer. See the [licence texts,
  ROM hashes and corresponding-source status](LICENSES/seabios.md).
- **GeneralUser GS** (`disks/386/gm.sf2`) by S. Christian Collins — see [`disks/386/gm.sf2.LICENSE.txt`](disks/386/gm.sf2.LICENSE.txt).
- **[VMDisp9x](https://github.com/JHRobotics/vmdisp9x)** by JHRobotics — the
  bundled Windows 95/98/Me graphics driver, under the **MIT licence**. The
  original licence is included in the archive; see [package provenance](drivers/windows9x/vmdisp9x/README.md).
- **[CuteMouse](https://cutemouse.sourceforge.net/)** by Nagy Daniel, with
  maintenance and contributions by Eric Auer and other contributors — DOS
  mouse driver under **GPL-2.0-or-later**. The [upstream archive](drivers/dos/ctmouse.zip)
  includes sources and `COPYING`; the installer also unpacks those sources.
  Included ASMLIB sources by Arkady Belousov retain their **LGPL-2.1-or-later** notices.
- **[DOS USB drivers](https://bretjohnson.us/)** by Bret E. Johnson, with
  Richard Bonner credited for contributions — copyrighted **freeware under
  its own terms**, not the project GPL. The complete redistribution conditions
  are in `USBINTRO.DOC` inside the preserved [upstream package](drivers/dos/usbdos/README.md).
- **Excluded packages:** NUSB by Maximus Decim (FE adaptation credited to
  Q@PassingBy), and Nathan Lineback's SanDisk-based Win95 USB package. Their
  names are recorded for historical acknowledgement only. Their proprietary
  files and download links are not distributed with this project.

The wyseDOS code is released under the **GNU General Public License v3.0** —
see [`LICENSE`](LICENSE) and [`LICENSES/`](LICENSES/). Bundled third-party guest
drivers retain their own terms. Excluded Windows USB packages are not part
of the GPL distribution; their redistribution permissions remain unverified.
