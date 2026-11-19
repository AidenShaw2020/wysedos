/* Guest VMCS backend tests; does not execute privileged instructions. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t fields[0x7000];
static unsigned reads, writes;
static uint64_t vm_read(uint64_t field)
{
    assert(field < 0x7000);
    ++reads;
    return fields[field];
}
static void vm_write(uint64_t field, uint64_t value)
{
    assert(field < 0x7000);
    ++writes;
    fields[field] = value;
}
#include "../../efi/src/sysenter_msr.h"

int main(void)
{
    uint64_t value;
    unsigned before;
    const uint64_t valid[] = {0, 0xffffffff, 0x7fffffffffffULL,
                             0xffff800000000000ULL, 0xffffffffffffffffULL};
    const uint64_t invalid[] = {0x800000000000ULL, 0xffff7fffffffffffULL,
                               0x1000000000000ULL, 0x8000000000000000ULL};
    for (uint32_t msr = 0x174; msr <= 0x176; ++msr) {
        value = 1;
        assert(wyse_sysenter_msr_read(msr, 0, &value) == WYSE_SYSENTER_OK);
        assert(value == 0);
    }
    assert(wyse_sysenter_msr_write(0x174, 0, 0xdeadbeef12340008ULL) == WYSE_SYSENTER_OK);
    assert(wyse_sysenter_msr_read(0x174, 0, &value) == WYSE_SYSENTER_OK);
    assert(value == 0x12340008); /* upper CS word is readable, high dword ignored */
    assert(fields[VMCS_GUEST_SYSENTER_ESP] == 0 && fields[VMCS_GUEST_SYSENTER_EIP] == 0);
    for (uint32_t msr = 0x175; msr <= 0x176; ++msr) {
        for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
            assert(wyse_sysenter_msr_write(msr, 0, valid[i]) == WYSE_SYSENTER_OK);
            assert(wyse_sysenter_msr_read(msr, 0, &value) == WYSE_SYSENTER_OK);
            assert(value == valid[i]);
        }
        for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            before = writes;
            assert(wyse_sysenter_msr_write(msr, 0, invalid[i]) == WYSE_SYSENTER_GP);
            assert(writes == before);
            assert(wyse_sysenter_msr_read(msr, 0, &value) == WYSE_SYSENTER_OK);
            assert(value == valid[sizeof(valid) / sizeof(valid[0]) - 1]);
        }
    }
    for (unsigned cpl = 1; cpl <= 3; ++cpl) {
        for (uint32_t msr = 0x174; msr <= 0x176; ++msr) {
            value = 0x11223344;
            before = writes;
            assert(wyse_sysenter_msr_write(msr, cpl, 8) == WYSE_SYSENTER_GP);
            assert(writes == before);
            before = reads;
            assert(wyse_sysenter_msr_read(msr, cpl, &value) == WYSE_SYSENTER_GP);
            assert(value == 0x11223344 && reads == before);
        }
    }
    before = writes;
    value = 0x55667788;
    assert(wyse_sysenter_msr_write(0x177, 0, 8) == WYSE_SYSENTER_NOT_HANDLED);
    assert(wyse_sysenter_msr_read(0x10, 0, &value) == WYSE_SYSENTER_NOT_HANDLED);
    assert(value == 0x55667788 && writes == before);
    memset(fields, 0, sizeof(fields)); /* reset VMCS, then initialize XP-style values */
    assert(wyse_sysenter_msr_write(0x174, 0, 8) == WYSE_SYSENTER_OK);
    assert(wyse_sysenter_msr_write(0x175, 0, 0xf9363000) == WYSE_SYSENTER_OK);
    assert(wyse_sysenter_msr_write(0x176, 0, 0x80800000) == WYSE_SYSENTER_OK);
    assert(fields[VMCS_GUEST_SYSENTER_CS] == 8);
    assert(fields[VMCS_GUEST_SYSENTER_ESP] == 0xf9363000);
    assert(fields[VMCS_GUEST_SYSENTER_EIP] == 0x80800000);
    puts("PASS: SYSENTER guest MSRs, readback, canonical bounds, privilege and isolation");
    return 0;
}
