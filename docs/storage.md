# Storage and removable media

[Documentation index](index.md)

## Native internal C:

`hda=host:emmc` exposes a bounded region of internal eMMC as the primary IDE
disk. C: is FAT32 using the available guest capacity, not a fixed 2 GB image.
The firmware partition and physical partition table are outside the guest's
address range. The guest has its own disk layout within this region, so a
guest MBR write cannot reach the firmware partition through this backend.
This protects monitor boot files, not C: from guest FORMAT/FDISK or deletion.

`386/config.ini` configures the monitor. `C:\CONFIG.SYS`, `AUTOEXEC.BAT` and
`MSDOS.SYS` configure the guest. BootGUI is a guest setting.

## External disk ownership

| Use | Filesystem owner | Guest view |
|---|---|---|
| Ordinary USB storage | Guest DOS/Windows driver | USB mass-storage disk |
| Source of floppy/CD/HDD images | Monitor FatFs | Virtual floppy or IDE/ATAPI media |

With `usb_storage=1`, unassigned USB disks attach automatically to the guest
USB controller. They do not need manual ATA mounting. A guest driver is needed.

For image use, place `.img`/`.ima` floppy or `.iso` CD media in directories on
external FAT12/16/32 storage. Open Win+F12, browse the source and select the
image for the appropriate drive. Guest C:/eMMC is deliberately not browsable.
These instructions do not qualify NTFS/exFAT external image sources.

Before takeover, close files and use Windows' Stop/Eject Hardware dialog.
Confirm ownership in Disk Manager. The **entire physical source disk** then
disappears from guest USB while any image from it remains mounted. Multiple
images retain ownership until the last is ejected. A successful flush/release
returns the disk to guest USB. Do not disconnect a source with active images.

Image writes persist in the source file. Failed sync must not release ownership
as though it succeeded. Media identity checks prevent pending writes from
being redirected to a replacement disk. External image selections are temporary
and must be made again after a monitor restart.

## DOS and physical drives

- `C:\DRIVERS\USBDOS\USBON.BAT` loads USBUHCIL and USBDRIVE with the supplied
  low-memory options. Read their bundled documentation for prerequisites.
- Use USBOFF before starting Windows USB drivers, avoiding competing stacks.
- Public releases do not supply Windows USB storage driver packages.
- Physical USB floppy/CD handling is separate from ordinary flash exposure.
  A physical 1.44 MB floppy was qualified for boot/read/write.
- DOS live reconnect and abrupt removal are less qualified than orderly
  Windows Stop/Eject. Do not infer hotplug support solely from boot-time access.

See [USB validation](usb-validation.md) for exact conditions, tests and limits.
