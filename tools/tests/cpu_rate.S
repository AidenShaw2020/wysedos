// DOS instruction-rate probe: assemble i386 ELF and objcopy .text to a COM.
// Prints eight hex digits: INC/CMP/JB iterations during 18 BIOS timer ticks.
// Run from DOS; the nominal CPU profile is not a cycle-accurate model.
.text
.code16
start:
    push %ds
    xor %ax,%ax
    mov %ax,%ds
    mov 0x46c,%edx
    add $18,%edx
    xor %ecx,%ecx
1:  inc %ecx
    cmp %edx,0x46c
    jb 1b
    pop %ds
    mov %ecx,%ebx
    mov $8,%bp
2:  rol $4,%ebx
    mov %bl,%dl
    and $15,%dl
    add $48,%dl
    cmp $57,%dl
    jbe 3f
    add $7,%dl
3:  mov $2,%ah
    int $0x21
    dec %bp
    jnz 2b
    mov $0x4c00,%ax
    int $0x21
