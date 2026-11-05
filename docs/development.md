# Building and testing

[Documentation index](index.md)

## Toolchain and builds

The checked scripts target Windows LLVM and a POSIX shell such as Git Bash.
Install Clang/lld-link, Python 3 and shell utilities (`find`, `sed`, `grep`,
`cp`). `LLVM` defaults to `/c/Program Files/LLVM/bin`; override for your setup.
The installer uses `${PYTHON:-python}`. A native Linux end-to-end installer
build is not qualified: the builder produces and runs `mkusb.exe`.

From the repository root in Git Bash:

```sh
./efi/build.sh
./tools/installer/build.sh
```

Outputs: `efi/build/bootx64.efi` and
`tools/installer/out/wysedos-installer.img`. The EFI build also stages a copy
under `tools/pxe/netboot/`; this does not create a working PXE server. The
installer rebuilds its runtime tree and prepares DRIVERS. For a distinct image:

```sh
./tools/installer/build.sh out/wysedos-installer-test.img
```

Outputs, captures, OS images and rig state are ignored by Git. Incremental
builds use source timestamps and a shared-header stamp. After restoring old
source with old timestamps, remove the repository's generated `efi/build`
directory before rebuilding, so experimental objects cannot remain newer.

## Local regression tests

Tests use actual device code with mocked clocks/media/firmware where needed.
Commands and required defines appear in file headers or engineering reports;
they do not all take identical flags. Local tests do not replace hardware runs.

PowerShell examples with Clang on PATH:

```powershell
New-Item -ItemType Directory -Force work | Out-Null
clang -O2 tools/tests/lpe_clock.c -o work/lpe_clock_test.exe
./work/lpe_clock_test.exe
clang -Ipc tools/tests/native_bounds.c -o work/native_bounds.exe
./work/native_bounds.exe
clang -O1 -Ipc tools/tests/usb_storage.c -o work/usb_storage.exe
./work/usb_storage.exe
clang -O2 -ffunction-sections -DBPP=32 -DWYSE_BUILD=1 tools/tests/vga_page_flip.c '-Wl,/opt:ref' -o work/vga_page_flip.exe
./work/vga_page_flip.exe
```

Quote `'-Wl,/opt:ref'` in PowerShell to prevent comma argument splitting.

| Area | Files under `tools/tests/` |
|---|---|
| VGA/blanking/page lifetime | `vga_planar.c`, `vga_page_flip.c` |
| Audio clocks, DMA and headphone routing | `lpe_clock.c`, `sb_dma_ownership.c`, `analog_volume.c` |
| USB/bounds/media identity | `usb_storage.c`, `hostblk.c`, `native_bounds.c` |
| Image ownership and IDE lifecycle | `image_media.c`, `ide_media.c` |
| HID/PS2/firmware events | `usbhid_recovery.c`, `ps2_reenable.c`, `firmware_events.c` |
| PnP/ROM protection/EDID | `ne2000_pnp.c`, `pam.c`, `edid.c` |
| Generated media/BIOS | `driver_floppy.py`, `vgabios_dpms.py` (check dependencies) |

## Hardware and releases

Preserve guest data/configuration, test the candidate on hardware, and check
the installer independently: extract EFI/runtime/DRIVERS, compare hashes and
check excluded packages. Hash the exact release files and verify uploaded
assets. Successful compilation alone is insufficient. ROM source provenance
has a [recorded remaining gap](../LICENSES/seabios.md).

PXE, UDP logs, HDMI capture and PicoHID form the local development rig. Ignored
`tools/rig`, `tools/pxe` and `tools/pico-hid` are not a reproducible public
remote-control toolkit. Ordinary users install with the USB image and local
keyboard/monitor; they should not be told to run missing rig utilities.

Record build hash, guest CPU/RAM, media backend, output, driver versions,
steps and results. Compare capture with direct output where possible: a
grabber's damaged MJPEG frame does not prove guest video-memory corruption.
