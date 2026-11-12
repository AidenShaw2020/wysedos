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
| `wysedos-drivers.zip` | Guest drivers and their documentation |
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
4. Choose **U — Upgrade** if a supported existing installation is detected,
   or **I — New install**. Upgrade asks for a second U confirmation and keeps
   guest data and settings. **New install repartitions eMMC and destroys its
   existing guest data**; it also requires a separate Y erase confirmation.
   Esc cancels. The graphical progress display identifies the current step.
5. Remove the stick when instructed and follow the reboot prompt.

A new installation creates a 128 MB firmware partition and a guest disk container
using the remaining space. It prepares FAT32 C: and copies `C:\DRIVERS`.
It does not supply DOS system files or install Windows.

The firmware partition holds `EFI/BOOT/BOOTX64.EFI` and `386/` runtime assets.
It is outside the disk presented to the guest. See [storage](storage.md).

## Install your operating system

1. Connect a physical USB floppy or USB CD-ROM drive with your installation
   media, or put floppy images (`.img`/`.ima`) and CD images (`.iso`) on
   external FAT media. Physical USB CD-ROM support has been tested; an ISO
   image is an alternative, not a requirement.
2. Open Win+F12 and select the media and boot device. Stop/eject a disk in
   Windows before granting it to Disk Manager as an image source.
3. Boot your DOS media. Transfer the system using that DOS version's procedure,
   for example its own `SYS C:`. Confirm that the DOS version supports FAT32.
   Formatting C: removes DRIVERS as well as other files; save it first if needed.
4. Install Windows normally from your media. Begin with standard VGA, then
   follow [Velocity9x for Windows 98](../drivers/windows9x/velocity9x/README.md).
   The latter is supplied in `C:\DRIVERS\VELOCITY`, with its GPL license,
   third-party notices and corresponding modified source in `SRC.ZIP`.
5. DOS mouse applications normally need CuteMouse or another PS/2 mouse driver.
   DOS USB storage needs the supplied USB drivers; Windows USB storage needs
   a separately supplied compatible driver.

Windows 98 FE and SE have been exercised. Other Windows variants do not have
equivalent complete qualification merely because the device model supports them.

## Upgrade an existing installation

Boot the same installer USB and choose **U**, then **U** again to confirm.
Upgrade is offered only for the supported native eMMC layout: a valid GPT,
non-overlapping firmware/guest partitions, matching `386/native.ini`, existing
configuration and firmware files. An older image-based layout or damaged
installation is not automatically migrated. If Upgrade is unavailable, cancel
and investigate; New install erases data.

Upgrade replaces the monitor EFI executable, BIOS/VGA ROMs, soundfont, logo and
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
