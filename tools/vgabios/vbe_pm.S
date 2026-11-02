// VBE 2.0 protected-mode interface (INT 10h AX=4F0Ah) for SeaVGABIOS on the
// Bochs/QEMU DISPI adapter (index 1CEh, data 1CFh). SeaVGABIOS answers 4F0Ah
// with 0100h; Warcraft II (and other games built on the same VESA library)
// take VBE 2.0 to mean the interface is there, and call a null bank-switch
// pointer: the jump to 0 that DOS/4GW reported as a page fault at 260:0000000D.
//
// patch.py appends this at BASE (the end of the original ROM) and points both
// of SeaVGABIOS's INT 10h entries (with and without its own stack) at the
// hooks here; everything but 4F0Ah goes on to the original entry.

        .set BASE, 0x9a00
        .set ENTRY_10, 0x5607
        .set ENTRY_10_EXTRASTACK, 0x563f
        .set DISPI_INDEX, 0x1ce
        .set DISPI_BPP, 3
        .set DISPI_BANK, 5
        .set DISPI_VIRT_WIDTH, 6
        .set DISPI_X_OFFSET, 8
        .set DISPI_Y_OFFSET, 9

        .text
        .code16
start:
hook_10:
        cmp     $0x4f0a, %ax
        je      pm_info
        .byte   0xe9
        .word   ENTRY_10 - (BASE + (1f - start))
1:
hook_10_extrastack:
        cmp     $0x4f0a, %ax
        je      pm_info
        .byte   0xe9
        .word   ENTRY_10_EXTRASTACK - (BASE + (1f - start))
1:

// BL=0: ES:DI = the table, CX = its length (code included)
pm_info:
        test    %bl, %bl
        jnz     1f
        push    %cs
        pop     %es
        mov     $(BASE + (table - start)), %di
        mov     $(table_end - table), %cx
        mov     $0x004f, %ax
        iret
1:      mov     $0x014f, %ax
        iret

// The table and the code a 32-bit protected-mode program copies to its own
// memory and calls near: position independent, registers kept but AX.
        .code32
table:
        .word   set_window - table
        .word   set_display_start - table
        .word   set_palette - table
        .word   ports - table

// 4F05: BX = 0 (window A), DX = bank in granularity units (64 KB)
set_window:
        test    %bx, %bx
        jnz     9f
        push    %edx
        push    %edx
        mov     $DISPI_INDEX, %dx
        mov     $DISPI_BANK, %ax
        out     %ax, %dx
        pop     %eax
        inc     %dx
        out     %ax, %dx
        pop     %edx
        mov     $0x004f, %ax
        ret
9:      mov     $0x014f, %ax
        ret

// 4F07: BL = 0 or 80h (wait for the retrace), DX:CX = start in dwords
set_display_start:
        test    $0x7f, %bl
        jnz     9b
        push    %ebx
        push    %ecx
        push    %edx
        push    %esi
        push    %edi
        test    %bl, %bl
        jz      1f
        mov     $0x3da, %dx
2:      in      %dx, %al
        test    $8, %al
        jz      2b
1:      shl     $16, %edx
        movzwl  %cx, %ecx
        or      %edx, %ecx
        shl     $2, %ecx                // bytes
        mov     $DISPI_INDEX, %dx
        mov     $DISPI_VIRT_WIDTH, %ax
        out     %ax, %dx
        inc     %dx
        in      %dx, %ax
        movzwl  %ax, %edi               // pixels per line
        dec     %dx
        mov     $DISPI_BPP, %ax
        out     %ax, %dx
        inc     %dx
        in      %dx, %ax
        movzwl  %ax, %esi               // bits per pixel
        cmp     $4, %esi
        je      3f
        add     $7, %esi
        shr     $3, %esi                // bytes per pixel
        mov     %edi, %ebx
        imul    %esi, %ebx              // bytes per line
        mov     %ecx, %eax
        xor     %edx, %edx
        div     %ebx
        mov     %eax, %edi              // y
        mov     %edx, %eax
        xor     %edx, %edx
        div     %esi                    // x
        jmp     4f
3:      mov     %edi, %ebx
        shr     $1, %ebx
        mov     %ecx, %eax
        xor     %edx, %edx
        div     %ebx
        mov     %eax, %edi
        mov     %edx, %eax
        shl     $1, %eax
4:      mov     %eax, %ecx
        mov     $DISPI_INDEX, %dx
        mov     $DISPI_X_OFFSET, %ax
        out     %ax, %dx
        inc     %dx
        mov     %cx, %ax
        out     %ax, %dx
        dec     %dx
        mov     $DISPI_Y_OFFSET, %ax
        out     %ax, %dx
        inc     %dx
        mov     %di, %ax
        out     %ax, %dx
        pop     %edi
        pop     %esi
        pop     %edx
        pop     %ecx
        pop     %ebx
        mov     $0x004f, %ax
        ret

// 4F09: BL = 0 or 80h, CX entries from DX, ES:EDI = blue, green, red, 0 each
set_palette:
        test    $0x7f, %bl
        jnz     9b
        push    %ecx
        push    %edx
        push    %edi
        movzwl  %cx, %ecx
        jecxz   2f
        mov     %dl, %al
        mov     $0x3c8, %dx
        out     %al, %dx
        inc     %dx
1:      mov     %es:2(%edi), %al
        out     %al, %dx
        mov     %es:1(%edi), %al
        out     %al, %dx
        mov     %es:(%edi), %al
        out     %al, %dx
        add     $4, %edi
        loop    1b
2:      pop     %edi
        pop     %edx
        pop     %ecx
        mov     $0x004f, %ax
        ret

// I/O ports the code uses, then no memory ranges
ports:
        .word   0x1ce, 0x1cf, 0x3c8, 0x3c9, 0x3da, 0xffff
        .word   0xffff
table_end:
