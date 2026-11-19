#ifndef WYSE_SYSENTER_MSR_H
#define WYSE_SYSENTER_MSR_H

#include "vmx.h"

/* vm_read/vm_write must be provided by the VMCS backend before inclusion.
 * Intel SDM vol.3C 31.10.4.2: VM entry/exit switches these guest MSRs through
 * the VMCS. Never write the root-mode hardware MSRs on behalf of a guest. */
enum WyseSysenterResult {
    WYSE_SYSENTER_GP = -1,
    WYSE_SYSENTER_NOT_HANDLED = 0,
    WYSE_SYSENTER_OK = 1
};

static inline uint64_t wyse_sysenter_field(uint32_t msr)
{
    switch (msr) {
    case 0x174: return VMCS_GUEST_SYSENTER_CS;
    case 0x175: return VMCS_GUEST_SYSENTER_ESP;
    case 0x176: return VMCS_GUEST_SYSENTER_EIP;
    default: return 0;
    }
}

static inline int wyse_sysenter_msr_read(uint32_t msr, unsigned cpl, uint64_t *value)
{
    uint64_t field = wyse_sysenter_field(msr);
    if (!field) return WYSE_SYSENTER_NOT_HANDLED;
    if (cpl) return WYSE_SYSENTER_GP;
    *value = vm_read(field);
    if (msr == 0x174) *value = (uint32_t)*value;
    return WYSE_SYSENTER_OK;
}

static inline int wyse_sysenter_msr_write(uint32_t msr, unsigned cpl, uint64_t value)
{
    uint64_t field = wyse_sysenter_field(msr);
    if (!field) return WYSE_SYSENTER_NOT_HANDLED;
    if (cpl) return WYSE_SYSENTER_GP;
    if (msr == 0x174) {
        /* SDM vol.4: CS bits31:16 read/write; bits63:32 ignored on write. */
        value = (uint32_t)value;
    } else {
        /* This VMX backend uses a 48-bit linear-address host. WRMSR rejects
         * a noncanonical EIP/ESP even while the guest runs in legacy mode. */
        uint64_t top = value >> 47;
        if (top != 0 && top != 0x1ffff) return WYSE_SYSENTER_GP;
    }
    vm_write(field, value);
    return WYSE_SYSENTER_OK;
}

#endif
