#!/usr/bin/env python3
"""Add the VBE 2.0 protected-mode interface (4F0Ah, vbe_pm.S) to SeaVGABIOS.

  patch.py                 seavgabios.orig.bin -> ../../disks/386/vgabios.bin

Assembles vbe_pm.S with LLVM, appends it at the end of the original ROM,
points both INT 10h entries SeaVGABIOS may install at the hooks, and fixes
the ROM and PCI data sizes and the checksum.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
LLVM = os.environ.get("LLVM", r"C:\Program Files\LLVM\bin")
BASE = 0x9a00
# where SeaVGABIOS's init stores the INT 10h vector: mov eax, entry (66 b8 imm32)
VECTOR_IMM = {0x3bd4: 0x5607, 0x3ccf: 0x563f}


def assemble():
    with tempfile.TemporaryDirectory() as tmp:
        obj, out = os.path.join(tmp, "vbe_pm.o"), os.path.join(tmp, "vbe_pm.bin")
        subprocess.run([os.path.join(LLVM, "clang"), "--target=i386-unknown-elf", "-c",
                        os.path.join(HERE, "vbe_pm.S"), "-o", obj], check=True)
        subprocess.run([os.path.join(LLVM, "llvm-objcopy"), "-O", "binary", "-j", ".text", obj, out], check=True)
        return open(out, "rb").read()


def main():
    rom = bytearray(open(os.path.join(HERE, "seavgabios.orig.bin"), "rb").read())
    assert len(rom) == BASE and rom[2] * 512 == BASE, "not the SeaVGABIOS build this was made for"
    blob = assemble()
    # hook_10, hook_10_extrastack: cmp ax, 4F0Ah; je pm_info; jmp entry
    hooks = [BASE, BASE + blob.find(b"\x3d\x0a\x4f\x74", 1)]
    for i, ((at, entry), hook) in enumerate(zip(VECTOR_IMM.items(), hooks)):
        assert rom[at:at + 2] == b"\x66\xb8" and int.from_bytes(rom[at + 2:at + 6], "little") == entry
        assert blob[hook - BASE:hook - BASE + 4] == b"\x3d\x0a\x4f\x74"
        jmp = hook - BASE + 5
        dispatch = BASE + 16 + i * 8
        assert blob[jmp] == 0xe9 and (hook + 8 + int.from_bytes(blob[jmp + 1:jmp + 3], "little")) & 0xffff == dispatch
        d = dispatch - BASE
        assert blob[d:d+4] == b"\x3d\x10\x4f\x74"
        assert blob[d+5] == 0xe9 and (dispatch + 8 + int.from_bytes(blob[d+6:d+8], "little")) & 0xffff == entry
        rom[at + 2:at + 6] = hook.to_bytes(4, "little")
    rom += blob
    rom += bytes(-(len(rom) + 1) % 512 + 1)     # room for the checksum byte
    blocks = len(rom) // 512
    rom[2] = blocks
    pcir = rom.find(b"PCIR")
    if pcir >= 0:
        rom[pcir + 0x10:pcir + 0x12] = blocks.to_bytes(2, "little")
    rom[-1] = 0
    rom[-1] = -sum(rom) & 0xff
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "disks", "386", "vgabios.bin")
    open(out, "wb").write(rom)
    print(f"{out}: {len(rom)} bytes, 4F0Ah interface {len(blob)} bytes at {BASE:#x}")


if __name__ == "__main__":
    main()
