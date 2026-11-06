#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define vm_read native_vm_read
#define vm_write native_vm_write
#include "../../efi/src/vmx.h"
#undef vm_read
#undef vm_write
static uint64_t fields[0x7000], perf_ctrl;
static bool pmu_supported = true;
static unsigned pmu_writes;
static uint64_t rdmsr_(uint32_t msr) {
    if(msr==0x490) return pmu_supported ? (uint64_t)ENTRY_LOAD_PERF_GLOBAL_CTRL<<32 : 0;
    if(msr==0x48f) return pmu_supported ? (uint64_t)EXIT_LOAD_PERF_GLOBAL_CTRL<<32 : 0;
    return perf_ctrl;
}
static void wrmsr_(uint32_t msr,uint64_t value) { (void)msr;perf_ctrl=value;pmu_writes++; }
static uint64_t vm_read(uint32_t field) { return fields[field]; }
static void vm_write(uint32_t field,uint64_t value) { fields[field]=value;pmu_writes++; }
#include "../../work/vmx-speed/actual_budget.h"

#define CHECK(condition) do { if (!(condition)) { printf("FAIL line %d\n", __LINE__); return 1; } } while (0)

static uint64_t calibration_ticks(uint64_t cpms)
{
    const uint64_t khz = 1440000;
    uint64_t now = 0, last = 0, remaining = 55000;
    int64_t budget = 0;
    while (remaining) {
        now += khz / 1000;
        uint64_t left = remaining;
        bool ready = cycle_budget_ready(&budget, now - last, cpms, khz, &left);
        last = now;
        if (!ready) continue;
        uint64_t used = left < remaining ? left : remaining;
        remaining -= used;
        if (cpms) budget -= (int64_t)used;
        now += used;
    }
    return now * 1193182 / (khz * 1000);
}

int main(int argc, char **argv)
{
    perf_ctrl=(3ull<<32)|16;
    fields[VMCS_ENTRY_CTLS]=ENTRY_LOAD_PAT;
    fields[VMCS_EXIT_CTLS]=EXIT_HOST_64BIT|EXIT_LOAD_EFER;
    CHECK(configure_guest_cycles());
    CHECK(fields[VMCS_ENTRY_CTLS]==(ENTRY_LOAD_PAT|ENTRY_LOAD_PERF_GLOBAL_CTRL));
    CHECK(fields[VMCS_EXIT_CTLS]==(EXIT_HOST_64BIT|EXIT_LOAD_EFER|EXIT_LOAD_PERF_GLOBAL_CTRL));
    CHECK(perf_ctrl==16);
    CHECK(fields[VMCS_HOST_PERF_GLOBAL_CTRL]==perf_ctrl);
    CHECK(fields[VMCS_GUEST_PERF_GLOBAL_CTRL]==((3ull<<32)|16));
    uint64_t cycles=0,instructions=0;
    if(perf_ctrl&(1ull<<33)) cycles+=1000;
    if(perf_ctrl&(1ull<<32)) instructions+=500;
    perf_ctrl=fields[VMCS_GUEST_PERF_GLOBAL_CTRL];
    if(perf_ctrl&(1ull<<33)) cycles+=300;
    if(perf_ctrl&(1ull<<32)) instructions+=80;
    perf_ctrl=fields[VMCS_HOST_PERF_GLOBAL_CTRL];
    bool host_negative=argc>1 && !strcmp(argv[1],"--host-cycle-accounting");
    bool instruction_negative=argc>1 && !strcmp(argv[1],"--host-instruction-accounting");
    if(host_negative) perf_ctrl|=1ull<<33;
    if(instruction_negative) perf_ctrl|=1ull<<32;
    if(perf_ctrl&(1ull<<33)) cycles+=5000;
    if(perf_ctrl&(1ull<<32)) instructions+=1300;
    printf("%s: PMU charged %llu cycles for 300 guest and 6000 host cycles\n",cycles==300?"PASS":"FAIL",(unsigned long long)cycles);
    CHECK(cycles==300);
    printf("%s: PMU charged %llu instructions for 80 guest and 1800 host instructions\n",instructions==80?"PASS":"FAIL",(unsigned long long)instructions);
    CHECK(instructions==80);
    pmu_supported=false;unsigned before=pmu_writes;
    CHECK(!configure_guest_cycles() && pmu_writes==before);
    puts("PASS: guest-only PMU preserves existing control bits and unsupported CPUs remain untouched");
    int64_t budget = 7;
    uint64_t left = 123;
    CHECK(cycle_budget_ready(&budget, 0, 0, 1440000, &left));
    CHECK(budget == 7 && left == 123);
    budget = 0; left = 1000000;
    CHECK(!cycle_budget_ready(&budget, 0, 166000, 1440000, &left));
    CHECK(cycle_budget_ready(&budget, 72000, 166000, 1440000, &left));
    CHECK(budget == 8300 && left == 8300);
    budget = -10000; left = 1000000;
    CHECK(!cycle_budget_ready(&budget, 72000, 166000, 1440000, &left));
    CHECK(budget == -1700);
    budget = 0; left = 1000000;
    CHECK(cycle_budget_ready(&budget, 1440000000, 166000, 1440000, &left));
    CHECK(budget == 8300 && left == 8300);
    budget = 0; left = 100;
    CHECK(cycle_budget_ready(&budget, 72000, 166000, 1440000, &left));
    CHECK(left == 100);
    bool negative = argc > 1 && !strcmp(argv[1], "--no-cycle-limit");
    uint64_t ticks = calibration_ticks(negative ? 0 : 166000);
    printf("%s: native LOOP timing PIT ticks=%llu; 16-bit divisor needs >=91\n",
           ticks >= 91 ? "PASS" : "FAIL", (unsigned long long)ticks);
    CHECK(ticks >= 91 && ticks < 600);
    CHECK(calibration_ticks(60000) >= ticks);
    puts("PASS: disabled budget, debt, 50us cap, idle/I/O credit cap and shorter preemption deadline");
    return 0;
}