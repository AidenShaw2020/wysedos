# USB storage validation, 2026-11-03

Historical hardware qualification included separately supplied Windows drivers.
The current distributable contains no NUSB FE/SE or Win95/SanDisk package,
download links or automatic downloader. Those private test results do not
imply redistribution permission.

Test rig: Dell Wyse 3040, PXE monitor, `hda=host:emmc`, 16 MiB guest RAM,
Windows 98 First Edition 4.10.1998, physical 16 GB FAT32 USB flash drive.
The initial USB qualification used PXE without changing the persistent monitor
or formatting the guest disk. A later explicitly requested clean installation
is recorded below.

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
- Installer image built; all 134 unpacked payload files extracted from the
  image matched the generated SHA-256 manifest. Installer copies DRIVERS to
  volume 3 at `guest.first + 2048`, after preparing its FAT32 filesystem.
  Required driver files are checked before the erase confirmation.
- Official CuteMouse 2.1b4 executable matches the already used binary:
  SHA-256 `822cf550c9e19a22785722d2306aa08ede10ff20bfe931d5a81a15f77c5f363e`.

## Limits and follow-up

- Abrupt physical unplug with an open file produced a removed-volume warning
  and loss of monitor control. A physical restart was required. This path is
  not qualified; use Windows' system Stop/Eject workflow before removal.
- SE and Win95 OSR2.1 packages were not tested on those operating systems.
  They are excluded from the current distribution.
- DOS reconnect with the TSRs loaded needs additional hardware qualification.
  Loading and unloading scripts are supplied; they do not edit AUTOEXEC.BAT.
- FDD/CD/HID backend paths remain separate. Keyboard/mouse and the existing
  CD drive were present in the Windows tests. Physical USB FDD boot/read/write
  and loading the DOS USB drivers were qualified in the later test below.
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
  implemented lifecycle paths passed local tests.
- Our test files WYIMGA.IMG and VALID.IMG are old invalid fixtures; DOSVAL2.IMG
  is the working image containing the Windows-written WINWRITE test line.

Final artifact check: installer image 120586240 bytes; all 134 unpacked driver
payload hashes match SHA256.TXT and the extracted EFI equals the final build.
The companion wysedos-drivers.zip carries the same DRIVERS tree.

## Clean installer and physical FDD qualification, 2026-11-03

- A physical 1.44 MB USB floppy booted DOS. The original boot options remained
  available after adding DOS USB support locally to the user's diskette.
  No copy of the diskette or its boot-menu files is published in this repository.
- USBUHCIL and USBDRIVE loaded in low memory from the physical diskette, and DOS
  assigned the 16 GB flash drive D:. Its FAT32 root directory was read successfully.
- At the user's explicit request, the graphical installer recreated the internal
  7456 MiB eMMC. All six stages completed, including the final storage flush.
  The private guest container starts at physical LBA 264192 and spans 15003648
  sectors; its FAT32 C: starts 2048 sectors inside the container. The protected
  firmware partition remains outside the guest's addressable disk.
- The installed monitor then started from eMMC and booted the physical floppy.
  C: is prepared for a new operating-system installation, with unpacked DRIVERS.
  Windows 98 SE installation and later restoration of games remain user follow-up.

## Distribution cleanup

The current driver bundle contains 134 payload files: VGA, DOS USB, CuteMouse,
their documentation/sources and an exclusion notice. All extracted installer
payload hashes and the EFI binary match the current build. NUSB FE/SE and
Win95/SanDisk files and their download links are not distributed. This cleanup
was verified by build/extraction and binary-hash checks; it did not reinstall
or modify the user's already prepared eMMC or boot diskette.
