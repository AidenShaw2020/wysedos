wyseDOS driver bundle
====================
The device installer copies this entire unpacked folder to C:\DRIVERS on
the newly prepared guest FAT32 partition. Drivers are not installed into
Windows automatically. Install an operating system first.

VGA      VMDisp9x 1.2025.0.119b, 2D. In Display Properties select the
         adapter driver VMDISP9X.INF. Tested on Win98 FE at 800x600.
USBDOS   Bret Johnson's DOS USB drivers, including their documentation.
         In real DOS run C:\DRIVERS\USBON.BAT. USB flash disks receive a
         free drive letter automatically, without choosing them in OSD.
         LowMemory:Yes is required by wyseDOS's bounded guest DMA.
         These drivers use the virtual UHCI controller, not physical HID.
CTMOUSE  CuteMouse 2.1 beta 4. Run C:\DRIVERS\CTMOUSE\CTMOUSE.EXE /P.
         The binary matches the version tested on this Wyse. GPL sources
         and the upstream documentation are included.
NUSBFE   Native USB 3.20 for Windows 98 First Edition, English.
NUSBSE   Maximus Decim NUSB 3.6 for Windows 98 Second Edition, English.
         Install ONLY the package for your Windows edition. These are
         complete packages and replace system components, including
         Explorer, ACPI and storage/kernel components. Localized Windows
         may become partly English. Keep your system backup beforehand.
         From Explorer open the matching folder, right-click _NUSB.INF
         and choose Install, then restart Windows. The original upstream
         readme and all unpacked files are in each folder.
         The FE package is tested on this Wyse. SE is supplied for SE
         installations; it has not been tested on the current FE system.
USB95    Nathan Lineback's generic USB storage driver for Win95 OSR2.1
         with Microsoft's USB supplement. Not for original Win95/95A.
         Included separately; not tested on a Windows 95 installation.

DOS / Windows handoff
---------------------
DOS USB TSRs and the Windows USB stack must not own UHCI simultaneously.
Before starting Windows run C:\DRIVERS\USBOFF.BAT, or start Windows using
C:\DRIVERS\WINUSB.BAT, which checks that both TSRs unloaded first.
Before unloading, change to C: and close programs using USB drives.
Do not disconnect media during writes. USB flash drives are automatic;
the OSD disk menu remains for floppy/CD/image selection.
In Windows 98, close all files on the USB drive, open the tray icon
"Unplug or Eject Hardware", select the USB drive and click Stop. Wait for
Windows to release the device before physically unplugging it. Reconnecting
then mounts the drive automatically. Pulling it while a file is open caused
a removed-volume warning and a stalled system in hardware testing.

Upstream sources and licensing
-----------------------------
VGA: https://github.com/JHRobotics/vmdisp9x/releases/tag/v1.2025.0.119
     MIT; upstream license included.
USBDOS: https://bretjohnson.us/programs/usbdos.zip
     Freeware; read the original USBINTRO.DOC for its redistribution terms.
CTMOUSE: https://ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/latest/base/ctmouse.zip
     GNU GPL v2; original documentation and sources included.
NUSB FE: external-reference-removed
NUSB SE: external-reference-removed
     Third-party packages containing Microsoft system files; not covered
     by the wyseDOS project's GPL license. Original notices are included.
     Read NUSB-LICENSE.TXT for authors, exact hashes and licensing limits.
     No blanket redistribution licence was found in the upstream readmes.
USB95: external-reference-removed
     Original third-party SanDisk driver with generic INF.

SHA256.TXT records every payload file for verification.

External image files (Win+F12)
-----------------------------
Put floppy IMG/IMA, CD ISO or hard-disk images anywhere on an external
FAT12/16/32 USB flash drive or HDD. Guest C: / internal eMMC is excluded.
Close all files and, in Windows, Stop the source USB disk using the system
tray before taking it. In DOS, close programs using the USB drive first.
Open Win+F12, select an empty FDD/CD/HDD slot, choose USB 1..4 and confirm
the ownership transfer. Browse folders, select an image, then Save and Exit.
The whole physical source disk is hidden from guest USB while images from
it are mounted. Multiple images can share that source. Eject the last one
and Save and Exit to flush and return the disk to guest USB automatically.
Cancelling the browser also returns an otherwise unused source disk.
External image selections are temporary across monitor restarts. HDD or
HDD/CD personality changes require a guest reboot; ordinary CD and floppy
media changes do not. Always wait for Save and Exit before removing media.
