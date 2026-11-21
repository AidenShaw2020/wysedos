#include "cpu_speed_dos.h"

.intel_syntax noprefix
.code16
/* Keep the command tail handling independent of DOS's initial stack. */
.macro skip_whitespace
.Lspace_loop\@:
    cmp byte ptr [si], ' '
    je .Lspace_next\@
    cmp byte ptr [si], 9
    jne .Lspace_done\@
.Lspace_next\@:
    inc si
    jmp .Lspace_loop\@
.Lspace_done\@:
.endm

.global _start
_start:
    cld
    mov si, 0x81
    skip_whitespace
    xor bx, bx
    cmp byte ptr [si], '/'
    je restore
    cmp byte ptr [si], '0'
    jb usage
    cmp byte ptr [si], '9'
    ja usage
digits:
    lodsb
    sub al, '0'
    cmp al, 9
    ja tail
    xor ah, ah
    mov di, ax
    mov ax, bx
    mov cx, 10
    mul cx
    add ax, di
    cmp ax, WYSE_CPU_SPEED_MAX
    ja usage
    mov bx, ax
    jmp digits
restore:
    inc si
    lodsb
    and al, 0xdf
    cmp al, 'R'
    jne usage
    mov bx, WYSE_CPU_SPEED_RESTORE
    jmp check_tail
tail:
    dec si
check_tail:
    skip_whitespace
    cmp byte ptr [si], 13
    jne usage
    mov dx, WYSE_CPU_SPEED_ID_PORT
    in ax, dx
    cmp ax, WYSE_CPU_SPEED_ID
    jne unavailable
    mov dx, WYSE_CPU_SPEED_PORT
    mov ax, bx
    out dx, ax
    mov ax, 0x4c00
    int 0x21

usage:
    mov dx, offset usage_text
    jmp error
unavailable:
    mov dx, offset unavailable_text
error:
    mov ah, 9
    int 0x21
    mov ax, 0x4c01
    int 0x21
usage_text:
    .ascii "Usage: CPUSPEED <0-2000 MHz> or CPUSPEED /R (restore saved speed)\r\n$"
unavailable_text:
    .ascii "CPUSPEED requires compatible wyseDOS firmware.\r\n$"
