/* Independent real-mode guest probe. Assemble with LLVM, link at 100h as COM.
 * Dumps the identifier/resources at DS:0200 (also usable under DOS DEBUG).
 * Does not change the NIC's active state or fixed resources. */
.intel_syntax noprefix
.code16
.global _start
_start:
    push cs
    pop ds
    push cs
    pop es
    cld
    mov dx, 0x279
    xor al, al
    out dx, al
    out dx, al
    mov si, offset key
    mov cx, 32
1:  lodsb
    out dx, al
    loop 1b
    mov ax, 0x0402 /* reset CSN, preserve boot defaults */
    call wr
    mov ax, 0x0003 /* Wake[0] */
    call wr
    mov ax, 0x8000 /* read data at 203h */
    call wr
    mov dx, 0x279
    mov al, 1
    out dx, al
    mov dx, 0x203
    mov di, 0x200
    mov bx, 9
2:  mov cx, 8
3:  in al, dx
    mov ah, al
    in al, dx
    cmp ax, 0x55aa
    sete al
    shr al, 1
    rcr byte ptr [di], 1
    loop 3b
    inc di
    dec bx
    jnz 2b
    mov ax, 0x0106 /* assign CSN 1, serial reads advanced resource pointer */
    call wr
    mov dx, 0x279
    mov al, 4
    out dx, al
    mov dx, 0x203
    mov cx, 22
4:  in al, dx
    stosb
    loop 4b
    mov ax, 0x0202 /* wait for key, preserve DOS operation */
    call wr
    mov ax, 0x4c00
    int 0x21
wr: mov dx, 0x279
    out dx, al
    mov dx, 0xa79
    mov al, ah
    out dx, al
    ret
key:
    .byte 0x6a,0xb5,0xda,0xed,0xf6,0xfb,0x7d,0xbe
    .byte 0xdf,0x6f,0x37,0x1b,0x0d,0x86,0xc3,0x61
    .byte 0xb0,0x58,0x2c,0x16,0x8b,0x45,0xa2,0xd1
    .byte 0xe8,0x74,0x3a,0x9d,0xce,0xe7,0x73,0x39
