// VMX entry/exit and instruction wrappers. MS x64 ABI (UEFI): args in rcx, rdx;
// rbx, rbp, rdi, rsi, r12-r15 are callee-saved.

    .text
    .intel_syntax noprefix

// int vmx_run(guest_regs_t *regs, int launched)
// Returns 0 after a VM exit (guest registers stored in *regs), 1 if VM entry failed.
    .globl vmx_run
vmx_run:
    push rbx
    push rbp
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    push rcx                    // [rsp] = regs, found again by the exit path

    mov rax, 0x6C14             // HOST_RSP
    vmwrite rax, rsp
    lea rbx, [rip + vmx_exit_point]
    mov rax, 0x6C16             // HOST_RIP
    vmwrite rax, rbx

    mov r8d, edx                // launched flag
    mov rax, [rcx + 0x00]
    mov rdx, [rcx + 0x10]
    mov rbx, [rcx + 0x18]
    mov rbp, [rcx + 0x28]
    mov rsi, [rcx + 0x30]
    mov rdi, [rcx + 0x38]
    mov r9,  [rcx + 0x48]
    mov r10, [rcx + 0x50]
    mov r11, [rcx + 0x58]
    mov r12, [rcx + 0x60]
    mov r13, [rcx + 0x68]
    mov r14, [rcx + 0x70]
    mov r15, [rcx + 0x78]
    test r8d, r8d
    mov r8,  [rcx + 0x40]       // mov does not touch flags
    mov rcx, [rcx + 0x08]
    jnz 1f
    vmlaunch
    jmp 2f
1:
    vmresume
2:
    // VM entry failed: unwind
    pop rcx
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbp
    pop rbx
    mov eax, 1
    ret

vmx_exit_point:
    // rsp is HOST_RSP: [rsp] = regs
    push rcx
    mov rcx, [rsp + 8]
    mov [rcx + 0x00], rax
    pop rax
    mov [rcx + 0x08], rax
    mov [rcx + 0x10], rdx
    mov [rcx + 0x18], rbx
    mov [rcx + 0x28], rbp
    mov [rcx + 0x30], rsi
    mov [rcx + 0x38], rdi
    mov [rcx + 0x40], r8
    mov [rcx + 0x48], r9
    mov [rcx + 0x50], r10
    mov [rcx + 0x58], r11
    mov [rcx + 0x60], r12
    mov [rcx + 0x68], r13
    mov [rcx + 0x70], r14
    mov [rcx + 0x78], r15
    pop rcx
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbp
    pop rbx
    xor eax, eax
    ret

// uint64_t vmx_vmread(uint64_t field)
    .globl vmx_vmread
vmx_vmread:
    vmread rax, rcx
    ret

// int vmx_vmwrite(uint64_t field, uint64_t value)
    .globl vmx_vmwrite
vmx_vmwrite:
    vmwrite rcx, rdx
    setbe al                    // CF or ZF = failure
    movzx eax, al
    ret

// int vmx_vmxon(uint64_t *phys)  (pointer to a variable holding the address)
    .globl vmx_vmxon
vmx_vmxon:
    vmxon [rcx]
    setbe al
    movzx eax, al
    ret

    .globl vmx_vmclear
vmx_vmclear:
    vmclear [rcx]
    setbe al
    movzx eax, al
    ret

    .globl vmx_vmptrld
vmx_vmptrld:
    vmptrld [rcx]
    setbe al
    movzx eax, al
    ret

    .globl vmx_vmxoff
vmx_vmxoff:
    vmxoff
    ret

// int vmx_invept(uint64_t type, void *desc)
    .globl vmx_invept
vmx_invept:
    invept rcx, [rdx]
    setbe al
    movzx eax, al
    ret

// 16-bit test guest for M2, copied to 0000:7C00 and run in real mode
    .globl guest_test_start
    .globl guest_test_end
    .att_syntax prefix
    .code16
guest_test_start:
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %ss
    mov $0x7C00, %sp
    mov $(0x7C00 + (msg - guest_test_start)), %si
3:
    lodsb
    or %al, %al
    jz 4f
    out %al, $0xE9
    jmp 3b
4:
    mov $0x3F8, %dx
    mov $0x1234, %ax
    out %ax, %dx
    in $0x60, %al
    mov $0xA000, %ax
    mov %ax, %es
    movb $0x55, %es:0x10
    mov %es:0x10, %bl
    mov %bl, %al
    out %al, $0xE9
    hlt
    jmp 4b
msg:
    .asciz "hello from real mode\n"
guest_test_end:
    .code64

// Host exception stubs (see exc.c): push a zero error code where the CPU did
// not, push the vector, save the GPRs and call host_exception(frame).
    .intel_syntax noprefix
    .code64
    .macro EXC_NOERR n
    .globl exc_stub_\n
exc_stub_\n:
    push 0
    push \n
    jmp exc_common
    .endm
    .macro EXC_ERR n
    .globl exc_stub_\n
exc_stub_\n:
    push \n
    jmp exc_common
    .endm
    EXC_NOERR 0
    EXC_NOERR 1
    EXC_NOERR 3
    EXC_NOERR 4
    EXC_NOERR 5
    EXC_NOERR 6
    EXC_NOERR 7
    EXC_ERR 8
    EXC_ERR 10
    EXC_ERR 11
    EXC_ERR 12
    EXC_ERR 13
    EXC_ERR 14
    EXC_NOERR 16
    EXC_ERR 17
    EXC_NOERR 18
    EXC_NOERR 19
exc_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rcx, rsp
    and rsp, -16
    sub rsp, 32
    call host_exception
1:
    hlt
    jmp 1b

// NMI: record where the core was (host_nmi(frame)) and carry on. The hang
// watch on a helper core sends one when the main loop stops moving.
    .globl nmi_stub
nmi_stub:
    push 0
    push 2
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rcx, rsp
    mov rbx, rsp
    and rsp, -16
    sub rsp, 32
    call host_nmi
    mov rsp, rbx
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq
