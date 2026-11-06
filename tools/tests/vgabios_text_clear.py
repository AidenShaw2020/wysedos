"""Host-only execution of the distributed patched VGA ROM (requires Unicorn).

Default ROM: disks/386/vgabios.bin. No ROM or production files are modified.
The VGA aperture is deliberately simple contiguous RAM, not a planar renderer.
Boot the option ROM, set mode 3 through its installed INT 10h vector, then
clear a seeded 80x25 page with AH=06 AL=00 CX=0000 DX=184f BH=07.
Nonzero upper halves and unknown-DS candidates are synthetic, not a replay
of a complete hardware snapshot. EFLAGS=27002 includes VM/IOPL as requested;
Unicorn real-mode execution is not an EMM386/VMX environment.
"""

import argparse
from collections import Counter, deque
import hashlib
from pathlib import Path
import struct
import sys

from unicorn import (Uc, UcError, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE,
                     UC_HOOK_INSN, UC_HOOK_INTR, UC_HOOK_MEM_WRITE)
from unicorn.x86_const import (
    UC_X86_INS_IN, UC_X86_INS_OUT, UC_X86_REG_EAX, UC_X86_REG_EBX,
    UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI,
    UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EFLAGS, UC_X86_REG_CS,
    UC_X86_REG_IP, UC_X86_REG_SS, UC_X86_REG_SP, UC_X86_REG_DS,
    UC_X86_REG_ES,
)

ROM_BASE = 0xc0000
TEXT = 0xb8000
STOP = 0x10100
REGS = (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
        UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP)
NAMES = ('eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp')


class Harness:
    def __init__(self, rom):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, 0x200000)
        self.uc.mem_write(ROM_BASE, rom)
        self.uc.mem_write(STOP, b'\xf4')
        self.uc.mem_write(0x410, struct.pack('<H', 0x0021))
        self.uc.mem_write(0x413, struct.pack('<H', 640))
        # CRTC/sequencer/graphics read back exactly what the ROM programmed.
        self.index = {0x3c4: 0, 0x3ce: 0, 0x3b4: 0, 0x3d4: 0}
        self.bank = {port: bytearray(256) for port in self.index}
        self.misc = 1
        self.attr = bytearray(32)
        self.attr_index = 0
        self.attr_data = False
        self.dispi_index = 0
        self.dispi = {0: 0xb0c5, 1: 640, 2: 480, 3: 8, 4: 0,
                      5: 0, 6: 640, 7: 480, 8: 0, 9: 0, 10: 256, 11: 0}
        self.pci_address = 0
        self.reads = Counter()
        self.writes = Counter()
        self.interrupts = Counter()
        self.trace = deque(maxlen=24)
        self.text_writes = []
        self.phase = 'setup'
        self.uc.hook_add(UC_HOOK_INSN, self.inp, None, 1, 0, UC_X86_INS_IN)
        self.uc.hook_add(UC_HOOK_INSN, self.out, None, 1, 0, UC_X86_INS_OUT)
        self.uc.hook_add(UC_HOOK_INTR, self.interrupt)
        self.uc.hook_add(UC_HOOK_CODE, self.code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.memory_write,
                         begin=0xa0000, end=0xbffff)

    def code(self, uc, address, size, user):
        self.trace.append((address, bytes(uc.mem_read(address, size)).hex()))
        if not ROM_BASE <= address < ROM_BASE + 0x10000 and address != STOP:
            raise RuntimeError(f'{self.phase}: unsupported external code {address:#x}')

    def memory_write(self, uc, access, address, size, value, user):
        if self.phase.startswith('clear'):
            self.text_writes.append((address, size, value,
                                     uc.reg_read(UC_X86_REG_CS),
                                     uc.reg_read(UC_X86_REG_IP)))

    def inp(self, uc, port, size, user):
        self.reads[(port, size)] += 1
        mask = (1 << (size * 8)) - 1
        if port == 0x1ce:
            return self.dispi_index
        if port == 0x1cf:
            if self.dispi.get(4, 0) & 2:
                return {1: 2560, 2: 1600, 3: 32}.get(
                    self.dispi_index, self.dispi.get(self.dispi_index, 0))
            return self.dispi.get(self.dispi_index, 0)
        if port in self.index:
            return self.index[port]
        if port - 1 in self.index:
            base = port - 1
            return self.bank[base][self.index[base]]
        if port in (0x3da, 0x3ba):
            self.attr_data = False
            return 8 if self.reads[(port, size)] & 1 else 0
        if port == 0x3cc:
            return self.misc
        if port == 0x3c0:
            return self.attr_index | 0x20
        if port == 0x3c1:
            return self.attr[self.attr_index]
        if port in (0x3c6, 0x3c7, 0x3c8, 0x3c9):
            return 0xff if port == 0x3c6 else 0
        if 0xcfc <= port <= 0xcff:
            # Synthetic Bochs PCI display, BDF 00:00.0, 16 MiB framebuffer.
            if self.pci_address & 0x00ffff00:
                return mask
            offset = (self.pci_address & 0xfc) + port - 0xcfc
            config = bytearray(256)
            struct.pack_into('<I', config, 0, 0x11111234)
            struct.pack_into('<I', config, 8, 0x03000000)
            struct.pack_into('<I', config, 0x10, 0xe0000000)
            return int.from_bytes(config[offset:offset + size], 'little')
        raise RuntimeError(f'{self.phase}: unsupported IN {port:#x}/{size}')

    def out(self, uc, port, size, value, user):
        self.writes[(port, size)] += 1
        if port == 0x1ce:
            self.dispi_index = value
        elif port == 0x1cf:
            self.dispi[self.dispi_index] = value
        elif port in self.index:
            self.index[port] = value & 0xff
            if size == 2:
                self.bank[port][value & 0xff] = (value >> 8) & 0xff
        elif port - 1 in self.index:
            base = port - 1
            self.bank[base][self.index[base]] = value & 0xff
        elif port == 0x3c2:
            self.misc = value & 0xff
        elif port == 0x3c0:
            if self.attr_data:
                self.attr[self.attr_index] = value & 0xff
            else:
                self.attr_index = value & 0x1f
            self.attr_data = not self.attr_data
        elif port == 0xcf8:
            self.pci_address = value
        elif 0xcfc <= port <= 0xcff or port in (0x3c6, 0x3c7, 0x3c8, 0x3c9):
            pass  # PCI writes and DAC contents do not affect contiguous text.
        elif port in (0x402, 0xaa55):
            # ROM debug putc reads its port from CS:0000 (the aa55 header).
            pass
        else:
            raise RuntimeError(f'{self.phase}: unsupported OUT {port:#x}/{size}')

    def interrupt(self, uc, number, user):
        self.interrupts[number] += 1
        if number != 0x10:
            raise RuntimeError(f'{self.phase}: unsupported INT {number:#x}')
        ip, cs = struct.unpack('<HH', uc.mem_read(0x40, 4))
        sp = (uc.reg_read(UC_X86_REG_SP) - 6) & 0xffff
        uc.mem_write(uc.reg_read(UC_X86_REG_SS) * 16 + sp, struct.pack(
            '<HHH', uc.reg_read(UC_X86_REG_IP), uc.reg_read(UC_X86_REG_CS),
            uc.reg_read(UC_X86_REG_EFLAGS) & 0xffff))
        uc.reg_write(UC_X86_REG_SP, sp)
        uc.reg_write(UC_X86_REG_EFLAGS,
                     uc.reg_read(UC_X86_REG_EFLAGS) & ~0x300)
        uc.reg_write(UC_X86_REG_CS, cs)
        uc.reg_write(UC_X86_REG_IP, ip)

    def execute(self, entry, boot=False, values=None, high=False, ds=0, flags=0x202):
        uc = self.uc
        for i, reg in enumerate(REGS):
            low = (values or {}).get(NAMES[i], 0)
            upper = ((0x1234 + i * 0x1111) & 0xffff) << 16 if high else 0
            uc.reg_write(reg, upper | low)
        uc.reg_write(UC_X86_REG_DS, ds)
        uc.reg_write(UC_X86_REG_ES, 0x3456 if high else 0)
        uc.reg_write(UC_X86_REG_SS, 0x2000)
        uc.reg_write(UC_X86_REG_ESP, 0xabcdff00 if high else 0xff00)
        uc.reg_write(UC_X86_REG_EFLAGS, flags & ~0x300 if not boot else flags)
        uc.reg_write(UC_X86_REG_CS, 0xc000)
        frame = struct.pack('<HH', 0x100, 0x1000)
        if not boot:
            frame += struct.pack('<H', flags & 0xffff)
        uc.mem_write(0x2ff00, frame)
        before = self.registers()
        self.trace.clear()
        uc.emu_start(ROM_BASE + entry, STOP, count=2000000)
        if uc.reg_read(UC_X86_REG_CS) != 0x1000 or uc.reg_read(UC_X86_REG_IP) != 0x100:
            raise RuntimeError(f'{self.phase}: instruction budget exhausted/no return')
        return before, self.registers()

    def registers(self):
        regs = dict(zip(NAMES, (self.uc.reg_read(r) for r in REGS)))
        for name, reg in (('esp', UC_X86_REG_ESP), ('ds', UC_X86_REG_DS),
                          ('es', UC_X86_REG_ES), ('flags', UC_X86_REG_EFLAGS)):
            regs[name] = self.uc.reg_read(reg)
        return {key: f'{value:08x}' for key, value in regs.items()}

    def bda(self):
        read = lambda at, n: int.from_bytes(self.uc.mem_read(at, n), 'little')
        return {'mode': read(0x449, 1), 'columns': read(0x44a, 2),
                'page_size': read(0x44c, 2), 'page_start': read(0x44e, 2),
                'page': read(0x462, 1), 'crtc': read(0x463, 2),
                'rows_minus_1': read(0x484, 1), 'char_height': read(0x485, 2),
                'extended_40a8_bf': bytes(self.uc.mem_read(0x4a8, 24)).hex()}

    def diagnostics(self):
        print('phase:', self.phase, 'registers:', self.registers())
        print('BDA:', self.bda())
        print('last instructions:', list(self.trace))
        print('IN:', dict(self.reads), 'OUT:', dict(self.writes),
              'INT:', dict(self.interrupts))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rom', nargs='?', type=Path,
                        default=Path(__file__).resolve().parents[2] / 'disks/386/vgabios.bin')
    args = parser.parse_args()
    rom = args.rom.read_bytes()
    if rom[:2] != b'\x55\xaa' or len(rom) != rom[2] * 512 or sum(rom) % 256:
        parser.error('invalid option ROM signature, length or checksum')
    if rom[0x9a00:0x9a04] != b'\x3d\x0a\x4f\x74':
        parser.error('expected actual locally patched SeaVGABIOS at 9a00')
    print('ROM:', args.rom, 'bytes:', len(rom), 'SHA256:', hashlib.sha256(rom).hexdigest())
    h = Harness(rom)
    try:
        h.phase = 'boot'
        h.execute(3, boot=True)
        ip, cs = struct.unpack('<HH', h.uc.mem_read(0x40, 4))
        print(f'boot installed INT10={cs:04x}:{ip:04x}; BDA:', h.bda())
        if cs != 0xc000 or ip not in (0x9a00, 0x9a08):
            raise RuntimeError('boot did not install a patched INT10 entry')
        h.phase = 'mode3'
        h.execute(ip, values={'eax': 3})
        bda = h.bda()
        print('BIOS mode3 BDA:', bda)
        print('mode3 CRTC:', bytes(h.bank[0x3d4][:25]).hex(),
              'DISPI:', h.dispi)
        expected = {'mode': 3, 'columns': 80, 'page': 0,
                    'page_start': 0, 'rows_minus_1': 24, 'char_height': 16}
        if any(bda[key] != value for key, value in expected.items()):
            raise RuntimeError('BIOS mode3 initialization disagrees with required BDA')
        # Preserve all initialized BIOS-private state; no fabricated currentmode.
        initialized = bytes(h.uc.mem_read(0, 0x100000))
        failures = 0
        cases = [('manual-low', False, 0, 0x202)]
        cases += [(f'upper-ds{ds:04x}-flags{flags:x}', True, ds, flags)
                  for ds in (0, 0x40, 0x1234, 0xffff)
                  for flags in (0x7002, 0x27002)]
        for entry in (0x9a00, 0x9a08):
            for label, high, ds, flags in cases:
                h.uc.mem_write(0, initialized)
                # All cells and off-screen guards differ from the desired 0720.
                seed = b''.join(struct.pack('<H', 0x1e41 + i % 26) for i in range(2000))
                h.uc.mem_write(TEXT - 32, b'\xa5' * 32 + seed + b'\x5a' * 32)
                h.phase = f'clear-{entry:04x}-{label}'
                h.text_writes.clear()
                before, after = h.execute(entry, values={
                    'eax': 0x0600, 'ebx': 0x0700, 'ecx': 0,
                    'edx': 0x184f, 'esi': 0x7766, 'edi': 0x5544,
                    'ebp': 0x3322}, high=high, ds=ds, flags=flags)
                text = bytes(h.uc.mem_read(TEXT, 4000))
                counts = [sum(text[2*i:2*i+2] == b'\x20\x07'
                              for i in range(row*80, (row+1)*80)) for row in range(25)]
                guards = (bytes(h.uc.mem_read(TEXT - 32, 32)) == b'\xa5' * 32
                          and bytes(h.uc.mem_read(TEXT + 4000, 32)) == b'\x5a' * 32)
                preserved = all(before[key] == after[key] for key in (*NAMES, 'ds', 'es'))
                stack_ok = int(after['esp'], 16) == int(before['esp'], 16) + 6
                ok = counts == [80] * 25 and guards and preserved and stack_ok
                failures += not ok
                print(('PASS' if ok else 'FAIL') + f' {entry:04x} {label}:',
                      'cleared/row=', counts, 'writes=', len(h.text_writes),
                      'guards=', guards, 'regs=', preserved, 'stack=', stack_ok,
                      'flags_out=', after['flags'])
                if not ok:
                    print('before:', before, 'after:', after)
                    print('first/last writes:', h.text_writes[:4], h.text_writes[-4:])
                    h.diagnostics()
            h.uc.mem_write(0, initialized)
            saved_bda = bytes(h.uc.mem_read(0x440, 0x48))
            h.phase = 'temporary-mode93'
            h.execute(entry, values={'eax': 0x93})
            h.uc.mem_write(0x440, saved_bda)
            h.uc.mem_write(TEXT, b'\x41\x1e' * 2000)
            h.phase = f'clear-{entry:04x}-restored-bda'
            h.text_writes.clear()
            h.execute(entry, values={'eax': 0x0600, 'ebx': 0x0700,
                             'ecx': 0, 'edx': 0x184f})
            text = bytes(h.uc.mem_read(TEXT, 4000))
            cleared = sum(text[offset:offset + 2] == b'\x20\x07'
                      for offset in range(0, 4000, 2))
            ok = cleared == 2000
            failures += not ok
            print(('PASS' if ok else 'FAIL') + f' {entry:04x} restored-bda:',
                'cleared=', cleared, 'BDA=', h.bda())
        return int(bool(failures))
    except (UcError, RuntimeError) as exc:
        print('INCOMPLETE:', exc)
        h.diagnostics()
        return 2


if __name__ == '__main__':
    sys.exit(main())