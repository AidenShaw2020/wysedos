/* Real engine lifecycle against RAM MMIO. No privileged I/O is executed:
 * fixtures represent an already allocated engine, as after its first open. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define WYSE_BUILD 1
#include "../../efi/platform/gpu3d.c"

EFI_BOOT_SERVICES *gBS;
static uint32_t registers[0x140000 / 4];
static uint32_t ring_memory[RING_BYTES / 4], status_memory[1024];
static uint64_t mapping_memory[8];
static uint64_t clock_us;
static uint32_t reject_reg;
static int reject_on = -1;
static uint32_t reject_reg2;
static int reject_on2 = -1;

uint64_t wyse_ticks_us(void)
{
    const uint32_t domains[] = {0x1300b0, 0x1300b8};
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t reg = domains[i], request = registers[reg / 4];
        int on = (request & 1) != 0;
        if ((request & 0x10000) && !(reg == reject_reg && on == reject_on) &&
            !(reg == reject_reg2 && on == reject_on2))
            registers[(reg + 4) / 4] = (uint32_t)on;
    }
    return clock_us += 1000;
}

static void fixture(void)
{
    memset(&g, 0, sizeof g);
    memset(registers, 0, sizeof registers);
    memset(ring_memory, 0x5a, sizeof ring_memory);
    memset(status_memory, 0, sizeof status_memory);
    memset(mapping_memory, 0x36, sizeof mapping_memory);
    ring_lock = 0; reject_reg = reject_reg2 = 0;
    reject_on = reject_on2 = -1; clock_us = 0;
    g.open = g.awake = true;
    g.mmio = (volatile uint8_t *)registers;
    g.ring = ring_memory; g.status = status_memory; g.ggtt = mapping_memory;
    g.ring_tail = 128; g.ring_seqno = 17; g.seqno = 23;
    g.next_index = GGTT_BASE_INDEX + 40;
    g.ring_gpu = 0x50000000; g.status_gpu = 0x50008000;
    status_memory[RING_SEQNO] = 17;
    registers[(RCS + 0x3c) / 4] = 0x7001;
    registers[(RCS + 0x30) / 4] = 128;
    registers[(RCS + 0x34) / 4] = 128;
    registers[0x1300b4 / 4] = registers[0x1300bc / 4] = 1;
}

static void preserved(void)
{
    assert(g.open && !g.broken);
    assert(g.ring == ring_memory && g.status == status_memory && g.ggtt == mapping_memory);
    assert(g.ring_tail == 128 && g.ring_seqno == 17 && g.seqno == 23);
    assert(g.next_index == GGTT_BASE_INDEX + 40);
    assert(g.ring_gpu == 0x50000000 && g.status_gpu == 0x50008000);
    assert(registers[(RCS + 0x3c) / 4] == (g.suspended ? 0 : 0x7001));
    assert(registers[(RCS + 0x30) / 4] == 128);
    assert(registers[(RCS + 0x34) / 4] == 128);
    for (unsigned i = 0; i < sizeof ring_memory / sizeof *ring_memory; ++i)
        assert(ring_memory[i] == 0x5a5a5a5a);
    for (unsigned i = 0; i < 8; ++i) assert(mapping_memory[i] == 0x3636363636363636ULL);
    assert(!ring_lock);
}

int main(void)
{
    fixture(); status_memory[RING_SEQNO] = 16;
    gpu3d_idle_release(); assert(g.awake); preserved();
    assert(registers[0x1300b0 / 4] == 0 && registers[0x1300b8 / 4] == 0);

    fixture(); registers[(RCS + 0x34) / 4] = 120;
    gpu3d_idle_release(); assert(g.awake && !g.suspended);
    assert(registers[(RCS + 0x3c) / 4] == 0x7001);

    fixture(); gpu3d_idle_release(); assert(!g.awake); preserved();
    assert(!registers[0x1300b4 / 4] && !registers[0x1300bc / 4]);
    gpu3d_idle_release(); assert(!g.awake); preserved();
    assert(gpu3d_open()); assert(g.awake); preserved();
    assert(registers[0x1300b4 / 4] && registers[0x1300bc / 4]);

    fixture(); gpu3d_idle_release(); reject_reg = 0x1300b8; reject_on = 1;
    assert(!gpu3d_open()); assert(!g.awake); preserved();
    assert(!registers[0x1300b4 / 4] && !registers[0x1300bc / 4]);
    reject_reg = 0; assert(gpu3d_open()); assert(g.awake); preserved();

    fixture(); reject_reg = 0x1300b8; reject_on = 0;
    gpu3d_idle_release(); assert(g.awake); preserved();
    assert(registers[0x1300b4 / 4] && registers[0x1300bc / 4]);

    /* Release fails in media, and restoring render also fails: never claim
     * awake or submit using the partially restored engine. */
    fixture(); reject_reg = 0x1300b8; reject_on = 0;
    reject_reg2 = 0x1300b0; reject_on2 = 1;
    gpu3d_idle_release(); assert(!g.awake); preserved();
    assert(!gpu3d_open()); assert(!g.awake); preserved();
    reject_reg = reject_reg2 = 0;
    assert(gpu3d_open()); assert(g.awake); preserved();

    fixture(); gpu3d_idle_release(); reject_reg = 0x1300b8; reject_on = 1;
    assert(gpu3d_submit_guest(0x50100000, 0x50200000, 99) != 0);
    assert(!g.awake); preserved();
    reject_reg = 0;
    assert(gpu3d_submit_guest(0x50100000, 0x50200000, 99) == 0);
    assert(g.awake && g.ring_seqno == 18 && g.guest_submits == 1);
    assert(g.ring_tail != 128 && status_memory[RING_SEQNO] == 17);
    gpu3d_idle_release(); assert(g.awake); /* submitted fence still pending */
    status_memory[RING_SEQNO] = 18;
    gpu3d_idle_release(); assert(g.awake); /* fence done but HEAD lags */
    registers[(RCS + 0x34) / 4] = g.ring_tail;
    gpu3d_idle_release(); assert(!g.awake);

    fixture(); g.broken = true; gpu3d_idle_release(); assert(g.awake);
    assert(registers[0x1300b0 / 4] == 0);
    gpu3d_close(); assert(!g.open && !g.awake);
    assert(registers[(RCS + 0x3c) / 4] == 0);
    fixture(); gpu3d_idle_release(); gpu3d_close();
    assert(!g.open && !g.awake && !registers[(RCS + 0x3c) / 4]);
    puts("gpu3d lifecycle PASS: pending, idle preservation, resume, ACK failure rollback");
    return 0;
}
