/* clang tools/tests/firmware_events.c -o work/firmware_events_test.exe
 * Verify the actual main-loop guard and delivery of held notifications. */
#include "../../efi/platform/firmware_events.c"
#include <stdlib.h>
EFI_BOOT_SERVICES *gBS;
static uint64_t now;
static EFI_TPL current=4;
static int pending,delivered;
static void (*notify)(EFI_EVENT,void *);
uint64_t wyse_ticks_us(void) { return now; }
static EFI_TPL raise(EFI_TPL next) { EFI_TPL old=current; if (next<old) abort(); current=next; return old; }
static void restore(EFI_TPL next) {
    if (next>current) abort(); current=next;
    if (pending && next<16) { pending=0; delivered++; notify((void *)1,NULL); }
}
static EFI_STATUS locate(const EFI_GUID *guid,void *key,void **p) {
    (void)guid;(void)key;(void)p; return EFI_NOT_FOUND;
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
    firmware_events_init(); CHECK(current==4 && tpl_repairs==0);
    now=100000; firmware_events_poll(now); CHECK(current==4 && tpl_repairs==0);
    current=16; pending=1; now=200000; firmware_events_poll(now);
    CHECK(current==4 && tpl_repairs==1 && delivered==1 && ticks==1 && tick_at==now);
    current=31; pending=1; now=300000; firmware_events_poll(now);
    CHECK(current==4 && tpl_repairs==2 && delivered==2 && ticks==2);
    now=400000; firmware_events_poll(now); CHECK(tpl_repairs==2);
    puts("PASS: normal TPL preserved; leaked NOTIFY/HIGH TPL recovered; pending heartbeat delivered");
    return 0;
}
