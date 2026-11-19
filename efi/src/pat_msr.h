#ifndef WYSE_PAT_MSR_H
#define WYSE_PAT_MSR_H
#include "sysenter_msr.h"

/* Intel SDM vol.3A 13.12: PAT entries accept only 00,01,04,05,06,07.
 * VM entry/exit already switches guest PAT through this VMCS field. Never
 * modify the host PAT MSR for a guest write. Reuse the MSR dispatch results. */
static inline int wyse_pat_msr_read(uint32_t msr, unsigned cpl, uint64_t *value)
{
    if (msr != 0x277) return WYSE_SYSENTER_NOT_HANDLED;
    if (cpl) return WYSE_SYSENTER_GP;
    *value = vm_read(VMCS_GUEST_PAT);
    return WYSE_SYSENTER_OK;
}

static inline int wyse_pat_msr_write(uint32_t msr, unsigned cpl, uint64_t value)
{
    if (msr != 0x277) return WYSE_SYSENTER_NOT_HANDLED;
    if (cpl) return WYSE_SYSENTER_GP;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned type = (unsigned)(value >> (8 * i)) & 0xff;
        if (type > 7 || type == 2 || type == 3) return WYSE_SYSENTER_GP;
    }
    vm_write(VMCS_GUEST_PAT, value);
    return WYSE_SYSENTER_OK;
}
#endif
