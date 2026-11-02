// Intel VMX definitions (SDM vol. 3, appendix B) and the VMX run interface.
#ifndef VMX_H
#define VMX_H

#include <stdint.h>
#include <stdbool.h>

// 16-bit fields
#define VMCS_VPID                   0x0000
#define VMCS_GUEST_ES_SEL           0x0800
#define VMCS_GUEST_CS_SEL           0x0802
#define VMCS_GUEST_SS_SEL           0x0804
#define VMCS_GUEST_DS_SEL           0x0806
#define VMCS_GUEST_FS_SEL           0x0808
#define VMCS_GUEST_GS_SEL           0x080A
#define VMCS_GUEST_LDTR_SEL         0x080C
#define VMCS_GUEST_TR_SEL           0x080E
#define VMCS_HOST_ES_SEL            0x0C00
#define VMCS_HOST_CS_SEL            0x0C02
#define VMCS_HOST_SS_SEL            0x0C04
#define VMCS_HOST_DS_SEL            0x0C06
#define VMCS_HOST_FS_SEL            0x0C08
#define VMCS_HOST_GS_SEL            0x0C0A
#define VMCS_HOST_TR_SEL            0x0C0C

// 64-bit fields
#define VMCS_IO_BITMAP_A            0x2000
#define VMCS_IO_BITMAP_B            0x2002
#define VMCS_MSR_BITMAP             0x2004
#define VMCS_TSC_OFFSET             0x2010
#define VMCS_EPT_POINTER            0x201A
#define VMCS_GUEST_PHYS_ADDR        0x2400
#define VMCS_LINK_POINTER           0x2800
#define VMCS_GUEST_DEBUGCTL         0x2802
#define VMCS_GUEST_PAT              0x2804
#define VMCS_GUEST_EFER             0x2806
#define VMCS_HOST_PAT               0x2C00
#define VMCS_HOST_EFER              0x2C02

// 32-bit fields
#define VMCS_PIN_CTLS               0x4000
#define VMCS_PROC_CTLS              0x4002
#define VMCS_EXCEPTION_BITMAP       0x4004
#define VMCS_PF_ERR_MASK            0x4006
#define VMCS_PF_ERR_MATCH           0x4008
#define VMCS_CR3_TARGET_COUNT       0x400A
#define VMCS_EXIT_CTLS              0x400C
#define VMCS_EXIT_MSR_STORE_COUNT   0x400E
#define VMCS_EXIT_MSR_LOAD_COUNT    0x4010
#define VMCS_ENTRY_CTLS             0x4012
#define VMCS_ENTRY_MSR_LOAD_COUNT   0x4014
#define VMCS_ENTRY_INTR_INFO        0x4016
#define VMCS_ENTRY_EXC_ERRCODE      0x4018
#define VMCS_ENTRY_INSTR_LEN        0x401A
#define VMCS_TPR_THRESHOLD          0x401C
#define VMCS_PROC_CTLS2             0x401E
#define VMCS_VM_INSTR_ERROR         0x4400
#define VMCS_EXIT_REASON            0x4402
#define VMCS_EXIT_INTR_INFO         0x4404
#define VMCS_EXIT_INTR_ERRCODE      0x4406
#define VMCS_IDT_VECTORING_INFO     0x4408
#define VMCS_IDT_VECTORING_ERRCODE  0x440A
#define VMCS_EXIT_INSTR_LEN         0x440C
#define VMCS_EXIT_INSTR_INFO        0x440E
#define VMCS_GUEST_ES_LIMIT         0x4800
#define VMCS_GUEST_CS_LIMIT         0x4802
#define VMCS_GUEST_SS_LIMIT         0x4804
#define VMCS_GUEST_DS_LIMIT         0x4806
#define VMCS_GUEST_FS_LIMIT         0x4808
#define VMCS_GUEST_GS_LIMIT         0x480A
#define VMCS_GUEST_LDTR_LIMIT       0x480C
#define VMCS_GUEST_TR_LIMIT         0x480E
#define VMCS_GUEST_GDTR_LIMIT       0x4810
#define VMCS_GUEST_IDTR_LIMIT       0x4812
#define VMCS_GUEST_ES_AR            0x4814
#define VMCS_GUEST_CS_AR            0x4816
#define VMCS_GUEST_SS_AR            0x4818
#define VMCS_GUEST_DS_AR            0x481A
#define VMCS_GUEST_FS_AR            0x481C
#define VMCS_GUEST_GS_AR            0x481E
#define VMCS_GUEST_LDTR_AR          0x4820
#define VMCS_GUEST_TR_AR            0x4822
#define VMCS_GUEST_INTERRUPTIBILITY 0x4824
#define VMCS_GUEST_ACTIVITY         0x4826
#define VMCS_GUEST_SYSENTER_CS      0x482A
#define VMCS_PREEMPTION_TIMER       0x482E
#define VMCS_HOST_SYSENTER_CS       0x4C00

// natural-width fields
#define VMCS_CR0_MASK               0x6000
#define VMCS_CR4_MASK               0x6002
#define VMCS_CR0_SHADOW             0x6004
#define VMCS_CR4_SHADOW             0x6006
#define VMCS_EXIT_QUALIFICATION     0x6400
#define VMCS_GUEST_LINEAR_ADDR      0x640A
#define VMCS_GUEST_CR0              0x6800
#define VMCS_GUEST_CR3              0x6802
#define VMCS_GUEST_CR4              0x6804
#define VMCS_GUEST_ES_BASE          0x6806
#define VMCS_GUEST_CS_BASE          0x6808
#define VMCS_GUEST_SS_BASE          0x680A
#define VMCS_GUEST_DS_BASE          0x680C
#define VMCS_GUEST_FS_BASE          0x680E
#define VMCS_GUEST_GS_BASE          0x6810
#define VMCS_GUEST_LDTR_BASE        0x6812
#define VMCS_GUEST_TR_BASE          0x6814
#define VMCS_GUEST_GDTR_BASE        0x6816
#define VMCS_GUEST_IDTR_BASE        0x6818
#define VMCS_GUEST_DR7              0x681A
#define VMCS_GUEST_RSP              0x681C
#define VMCS_GUEST_RIP              0x681E
#define VMCS_GUEST_RFLAGS           0x6820
#define VMCS_GUEST_PENDING_DBG      0x6822
#define VMCS_GUEST_SYSENTER_ESP     0x6824
#define VMCS_GUEST_SYSENTER_EIP     0x6826
#define VMCS_HOST_CR0               0x6C00
#define VMCS_HOST_CR3               0x6C02
#define VMCS_HOST_CR4               0x6C04
#define VMCS_HOST_FS_BASE           0x6C06
#define VMCS_HOST_GS_BASE           0x6C08
#define VMCS_HOST_TR_BASE           0x6C0A
#define VMCS_HOST_GDTR_BASE         0x6C0C
#define VMCS_HOST_IDTR_BASE         0x6C0E
#define VMCS_HOST_SYSENTER_ESP      0x6C10
#define VMCS_HOST_SYSENTER_EIP      0x6C12
#define VMCS_HOST_RSP               0x6C14
#define VMCS_HOST_RIP               0x6C16

// pin-based controls
#define PIN_EXT_INTR_EXIT           (1u << 0)
#define PIN_NMI_EXIT                (1u << 3)
#define PIN_VIRTUAL_NMI             (1u << 5)
#define PIN_PREEMPTION_TIMER        (1u << 6)

// primary processor-based controls
#define PROC_INTR_WINDOW_EXIT       (1u << 2)
#define PROC_TSC_OFFSET             (1u << 3)
#define PROC_HLT_EXIT               (1u << 7)
#define PROC_INVLPG_EXIT            (1u << 9)
#define PROC_MWAIT_EXIT             (1u << 10)
#define PROC_RDPMC_EXIT             (1u << 11)
#define PROC_RDTSC_EXIT             (1u << 12)
#define PROC_CR3_LOAD_EXIT          (1u << 15)
#define PROC_CR3_STORE_EXIT         (1u << 16)
#define PROC_CR8_LOAD_EXIT          (1u << 19)
#define PROC_CR8_STORE_EXIT         (1u << 20)
#define PROC_NMI_WINDOW_EXIT        (1u << 22)
#define PROC_MOV_DR_EXIT            (1u << 23)
#define PROC_UNCOND_IO_EXIT         (1u << 24)
#define PROC_USE_IO_BITMAPS         (1u << 25)
#define PROC_MONITOR_TRAP           (1u << 27)
#define PROC_USE_MSR_BITMAPS        (1u << 28)
#define PROC_MONITOR_EXIT           (1u << 29)
#define PROC_PAUSE_EXIT             (1u << 30)
#define PROC_SECONDARY              (1u << 31)

// secondary processor-based controls
#define PROC2_EPT                   (1u << 1)
#define PROC2_DESC_TABLE_EXIT       (1u << 2)
#define PROC2_RDTSCP                (1u << 3)
#define PROC2_VPID                  (1u << 5)
#define PROC2_WBINVD_EXIT           (1u << 6)
#define PROC2_UNRESTRICTED_GUEST    (1u << 7)

// exit controls
#define EXIT_SAVE_DEBUG             (1u << 2)
#define EXIT_HOST_64BIT             (1u << 9)
#define EXIT_ACK_INTR               (1u << 15)
#define EXIT_SAVE_PAT               (1u << 18)
#define EXIT_LOAD_PAT               (1u << 19)
#define EXIT_SAVE_EFER              (1u << 20)
#define EXIT_LOAD_EFER              (1u << 21)
#define EXIT_SAVE_PREEMPT           (1u << 22)

// entry controls
#define ENTRY_LOAD_DEBUG            (1u << 2)
#define ENTRY_IA32E_GUEST           (1u << 9)
#define ENTRY_LOAD_PAT              (1u << 14)
#define ENTRY_LOAD_EFER             (1u << 15)

// exit reasons
#define EXIT_EXCEPTION_NMI          0
#define EXIT_EXTERNAL_INTR          1
#define EXIT_TRIPLE_FAULT           2
#define EXIT_INIT                   3
#define EXIT_SIPI                   4
#define EXIT_INTR_WINDOW            7
#define EXIT_NMI_WINDOW             8
#define EXIT_TASK_SWITCH            9
#define EXIT_CPUID                  10
#define EXIT_HLT                    12
#define EXIT_INVD                   13
#define EXIT_INVLPG                 14
#define EXIT_RDPMC                  15
#define EXIT_RDTSC                  16
#define EXIT_VMCALL                 18
#define EXIT_CR_ACCESS              28
#define EXIT_DR_ACCESS              29
#define EXIT_IO                     30
#define EXIT_RDMSR                  31
#define EXIT_WRMSR                  32
#define EXIT_INVALID_GUEST          33
#define EXIT_MSR_LOADING            34
#define EXIT_MWAIT                  36
#define EXIT_MONITOR_TRAP           37
#define EXIT_MONITOR                39
#define EXIT_PAUSE                  40
#define EXIT_MACHINE_CHECK          41
#define EXIT_GDTR_IDTR              46
#define EXIT_LDTR_TR                47
#define EXIT_EPT_VIOLATION          48
#define EXIT_EPT_MISCONFIG          49
#define EXIT_INVEPT                 50
#define EXIT_RDTSCP                 51
#define EXIT_PREEMPTION_TIMER       52
#define EXIT_INVVPID                53
#define EXIT_WBINVD                 54
#define EXIT_XSETBV                 55

// guest general purpose registers saved by the exit stub (order matters, see vmx_asm.S)
typedef struct {
    uint64_t rax, rcx, rdx, rbx, rsp_unused, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
} guest_regs_t;

// vmx_asm.S
int vmx_run(guest_regs_t *regs, int launched);   // 0 = VM exit, 1 = VMfail
uint64_t vmx_vmread(uint64_t field);
int vmx_vmwrite(uint64_t field, uint64_t value);  // 0 ok
int vmx_vmxon(uint64_t *phys);
int vmx_vmclear(uint64_t *phys);
int vmx_vmptrld(uint64_t *phys);
void vmx_vmxoff(void);
int vmx_invept(uint64_t type, void *desc);

#endif
