"""Execute the actual patched 16-bit INT 10h handlers (requires Unicorn).

python tools/vgabios/patch.py work/vgabios-dpms.bin
python tools/tests/vgabios_dpms.py work/vgabios-dpms.bin
"""
import pathlib
import struct
import sys
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INSN
from unicorn.x86_const import *

rom = pathlib.Path(sys.argv[1]).read_bytes()
assert len(rom) == rom[2] * 512 and sum(rom) % 256 == 0
for entry in (0x9a00, 0x9a08):
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 0x100000)
    uc.mem_write(0xc0000, rom)
    uc.mem_write(0x4b9, b'\x40')
    state = {'index': 7, 'power': 0, 'writes': 0}

    def inp(uc, port, size, user):
        assert port == 0x1ce and size == 2
        return state['index']

    def out(uc, port, size, value, user):
        assert size == 2
        if port == 0x1ce:
            state['index'] = value
        else:
            assert port == 0x1cf and state['index'] == 11
            state['power'] = value
            state['writes'] += 1

    uc.hook_add(UC_HOOK_INSN, inp, None, 1, 0, UC_X86_INS_IN)
    uc.hook_add(UC_HOOK_INSN, out, None, 1, 0, UC_X86_INS_OUT)

    def call(bx, ax=0x4f10):
        for reg, value in ((UC_X86_REG_EAX, 0x12340000 | ax),
                           (UC_X86_REG_EBX, 0x56780000 | bx),
                           (UC_X86_REG_ECX, 0x99887766),
                           (UC_X86_REG_EDX, 0x44332211),
                           (UC_X86_REG_ES, 0x3456),
                           (UC_X86_REG_SS, 0x2000),
                           (UC_X86_REG_ESP, 0xff00),
                           (UC_X86_REG_CS, 0xc000)):
            uc.reg_write(reg, value)
        uc.mem_write(0x2ff00, struct.pack('<HHH', 0x100, 0x1000, 0x202))
        uc.emu_start(0xc0000 + entry, 0x10100, count=300)
        assert uc.reg_read(UC_X86_REG_CS) == 0x1000
        assert uc.reg_read(UC_X86_REG_IP) == 0x100
        if ax != 0x4f0a:
            assert uc.reg_read(UC_X86_REG_ECX) == 0x99887766
            assert uc.reg_read(UC_X86_REG_ES) == 0x3456
        assert uc.reg_read(UC_X86_REG_EDX) == 0x44332211
        assert uc.reg_read(UC_X86_REG_EAX) >> 16 == 0x1234
        assert uc.reg_read(UC_X86_REG_EBX) >> 16 == 0x5678
        assert state['index'] == 7
        return uc.reg_read(UC_X86_REG_AX), uc.reg_read(UC_X86_REG_BX)

    assert call(0) == (0x4f, 0x0730)
    for power in (1, 2, 4, 0):
        assert call((power << 8) | 1) == (0x4f, (power << 8) | 1)
        assert state['power'] == power
        assert uc.mem_read(0x4b9, 1) == bytes([0x40 | power])
        assert call(2) == (0x4f, (power << 8) | 2)
    writes = state['writes']
    for bx in (0x301, 0x801, 3, 0xff01):
        assert call(bx) == (0x14f, bx)
        assert state['writes'] == writes and state['power'] == 0
    # Existing VBE protected-mode entry remains callable after the new hook.
    ax, bx = call(0, 0x4f0a)
    assert ax == 0x4f
print('PASS: both INT 10h entries; DPMS caps/set/get/errors; register/index/BDA preservation; ROM checksum')
