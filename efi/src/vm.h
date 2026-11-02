// A single virtual PC on the bootstrap processor, run with VT-x.
#ifndef VM_H
#define VM_H

#include "vmx.h"

typedef struct {
    uint8_t *ram;               // guest physical 0 .. ram_size, host-identity-mapped buffer
    uint64_t ram_size;
    guest_regs_t regs;
    bool launched;

    // last exit
    uint32_t exit_reason;
    uint64_t exit_qual;
    uint64_t exit_gpa;
    uint64_t exit_gla;
    uint32_t exit_instr_len;
    uint32_t exit_intr_info;
} vm_t;

// Host side: VMXON on this CPU. Returns NULL on success or an error string.
const char *vmx_host_init(void);

// Allocate guest RAM and build EPT + VMCS. The VGA window 0xA0000-0xBFFFF is left
// unmapped (EPT violations) unless vm_map() maps it.
const char *vm_create(vm_t *vm, void *ram, uint64_t ram_size);

// Map guest-physical [gpa, gpa+size) to host memory (4 KB granularity), or unmap
// it when host == NULL. Flushes EPT.
void vm_map(vm_t *vm, uint64_t gpa, void *host, uint64_t size, bool writable);

// The guest's cached translations gone (INVEPT, which takes the combined
// linear mappings too): what a CR3 load does on the real thing
void vm_flush_tlb(void);

// Let the guest's IN/OUT on `port` reach the hardware (trap = false), or exit again
void vm_io_trap(uint16_t port, bool trap);

// Map guest-physical [gpa, gpa+size) onto host-physical device memory, uncached
// whatever the guest's page tables say (the Atom's own VGA at A0000).
void vm_map_mmio(vm_t *vm, uint64_t gpa, uint64_t hpa, uint64_t size);

// Real-mode reset state with CS:IP as given (CS base = cs << 4).
void vm_reset_real(vm_t *vm, uint16_t cs, uint16_t ip);

// Run until the next VM exit. Returns false if VM entry failed (vm->exit_reason
// then holds the VM-instruction error).
bool vm_run(vm_t *vm);

// Advance guest RIP past the exiting instruction.
void vm_skip_instr(vm_t *vm);

uint64_t vm_read(uint64_t field);
void vm_write(uint64_t field, uint64_t value);

#endif
