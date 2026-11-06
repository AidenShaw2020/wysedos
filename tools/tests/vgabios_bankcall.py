"""Execute the ROM's advertised real-mode VBE WinFuncPtr (requires Unicorn)."""

import argparse
from pathlib import Path
import struct

from vgabios_text_clear import Harness


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rom', nargs='?', type=Path,
                        default=Path(__file__).resolve().parents[2] / 'disks/386/vgabios.bin')
    args = parser.parse_args()
    rom = args.rom.read_bytes()
    if rom[:2] != b'\x55\xaa' or len(rom) != rom[2] * 512 or sum(rom) % 256:
        parser.error('invalid option ROM signature, length or checksum')
    failures = 0
    for entry in (0x9a00, 0x9a08):
        harness = Harness(rom)
        harness.execute(3, boot=True)
        harness.execute(entry, values={'eax': 0x4f02, 'ebx': 0x8101})
        _, result = harness.execute(entry, values={
            'eax': 0x4f01, 'ecx': 0x101, 'edi': 0x8000})
        if int(result['eax'], 16) != 0x4f:
            raise RuntimeError('VBE mode information unavailable')
        offset, segment = struct.unpack('<HH', harness.uc.mem_read(0x800c, 4))
        if segment != 0xc000:
            raise RuntimeError('WinFuncPtr does not point into the tested ROM')
        for flags in (0x2, 0x202, 0x402, 0x602):
            for label, window, bank, status in (
                    ('set', 0, 1, 0x4f), ('get', 0x100, 0, 0x4f),
                    ('invalid-window', 1, 2, 0x14f)):
                before, after = harness.execute(offset, boot=True, flags=flags, values={
                    'eax': 0x4f05, 'ebx': window, 'edx': bank,
                    'ecx': 0x1234, 'esi': 0x5678, 'edi': 0x9abc,
                    'ebp': 0xdef0})
                expected = dict(before)
                expected['eax'] = f'{status:08x}'
                expected['esp'] = f'{int(before["esp"], 16) + 4:08x}'
                if label == 'get':
                    expected['edx'] = '00000001'
                passed = after == expected and harness.dispi[5] == 1
                print('PASS' if passed else 'FAIL', f'{entry:04x}', label,
                      f'flags={flags:04x}', 'bank=', harness.dispi[5])
                if not passed:
                    print('expected:', expected, 'actual:', after)
                    failures += 1
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())