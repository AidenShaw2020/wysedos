# Installation

[Documentation index](index.md)

## Target and release files

The qualified target is the Dell Wyse 3040 with Atom x5-Z8350, UEFI, VT-x/EPT
and internal eMMC. Other PCs are not qualified: the platform contains Wyse and
Cherry Trail assumptions. Use a USB keyboard, a DisplayPort monitor or suitable
HDMI adapter, and a USB installer stick. Firmware virtualization must be
enabled, and the boot policy must permit the unsigned EFI application.

| Release asset | Purpose |
|---|---|
| `wysedos-installer.img` | Raw bootable USB image; new installation or firmware upgrade |
| `BOOTX64.EFI` | Monitor executable; needs matching runtime assets |
| `wysedos-drivers.zip` | The same DOS, Windows 9x and NT5 driver files as the shared CD |
| `wysedos-nt5-driver-package.iso` | Manifest-checked NT5 input for rebuilding the USB installer |
| `wysedos-licenses.zip` | Licence texts and recorded notices |
| `SHA256SUMS.txt` | Checksums of release assets |

Supply your own appropriately licensed DOS/Windows and game installation media.
They are not part of the public distribution.

## Install or upgrade eMMC

1. Download the installer from the repository's Releases page and check its
   SHA-256 against the manifest.
2. Write it to a USB stick in raw/DD mode. This replaces the stick's contents;
   copying the image file onto a formatted stick is insufficient.
3. At the Dell logo press F12 and choose the stick's UEFI boot entry.
4. Choose **U — Upgrade** if available or **I — New install**. Upgrade asks
   for a second U confirmation and keeps guest data and settings. A new
   installation requires Y. **It repartitions eMMC and destroys its existing
   data.** Esc cancels.
5. Remove the stick when instructed and follow the reboot prompt.

A new installation creates a 128 MiB firmware partition and reserves the
remaining space for the guest. The same unpartitioned disk is presented to
DOS, Windows 9x, Windows 2000, and Windows XP. Each operating system creates
its own partition and file system. Supply your own installation media.

The firmware partition holds `EFI/BOOT/BOOTX64.EFI`, the `386/` runtime assets,
and `386/drivers.iso`. It is outside the disk presented to the guest. See
[storage](storage.md).

## Install DOS and Windows 9x

1. Connect a physical USB floppy or USB CD-ROM drive with your installation
   media, or put floppy images (`.img`/`.ima`) and CD images (`.iso`) on
   external FAT media. Physical USB CD-ROM support has been tested; an ISO
   image is an alternative, not a requirement.
2. Open Win+F12 and select the media and boot device. Stop/eject a disk in
   Windows before granting it to Disk Manager as an image source.
3. Boot your DOS media. Use its partitioning and formatting tools to create
   the DOS volume, then transfer the system using that DOS version's procedure.
   Choose a file system supported by your DOS version.
4. Install Windows normally from your media. Begin with standard VGA, then
   follow [Velocity9x for Windows 98](../drivers/windows9x/velocity9x/README.md).
   The driver CD supplies `VELOCITY`, with its GPL license,
   third-party notices and corresponding modified source in `SRC.ZIP`.
5. DOS mouse applications normally need CuteMouse or another PS/2 mouse driver.
   DOS USB storage needs the supplied USB drivers; Windows USB storage needs
   a separately supplied compatible driver.

Windows 98 FE and SE have been exercised. Other Windows variants do not have
equivalent complete qualification merely because the device model supports them.

## Install Windows 2000 or Windows XP

Insert your Windows CD or select its ISO in Win+F12. Boot the CD for the
first stage of Windows Setup. Select the
unpartitioned guest disk and let Setup create and format its system partition.
When Setup restarts, let it boot from the hard disk and continue with the
normal Setup prompts. Windows sees the full guest capacity. The 128 MiB
firmware partition remains outside the guest.

Use 32-bit Windows 2000 SP4 or XP SP3 for the graphics preview. Once the
desktop is ready, mount the shared driver CD. Run `NTINST.EXE --install`
then `NTINST.EXE --display` from its `NT5` directory as Administrator and
restart if requested. See [NT5 validation and limits](nt5-support.md).

If Windows keeps an older wyseDOS driver with the same Plug and Play rank,
use **Update Driver → Have Disk** for the matching wyseDOS WGPU
co-processor and display adapter. Point the wizard to the CD's `NT5`
directory and restart after the updates.

## Mount the driver CD

When booted from eMMC, open Win+F12, choose the CD-ROM slot ATA1-0, and select
`drivers.iso`. Disk Manager offers the bundled CD directly from the firmware
partition and can change the disc in that slot while the guest is running.
This works offline.

DOS requires a CD-ROM driver and MSCDEX or equivalent to assign a drive letter.
Start with `README.TXT` on the CD. In Windows 2000/XP, `NT5` contains the NT
Plug and Play packages. In DOS, copy any persistent tools you use to the DOS
volume.

## Upgrade an existing installation

Boot the same installer USB and choose **U**, then **U** again to confirm.
Upgrade is offered only for the supported native eMMC layout: a valid GPT,
non-overlapping firmware/guest partitions, matching `386/native.ini`, existing
configuration and firmware files. An older image-based layout or damaged
installation is not automatically migrated. If Upgrade is unavailable, cancel
and investigate; New install erases data.

Upgrade replaces the monitor EFI executable, BIOS/VGA ROMs, soundfont, logo, driver CD and
accompanying runtime licence/notice files. It keeps `386/config.ini`, the native
layout marker, partition tables, boot entry and the entire guest C: unchanged.
Games, Windows, AUTOEXEC.BAT, CONFIG.SYS and `C:\DRIVERS` are not overwritten.
Obtain optional guest driver updates separately from `wysedos-drivers.zip`.
Existing settings remain active; new release defaults are not applied.

The updater stages and reads back all new firmware files before activation,
keeps temporary originals and activates EFI last. A small journal permits a
retry from the installer after an interrupted update: incomplete activation is
rolled back, or completed activation has its temporary files cleaned up. Leave
power connected until completion and keep a backup of important data. This is
recovery from recorded update steps, not protection against arbitrary FAT or
hardware corruption. If the firmware no longer boots after an interruption,
boot the installer USB and select Upgrade again.

The standalone EFI asset is not a complete updater; use the installer for
matching runtime files. Local development rigs also offer `emmcupd keepconfig`,
which is separate from this public installer path.
