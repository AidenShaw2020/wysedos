// Application processor start-up trampoline, copied to a page below 1 MB.
//
// INIT-SIPI-SIPI starts the AP in real mode at <page>:0000. It switches to
// protected mode, enables PAE and long mode with the BSP's page tables (CR3,
// CR4, EFER patched into the data block below), loads the BSP's GDT and IDT,
// far-returns into the BSP's 64-bit code selector and calls entry(arg) on its
// own stack. EBX holds the trampoline's linear base throughout.

    .att_syntax prefix
    .globl ap_tramp_start
    .globl ap_tramp_end
    .globl ap_tramp_data

    .code16
ap_tramp_start:
    cli
    cld
    xor %ebx, %ebx
    mov %cs, %bx
    shl $4, %ebx                    // linear base of this page
    mov %cs, %ax
    mov %ax, %ds
    lgdtl (tramp_gdtr - ap_tramp_start)
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmpl *(far32 - ap_tramp_start)

    .code32
pm32:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov (d_cr4 - ap_tramp_start)(%ebx), %eax
    mov %eax, %cr4
    mov (d_cr3 - ap_tramp_start)(%ebx), %eax
    mov %eax, %cr3
    mov $0xC0000080, %ecx
    mov (d_efer - ap_tramp_start)(%ebx), %eax
    xor %edx, %edx
    wrmsr
    mov (d_cr0 - ap_tramp_start)(%ebx), %eax
    mov %eax, %cr0                  // paging on: long mode active
    ljmpl *(far64 - ap_tramp_start)(%ebx)

    .code64
lm64:
    mov %ebx, %ebx                  // upper half of rbx is undefined after the switch
    mov (d_stack - ap_tramp_start)(%rbx), %rsp
    lgdt (d_gdtr - ap_tramp_start)(%rbx)
    lidt (d_idtr - ap_tramp_start)(%rbx)
    movzwq (d_cs - ap_tramp_start)(%rbx), %rax
    push %rax
    lea (lm64_cs - ap_tramp_start)(%rbx), %rax
    push %rax
    lretq
lm64_cs:
    movzwq (d_ds - ap_tramp_start)(%rbx), %rax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    fninit
    mov (d_arg - ap_tramp_start)(%rbx), %rcx
    mov (d_entry - ap_tramp_start)(%rbx), %rax
    sub $32, %rsp                   // shadow space; rsp % 16 == 8 after the call, as the ABI wants
    call *%rax
1:
    cli
    hlt
    jmp 1b

    .p2align 3
tramp_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF        // 0x08 32-bit code
    .quad 0x00CF92000000FFFF        // 0x10 data
    .quad 0x00AF9A000000FFFF        // 0x18 64-bit code
tramp_gdtr:
    .word 4 * 8 - 1
    .long 0                         // patched: base + (tramp_gdt - start)
far32:
    .long 0                         // patched: base + (pm32 - start)
    .word 0x08
far64:
    .long 0                         // patched: base + (lm64 - start)
    .word 0x18

    .p2align 3
ap_tramp_data:
d_cr3:   .quad 0
d_cr4:   .quad 0
d_efer:  .quad 0
d_cr0:   .quad 0
d_stack: .quad 0
d_entry: .quad 0
d_arg:   .quad 0
d_gdtr:  .word 0
         .quad 0
         .p2align 3
d_idtr:  .word 0
         .quad 0
         .p2align 3
d_cs:    .word 0
d_ds:    .word 0
         .p2align 3
// offsets the C side patches (relative to ap_tramp_start)
    .globl ap_tramp_off_gdtr, ap_tramp_off_gdt, ap_tramp_off_far32, ap_tramp_off_pm32
    .globl ap_tramp_off_far64, ap_tramp_off_lm64
ap_tramp_off_gdtr:  .long tramp_gdtr - ap_tramp_start
ap_tramp_off_gdt:   .long tramp_gdt - ap_tramp_start
ap_tramp_off_far32: .long far32 - ap_tramp_start
ap_tramp_off_pm32:  .long pm32 - ap_tramp_start
ap_tramp_off_far64: .long far64 - ap_tramp_start
ap_tramp_off_lm64:  .long lm64 - ap_tramp_start
ap_tramp_end:
    .code64
