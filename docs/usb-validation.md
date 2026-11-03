# USB storage validation, 2026-11-03

Test rig: Dell Wyse 3040, PXE monitor, `hda=host:emmc`, 16 MiB guest RAM,
Windows 98 First Edition 4.10.1998, physical 16 GB FAT32 USB flash drive.
The persistent monitor on eMMC was not updated. No guest partitions were
recreated or formatted during these tests.

## Completed

- Real UHCI/BOT/SCSI unit tests: enumeration, short transfers, CSW residue,
  READ FORMAT CAPACITIES (252-byte request / 12-byte response), sector reads
  and writes, read-only errors, DMA bounds and aborting partial writes on unplug.
- Real host backend with mocked firmware: media identity, changed MediaId,
  geometry bounds, failed writes, no retry to replacement media, reconnect,
  and stable per-port USB serial numbers.
- Full NUSB 3.20 FE installed on the existing Win98 FE system with explicit
  user approval for system component replacement. The earlier standalone INF
  is diagnostic only; using only a subset of the package was insufficient.
- Win98 FE automatically assigned E:, opened the existing FAT32 directory,
  wrote `E:\WYS31103.TXT`, and read the same content after virtual unplug/replug.
- Repeated virtual reconnect after fixing changing serial numbers no longer
  launched the new-hardware wizard.
- User confirmed successful physical unplug/replug after stopping the USB
  drive through Windows' Unplug or Eject Hardware dialog.
- Prior DOS 7.1 test with `USBUHCIL LowMemory:Yes` and `USBDRIVE LowMemory:Yes`:
  FAT32 enumeration, directory/file read, write/readback on the same flash.
- Installer image built; all 227 unpacked payload files extracted from the
  image matched the generated SHA-256 manifest. Installer copies DRIVERS to
  volume 3 at `guest.first + 2048`, after preparing its FAT32 filesystem.
  Required driver files are checked before the erase confirmation.
- Official CuteMouse 2.1b4 executable matches the already used binary:
  SHA-256 `822cf550c9e19a22785722d2306aa08ede10ff20bfe931d5a81a15f77c5f363e`.

## Limits and follow-up

- Abrupt physical unplug with an open file produced a removed-volume warning
  and loss of monitor control. A physical restart was required. This path is
  not qualified; use Windows' system Stop/Eject workflow before removal.
- SE and Win95 OSR2.1 packages are supplied separately but not tested on
  those operating systems. Never install the SE package on FE.
- DOS reconnect with the TSRs loaded needs additional hardware qualification.
  Loading and unloading scripts are supplied; they do not edit AUTOEXEC.BAT.
- FDD/CD/HID backend paths remain separate. Keyboard/mouse and the existing
  CD drive were present in the Windows tests; no physical FDD test was run.
- A fresh eMMC installation was not executed, to preserve the user's existing
  Windows and games. Validation covers build and image payload integrity.
- Temporary test files `WYS31103.TXT` and earlier `WYS31USB.TST` remain on the
  flash; `C:\TEMP\USBREAD.TXT` is the source text used for the read/write test.

## External image ownership validation, 2026-11-03

- Disk Manager browsed arbitrary directories on the existing 16 GB FAT32 flash,
  took exclusive ownership and disconnected its virtual USB port while an image
  was mounted. Installed FAT runtime exposes external sources only; the PXE
  development configuration retains its network fixture list. eMMC C: is excluded.
- Live Windows 98 FE: system Stop/Eject confirmed E: safe to remove; Disk Manager
  took the source and mounted DOSVAL2.IMG as A:. Explorer opened IMAGETST.TXT.
- Notepad appended WINWRITE and saved through its explicit close/save dialog.
  After image ejection, USB automatically re-enumerated in Windows. The image
  was mounted again, Windows was already shut down normally, and a fresh DOS
  boot read the saved WINWRITE line. This validates persistence across closing
  and reopening the image, independently of Windows' filesystem cache.
- DOS also previously copied/read back a file inside the mounted image.
- Found a destructive legacy BIOS Emulink floppy interaction during Windows
  startup: two BIOS writes replaced valid image boot sectors with CONFIG$ data.
  Wyse now uses its existing standard FDC/DMA/IRQ6 model for BIOS accesses too.
  A known-valid full image matched before startup; with the fix it remained
  unchanged through Windows startup and supported Windows read/write afterward.
- The floppy builder also had a separate fixed-width header slice bug. OEM and
  label fields now have exact widths; independent FAT12 checks cover both
  1.44 MB and 2.88 MB generated images and cross-cluster payloads.
- Real FatFs/router/drive tests cover concurrent image references, conflicting
  writable opens, failed-open rollback, invalid-size refusal, retained ownership
  on sync failure, retry after failed-mount release, and no replacement writes.
  Ejecting an empty floppy cannot fall through to ejecting native C:.
- Real IDE lifecycle tests cover old-backend flush, busy/error refusal,
  empty slots and HDD/CD personality changes without replaying old buffers.
- Physical CD/HDD image use remains additional hardware qualification; their
  implemented lifecycle paths passed local tests. No persistent eMMC monitor
  update or fresh installer run was performed.
- Our test files WYIMGA.IMG and VALID.IMG are old invalid fixtures; DOSVAL2.IMG
  is the working image containing the Windows-written WINWRITE test line.

Final artifact check: installer image 120586240 bytes; all 227 unpacked driver
payload hashes match SHA256.TXT and the extracted EFI equals the final build.
The companion wysedos-drivers.zip carries the same DRIVERS tree.
