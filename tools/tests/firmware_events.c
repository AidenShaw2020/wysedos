/* clang tools/tests/firmware_events.c -o work/firmware_events_test.exe
 * Verify the actual main-loop guard and delivery of held notifications. */
#include "../../efi/platform/firmware_events.c"
#include <stdlib.h>
EFI_BOOT_SERVICES *gBS;
static uint64_t now;
static EFI_TPL current=4;
static int pending,delivered;
static void (*notify)(EFI_EVENT,void *);
static int have_timer,fail_get,fail_set;
static uint64_t hardware_period,last_period;
static unsigned period_writes;
static EFI_STATUS set_period(timer_arch *self,uint64_t p) {
    (void)self; period_writes++;last_period=p;
    if (fail_set) return EFI_UNSUPPORTED;
    hardware_period=p;return EFI_SUCCESS;
}
static EFI_STATUS get_period(timer_arch *self,uint64_t *p) {
    (void)self;if(fail_get) return EFI_DEVICE_ERROR;
    *p=hardware_period;return EFI_SUCCESS;
}
static timer_arch mock_timer={NULL,set_period,get_period,NULL};
uint64_t wyse_ticks_us(void) { return now; }
static EFI_TPL raise(EFI_TPL next) { EFI_TPL old=current; if (next<old) abort(); current=next; return old; }
static void restore(EFI_TPL next) {
    if (next>current) abort(); current=next;
    if (pending && next<16) { pending=0; delivered++; notify((void *)1,NULL); }
}
static EFI_STATUS locate(const EFI_GUID *guid,void *key,void **p) {
    (void)guid;(void)key;
    if (!have_timer) return EFI_NOT_FOUND;
    *p=&mock_timer;return EFI_SUCCESS;
}
static EFI_STATUS create(uint32_t type,EFI_TPL tpl,void *fn,void *ctx,EFI_EVENT *ev) {
    (void)ctx; if (type!=(EVT_TIMER|EVT_NOTIFY_SIGNAL) || tpl!=16) abort();
    notify=(void (*)(EFI_EVENT,void *))fn; *ev=(void *)1; return 0;
}
static EFI_STATUS set(EFI_EVENT ev,uint32_t type,uint64_t interval) {
    if (ev!=(void *)1 || type!=TimerPeriodic || interval!=100000) abort(); return 0;
}
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n",__LINE__); return 1; } } while(0)
int main(void) {
    static EFI_BOOT_SERVICES services;
    services.RaiseTPL=raise; services.RestoreTPL=restore; services.LocateProtocol=locate;
    services.CreateEvent=create; services.SetTimer=set; gBS=&services;
    firmware_events_init(5000); CHECK(current==4 && tpl_repairs==0 && period_writes==0);
    now=100000; firmware_events_poll(now); CHECK(current==4 && tpl_repairs==0);
    current=16; pending=1; now=200000; firmware_events_poll(now);
    CHECK(current==4 && tpl_repairs==1 && delivered==1 && ticks==1 && tick_at==now);
    current=31; pending=1; now=300000; firmware_events_poll(now);
    CHECK(current==4 && tpl_repairs==2 && delivered==2 && ticks==2);
    now=400000; firmware_events_poll(now); CHECK(tpl_repairs==2);
    have_timer=1;hardware_period=549254;
    firmware_events_init(0);CHECK(hardware_period==549254 && period_writes==0);
    firmware_events_init(5000);CHECK(hardware_period==50000 && period==50000 && last_period==50000 && period_writes==1);
    firmware_events_recover();CHECK(hardware_period==50000 && last_period==50000 && period_writes==2);
    hardware_period=10000;
    firmware_events_init(5000);CHECK(hardware_period==10000 && period==10000 && period_writes==2);
    hardware_period=549254;fail_set=1;
    firmware_events_init(5000);CHECK(hardware_period==549254 && period==549254 && period_writes==3);
    fail_set=0;fail_get=1;
    firmware_events_init(5000);CHECK(hardware_period==549254 && period_writes==3);
    puts("PASS: TPL recovery; opt-in 5 ms timer; faster/default timers preserved; unavailable/rejected timers tolerated; recovery retains selected period");
    return 0;
}
