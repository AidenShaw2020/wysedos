#!/bin/sh
# Build wyseDOS (bootx64.efi) with LLVM, no EDK2, and drop it where the netboot
# server serves it. Objects are rebuilt only when their source is newer.
#   build.sh [clean]
set -e
cd "$(dirname "$0")"
LLVM="${LLVM:-/c/Program Files/LLVM/bin}"
[ "$1" = clean ] && rm -rf build

COMMON="--target=x86_64-unknown-windows -std=gnu11 -O2 -ffreestanding -nostdlibinc
        -fno-builtin-printf -fno-stack-protector -fno-stack-check -mno-stack-arg-probe
        -fshort-wchar -fno-math-errno -mno-red-zone -msse4.2 -Ilibc/include -Iinclude -Isrc -Iplatform -I../pc -I../pc/voodoo/86box
        -DWYSE_BUILD=1 -DWYSE_POC_READ_ONLY=0 -DCIRCLE_BUILD=1 -DCIRCLE_DISK_WRITE=1 -DTINY386_PC=1 -DFRANK_NO_ADLIB=0
        -DNATIVE_JIT=0 -DBLOCK_JIT=0 -DTRACE_PORTS=0 -DSOUND_FREQUENCY=48000 -DCIRCLE_PC_NO_UART=1"
OWN="-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare"
MODEL="-Wno-everything"

mkdir -p build/obj
# Shared structure changes must rebuild every consumer. Previously only .c
# timestamps were checked, leaving incompatible PC layouts in old objects.
if [ ! -f build/headers.stamp ] || [ -n "$(find libc/include include src platform fatfs ../pc -type f -name '*.h' -newer build/headers.stamp -print -quit)" ]; then
    touch build/headers.stamp
fi
OBJS=""
compile() {   # src flags
    o="build/obj/$(echo "$1" | sed 's|^\.\./||; s|/|_|g; s|\.[cS]$||').obj"
    if [ ! -f "$o" ] || [ "$1" -nt "$o" ] || [ build.sh -nt "$o" ] || [ build/headers.stamp -nt "$o" ]; then
        "$LLVM/clang" $COMMON $2 -c "$1" -o "$o"
    fi
    OBJS="$OBJS $o"
}
for f in src/*.c src/*.S libc/*.c platform/*.c; do compile "$f" "$OWN"; done
# the real FatFs, its API renamed to fat_* (fatfs/rename.h) beside platform/ff.h's
for f in fatfs/*.c; do compile "$f" "-include fatfs/rename.h -Wno-everything"; done
for f in ../pc/*.c ../pc/nuked/*.c ../pc/voodoo/86box/*.c; do compile "$f" "$MODEL"; done

"$LLVM/lld-link" -subsystem:efi_application -entry:efi_main -nodefaultlib -map:build/bootx64.map -out:build/bootx64.efi $OBJS
mkdir -p ../tools/pxe/netboot
cp build/bootx64.efi ../tools/pxe/netboot/bootx64.efi
ls -l build/bootx64.efi
