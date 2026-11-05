"""Compile the actual byte-safe MMIO helpers and execute them in Unicorn.

python tools/tests/hwvga_access.py
Requires LLVM and Unicorn. Enforces hardware transaction widths, not just RAM
results (ordinary RAM would hide the Cherry Trail unaligned-access fault).
"""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.x86_const import *

root = Path(__file__).resolve().parents[2]
llvm = Path(os.environ.get('LLVM', 'C:/Program Files/LLVM/bin'))
work = root / 'work'
work.mkdir(exist_ok=True)
source = '''#include "hwvga_access.h"
unsigned r16(void *p) { return hwvga_load_bytes((uintptr_t)p,2); }
unsigned r32(void *p) { return hwvga_load_bytes((uintptr_t)p,4); }
void w16(void *p,unsigned v) { hwvga_store_bytes((uintptr_t)p,v,2); }
void w32(void *p,unsigned v) { hwvga_store_bytes((uintptr_t)p,v,4); }
unsigned old_r16(void *p) { return *(volatile uint16_t *)p; }
unsigned old_r32(void *p) { return *(volatile uint32_t *)p; }
void old_w16(void *p,unsigned v) { *(volatile uint16_t *)p=(uint16_t)v; }
void old_w32(void *p,unsigned v) { *(volatile uint32_t *)p=v; }
'''
with tempfile.TemporaryDirectory(dir=work) as tmp:
    tmp = Path(tmp)
    cfile, obj, binary = (tmp / n for n in ('access.c', 'access.o', 'access.bin'))
    cfile.write_text(source, encoding='utf-8')
    subprocess.run([str(llvm/'clang.exe'), '--target=i386-unknown-elf',
                    '-ffreestanding', '-O2', '-I'+str(root/'efi/platform'),
                    '-c', str(cfile), '-o', str(obj)], check=True)
    subprocess.run([str(llvm/'llvm-objcopy.exe'), '-O', 'binary',
                    '--only-section=.text', str(obj), str(binary)], check=True)
    symbols = subprocess.check_output([str(llvm/'llvm-nm.exe'), '--defined-only',
                                       '--format=posix', str(obj)], text=True)
    entries = {x[0]: int(x[2],16) for line in symbols.splitlines()
               if len(x := line.split()) >= 3 and x[1] == 'T'}
    code = binary.read_bytes()

def run(name, offset, width):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(0xa0000, 0x1000)
    uc.mem_map(0x100000, 0x100000)
    uc.mem_write(0x100000, code)
    before = bytes(range(0x20,0x30))
    uc.mem_write(0xa0000, before)
    uc.reg_write(UC_X86_REG_ESP, 0x1f0000)
    uc.mem_write(0x1f0000, struct.pack('<III',0x120000,0xa0000+offset,0x88776655))
    transactions = []

    def access(uc, kind, address, size, value, user):
        if 0xa0000 <= address < 0xa1000:
            transactions.append((address, size))

    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, access)
    uc.emu_start(0x100000+entries[name], 0x120000, count=100)
    assert uc.reg_read(UC_X86_REG_EIP) == 0x120000
    if name.startswith('old_'):
        assert any(size != 1 for address,size in transactions)
        return
    assert transactions == [(0xa0000+offset+i,1) for i in range(width)], transactions
    if name.startswith('r'):
        assert uc.reg_read(UC_X86_REG_EAX) == int.from_bytes(before[offset:offset+width],'little')
        assert bytes(uc.mem_read(0xa0000,16)) == before
    else:
        expected = bytearray(before)
        expected[offset:offset+width] = struct.pack('<I',0x88776655)[:width]
        assert bytes(uc.mem_read(0xa0000,16)) == expected

for width in (2,4):
    for offset in range(8):
        for kind in ('r','w'):
            run(f'{kind}{width*8}',offset,width)
            run(f'old_{kind}{width*8}',offset,width)
print('PASS: actual compiled MMIO helpers use only byte transactions at all word/dword alignments; values and neighbors preserved; old wide-access controls rejected')
