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
| `wysedos-installer.img` | Raw bootable USB image; prepares internal eMMC |
| `BOOTX64.EFI` | Monitor executable; needs matching runtime assets |
| `wysedos-drivers.zip` | Guest drivers and their documentation |
| `wysedos-licenses.zip` | Licence texts and recorded notices |
| `SHA256SUMS.txt` | Checksums of release assets |

Supply your own appropriately licensed DOS/Windows and game installation media.
They are not part of the public distribution.

## Prepare eMMC

1. Download the installer from the repository's Releases page and check its
   SHA-256 against the manifest.
2. Write it to a USB stick in raw/DD mode. This replaces the stick's contents;
   copying the image file onto a formatted stick is insufficient.
3. At the Dell logo press F12 and choose the stick's UEFI boot entry.
4. Read the confirmation before accepting. **Installation repartitions eMMC
   and destroys its existing guest data.** The graphical progress display
   identifies the stage being processed.
5. Remove the stick when instructed and follow the reboot prompt.

The installer creates a 128 MB firmware partition and a guest disk container
using the remaining space. It prepares FAT32 C: and copies `C:\DRIVERS`.
It does not supply DOS system files or install Windows.

The firmware partition holds `EFI/BOOT/BOOTX64.EFI` and `386/` runtime assets.
It is outside the disk presented to the guest. See [storage](storage.md).

## Install your operating system

1. Connect a supported USB floppy, or put floppy images (`.img`/`.ima`) and
   CD images (`.iso`) on external FAT media.
2. Open Win+F12 and select the media and boot device. Stop/eject a disk in
   Windows before granting it to Disk Manager as an image source.
3. Boot your DOS media. Transfer the system using that DOS version's procedure,
   for example its own `SYS C:`. Confirm that the DOS version supports FAT32.
   Formatting C: removes DRIVERS as well as other files; save it first if needed.
4. Install Windows normally from your media. Begin with standard VGA, then
   follow the [VMDisp9x instructions](../drivers/windows9x/vmdisp9x/README.md).
5. DOS mouse applications normally need CuteMouse or another PS/2 mouse driver.
   DOS USB storage needs the supplied USB drivers; Windows USB storage needs
   a separately supplied compatible driver.

Windows 98 FE and SE have been exercised. Other Windows variants do not have
equivalent complete qualification merely because the device model supports them.

## Updates and clean installations

Re-running the installer is a clean installation, not a data-preserving update.
The standalone EFI is not a complete updater and needs matching ROM/runtime
assets. The development monitor offers `emmcupd keepconfig` to update the
monitor and ROMs while retaining its configuration. Its remote-control tools
belong to the local development rig and are not a public consumer updater.
Do not use the clean installer to preserve an existing C:.
