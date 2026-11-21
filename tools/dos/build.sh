#!/bin/sh
# Build a flat DOS .COM with a load address of 0100h.
set -e
cd "$(dirname "$0")"
LLVM="${LLVM:-/c/Program Files/LLVM/bin}"
mkdir -p out
"$LLVM/clang" --target=i386-unknown-linux-gnu -I../../pc -c cpuspeed.S -o out/cpuspeed.o
"$LLVM/ld.lld" -m elf_i386 --image-base=0 -Ttext=0x100 --oformat=binary out/cpuspeed.o -o out/CPUSPEED.COM
