"""Execute legacy INT10 output after a VDD-style standard-BDA restore.

Host Unicorn regression, not a Windows VDD runtime test. The private mode
cache intentionally retains a VBE mode, while the standard BDA is text.
The old ROM calls INT15/87 and writes the inherited graphics framebuffer.
"""
import argparse
from pathlib import Path
import struct

from vgabios_text_clear import Harness, TEXT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rom', type=Path)
    args = parser.parse_args()
    rom = args.rom.read_bytes()
    assert rom[:2] == b'\x55\xaa' and len(rom) == rom[2] * 512 and sum(rom) % 256 == 0
    failures = 0
    for entry in (0x9a00, 0x9a08):
        h = Harness(rom)
        try:
            h.execute(3, boot=True)
            h.execute(entry, values={'eax': 3})
            standard = bytes(h.uc.mem_read(0x449, 0x70))
            h.execute(entry, values={'eax': 0x4f02, 'ebx': 0xc111})
            cached = bytes(h.uc.mem_read(0x4ba, 4))
            assert struct.unpack('<H', cached[:2])[0] & 0x1ff == 0x111
            h.uc.mem_write(0x449, standard)
            h.uc.mem_write(TEXT, b'\x20\x07' * 2000)
            graphics = dict(h.dispi)
            h.phase = 'restored text teletype'
            h.execute(entry, values={'eax': 0x0e49, 'ebx': 7}, ds=0x1234)
            assert bytes(h.uc.mem_read(TEXT, 2)) == b'I\x07'
            assert struct.unpack('<H', h.uc.mem_read(0x450, 2))[0] == 1
            assert h.dispi == graphics, 'legacy output changed the physical VBE mode'
            assert h.interrupts[0x15] == 0, 'text output attempted high-memory BIOS copies'
            h.phase = 'restored text scroll'
            h.execute(entry, values={'eax': 0x0601, 'ebx': 0x0700,
                                     'ecx': 0, 'edx': 0x184f})
            assert bytes(h.uc.mem_read(TEXT, 4000)) == b'\x20\x07' * 2000
            assert h.dispi == graphics
            # An ordinary VBE context keeps its private descriptor unchanged.
            h.execute(entry, values={'eax': 0x4f02, 'ebx': 0xc111})
            current = bytes(h.uc.mem_read(0x4b9, 5))
            h.execute(entry, values={'eax': 0x0f00})
            assert bytes(h.uc.mem_read(0x4b9, 5)) == current
            print(f'PASS {entry:04x}: restored text output/scroll, no LFB copy or mode write; VBE cache preserved')
        except (AssertionError, RuntimeError) as exc:
            failures += 1
            print(f'FAIL {entry:04x}: {exc}')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
