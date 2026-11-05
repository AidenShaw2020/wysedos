"""Execute the ROM's relocated VBE 2.0 protected-mode display-start entry.

python tools/tests/vgabios_pm.py disks/386/vgabios.bin
Requires Unicorn. Tests the actual assembled BIOS, not a translation of it.
"""
import pathlib
import struct
import sys
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_MODE_32, UC_HOOK_INSN
from unicorn.x86_const import *

rom = pathlib.Path(sys.argv[1]).read_bytes()
assert len(rom) == rom[2] * 512 and sum(rom) % 256 == 0
# Obtain the interface exactly as a guest does, through INT 10h AX=4F0Ah.
real = Uc(UC_ARCH_X86, UC_MODE_16)
real.mem_map(0, 0x100000)
real.mem_write(0xc0000, rom)
for reg, value in ((UC_X86_REG_AX, 0x4f0a), (UC_X86_REG_BX, 0),
                   (UC_X86_REG_CS, 0xc000), (UC_X86_REG_SS, 0x2000),
                   (UC_X86_REG_SP, 0xff00)):
    real.reg_write(reg, value)
real.mem_write(0x2ff00, struct.pack('<HHH', 0x100, 0x1000, 0x202))
real.emu_start(0xc9a00, 0x10100, count=300)
assert real.reg_read(UC_X86_REG_AX) == 0x4f
table = bytes(real.mem_read(real.reg_read(UC_X86_REG_ES) * 16 +
                            real.reg_read(UC_X86_REG_DI),
                            real.reg_read(UC_X86_REG_CX)))
display = struct.unpack_from('<H', table, 2)[0]

def check(width, bpp, dwords, wait):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(0x100000, 0x100000)
    uc.mem_write(0x100000, table)  # Relocated, as required by the VBE interface.
    regs = {UC_X86_REG_EBX: 0x12340000 | wait,
            UC_X86_REG_ECX: 0x56780000 | (dwords & 0xffff),
            UC_X86_REG_EDX: 0x9abc0000 | (dwords >> 16),
            UC_X86_REG_ESI: 0x11223344, UC_X86_REG_EDI: 0x55667788,
            UC_X86_REG_EBP: 0x88776655}
    for reg, value in regs.items():
        uc.reg_write(reg, value)
    uc.reg_write(UC_X86_REG_ESP, 0x1f0000)
    uc.mem_write(0x1f0000, struct.pack('<I', 0x120000))
    state = {'index': 7, 'x': None, 'y': None, 'status_reads': 0}

    def inp(uc, port, size, user):
        if port == 0x3da:
            assert size == 1
            state['status_reads'] += 1
            return 0 if state['status_reads'] == 1 else 8
        assert port == 0x1cf and size == 2
        return {6: width, 3: bpp}[state['index']]

    def out(uc, port, size, value, user):
        assert size == 2
        if port == 0x1ce:
            state['index'] = value
        else:
            assert port == 0x1cf
            assert state['index'] in (8, 9)
            state['x' if state['index'] == 8 else 'y'] = value

    uc.hook_add(UC_HOOK_INSN, inp, None, 1, 0, UC_X86_INS_IN)
    uc.hook_add(UC_HOOK_INSN, out, None, 1, 0, UC_X86_INS_OUT)
    uc.emu_start(0x100000 + display, 0x120000, count=300)
    assert uc.reg_read(UC_X86_REG_EIP) == 0x120000
    assert uc.reg_read(UC_X86_REG_ESP) == 0x1f0004
    for reg, value in regs.items():
        assert uc.reg_read(reg) == value, (reg, hex(uc.reg_read(reg)), hex(value))
    if wait not in (0, 0x80):
        assert uc.reg_read(UC_X86_REG_AX) == 0x14f
        assert state['x'] is None and state['y'] is None
        return
    assert uc.reg_read(UC_X86_REG_AX) == 0x4f
    nbytes = (bpp + 7) // 8
    y, remainder = divmod(dwords * 4, width * nbytes)
    x = remainder // nbytes
    assert (state['x'], state['y']) == (x, y), (
        width, bpp, hex(dwords), hex(wait), state, (x, y))
    assert state['status_reads'] == (2 if wait else 0)

for width, bpp in ((640, 8), (800, 15), (800, 16), (640, 24), (800, 32)):
    for dwords in (0, 0x2c00, 0x5800, 0x12c00, 0x25800):
        for wait in (0, 0x80):
            check(width, bpp, dwords, wait)
for invalid in (1, 0x81, 0xff):
    check(640, 8, 0x12c00, invalid)
print('PASS: relocated PM display start, immediate/retrace, high/low address words, packed pixel depths, register preservation and invalid calls')
