# VMDisp9x: Windows 95/98/Me display driver

Version **1.2025.0.119b**, 2D only. The upstream ZIP archive and FAT12 floppy
image (`.ima`) are kept unmodified; the included MIT licence comes from the
upstream package.

- Project: https://github.com/JHRobotics/vmdisp9x
- Release: https://github.com/JHRobotics/vmdisp9x/releases/tag/v1.2025.0.119
- ZIP: https://github.com/JHRobotics/vmdisp9x/releases/download/v1.2025.0.119/vmdisp9x-1.2025.0.119b-driver-2d.zip
- Floppy image: https://github.com/JHRobotics/vmdisp9x/releases/download/v1.2025.0.119/vmdisp9x-1.2025.0.119b-driver-2d.img

The upstream `.img` file is named `.ima` here so the disk manager treats it as
a floppy image; its contents and checksums are unchanged (see `SHA256SUMS.txt`).

wyseDOS exposes a PCI card (**1234:1111**) with the Bochs VBE interface to the
guest. The package INF matches that PCI ID directly and installs `qemumini.drv` /
`qemumini.vxd` for the emulated card; Windows never touches the physical Intel
display hardware on the Wyse 3040.

Tested on Windows 98 FE and Windows 98 SE, including 800x600 in 32-bit colour.
The monitor's native-framebuffer refresh must remain active while Windows is
idle so software cursor updates are not held until unrelated guest activity.

## Installation in Windows 95 / 98 / Me

1. Complete the Windows installation first with the standard VGA driver.
2. Copy the `.ima` image onto an external FAT16/FAT32 USB drive, e.g. as
   `vmdisp.ima`, or use the unpacked driver in `C:\DRIVERS\VGA`.
3. Press **Win+F12** → **FDD-0**: eject any current floppy, pick `vmdisp.ima`,
   and choose **Save and Exit**.
4. In Windows, open **Display Properties → Settings → Advanced → Adapter → Change**.
5. Search for an updated driver on the floppy drives. Windows will offer
   **QEMU STD VGA PCI Adapter** from `A:\VMDISP9X.INF`.
6. Finish copying the files. Open **Win+F12**, eject the floppy and save, then
   confirm the Windows reboot prompt.
7. After rebooting, open **Display Properties → Settings**, choose the desired
   colour depth and resolution (e.g. **640×480 / 256 colours** or **800×600 /
   True Color 32-bit**), and confirm the test dialog before the timeout expires.

