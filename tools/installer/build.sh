#!/bin/sh
# Build the wyseDOS installation USB image.
#   build.sh [image]     default: tools/installer/out/wysedos-installer.img
#
# The image holds the monitor (EFI/BOOT/BOOTX64.EFI), install.ini, which makes
# it start as the installer, and disks/386/ - the BIOS, VGA BIOS, config.ini
# and the runtime assets. A native FAT32 guest disk is created on eMMC; no
# HDD images are installed. Write the installer image to a USB stick
# with Rufus (DD image mode), balenaEtcher or dd; everything on the stick is
# replaced.
set -e
cd "$(dirname "$0")"
LLVM="${LLVM:-/c/Program Files/LLVM/bin}"
OUT="${1:-out/wysedos-installer.img}"
mkdir -p out/obj "$(dirname "$OUT")"
"${PYTHON:-python}" prepare_drivers.py

../../efi/build.sh >/dev/null
"$LLVM/clang" -O2 -DFF_NO_WINDOWS_H -D_CRT_SECURE_NO_WARNINGS -I../../efi/fatfs -Wno-everything \
    mkusb.c ../../efi/fatfs/ff.c ../../efi/fatfs/ffunicode.c -o out/mkusb.exe

cat > out/install.ini <<'EOF'
; wyseDOS installation medium: while this file is here, the stick installs
; wyseDOS onto the Wyse 3040's eMMC (after asking) instead of running it.
EOF

# Public defaults never inherit local diagnostic media or development control.
cp runtime.ini out/config.ini

rm -rf out/runtime
mkdir -p out/runtime
for name in bios.bin vgabios.bin guest_mbr.bin logo.bin gm.sf2 gm.sf2.LICENSE.txt \
            SeaBIOS-COPYING.txt SeaBIOS-COPYING.LESSER.txt SeaBIOS-NOTICE.txt; do
    cp "../../disks/386/$name" out/runtime/
done
out/mkusb.exe "$OUT" ../../efi/build/bootx64.efi=EFI/BOOT/BOOTX64.EFI out/install.ini=install.ini out/runtime=386 out/config.ini=386/config.ini out/DRIVERS=DRIVERS
ls -l "$OUT"
