/* Firmware timer / notification health, called only from the application's
 * main loop. A leaked TPL can leave IF=1 and the CPU running while every USB
 * async callback remains queued. Never lower TPL from inside a callback. */
#include <stdio.h>
#include "bio.h"
extern EFI_BOOT_SERVICES *gBS;
uint64_t wyse_ticks_us(void);
#define TPL_APPLICATION 4
#define TPL_NOTIFY 16
#define TPL_HIGH_LEVEL 31
#define EVT_NOTIFY_SIGNAL 0x200

typedef struct timer_arch timer_arch;
struct timer_arch {
    void *RegisterHandler;
    EFI_STATUS (*SetTimerPeriod)(timer_arch *,uint64_t);
    EFI_STATUS (*GetTimerPeriod)(timer_arch *,uint64_t *);
    EFI_STATUS (*GenerateSoftInterrupt)(timer_arch *);
};
static timer_arch *timer;
static uint64_t period;
static volatile uint64_t tick_at;
static volatile uint32_t ticks;
static uint32_t tpl_repairs;
static uint64_t checked_at, warned_at;
static EFI_EVENT heartbeat;

static void tick(EFI_EVENT ev,void *ctx) {
    (void)ev;(void)ctx;
    tick_at=wyse_ticks_us(); ticks++;
}

/* Main-loop boundary: no firmware notification is on this call stack.
 * Restore to application priority only if firmware returned at a higher TPL.
 * This also dispatches notifications held pending at the leaked priority. */
static EFI_TPL application_priority(void) {
    EFI_TPL old=gBS->RaiseTPL(TPL_HIGH_LEVEL);
    gBS->RestoreTPL(old>TPL_APPLICATION?TPL_APPLICATION:old);
    if (old>TPL_APPLICATION) {
        tpl_repairs++;
        printf("firmware events: returned at TPL %llu; restored application TPL 4 (%u)\n",
            (unsigned long long)old,tpl_repairs);
    }
    return old;
}

void firmware_events_init(void) {
    static const EFI_GUID guid={0x26baccb3,0x6f42,0x11d4,{0xbc,0xe7,0,0x80,0xc7,0x3c,0x88,0x81}};
    EFI_TPL initial=application_priority();
    if (!EFI_ERROR(gBS->LocateProtocol(&guid,NULL,(void **)&timer))) timer->GetTimerPeriod(timer,&period);
    EFI_STATUS r=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_NOTIFY,tick,NULL,&heartbeat);
    if (!EFI_ERROR(r)) r=gBS->SetTimer(heartbeat,TimerPeriodic,100000);
    tick_at=wyse_ticks_us();
    printf("firmware events: initial TPL %llu, hardware period %llu x100 ns, heartbeat %llx\n",
        (unsigned long long)initial,(unsigned long long)period,(unsigned long long)r);
}

void firmware_events_poll(uint64_t now) {
    if (now-checked_at<100000) return;
    checked_at=now;
    application_priority();
    uint64_t age=now>=tick_at?now-tick_at:0;
    if (age>2000000 && now-warned_at>5000000) {
        warned_at=now;
        printf("firmware events: heartbeat stalled %llu ms, %u ticks, %u TPL repairs\n",
            (unsigned long long)(age/1000),ticks,tpl_repairs);
    }
}

void firmware_events_report(void) {
    EFI_TPL tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
    uint8_t irr0,isr0,irr1,isr1;
    __asm__ volatile("outb %0,$0x20; outb %0,$0xa0"::"a"((uint8_t)0x0b));
    __asm__ volatile("inb $0x20,%0":"=a"(isr0));
    __asm__ volatile("inb $0xa0,%0":"=a"(isr1));
    __asm__ volatile("outb %0,$0x20; outb %0,$0xa0"::"a"((uint8_t)0x0a));
    __asm__ volatile("inb $0x20,%0":"=a"(irr0));
    __asm__ volatile("inb $0xa0,%0":"=a"(irr1));
    gBS->RestoreTPL(tpl);
    uint64_t p=0; if (timer) timer->GetTimerPeriod(timer,&p);
    printf("firmware events: TPL %llu, PIC ISR %02x/%02x IRR %02x/%02x, ticks %u, age %llu ms, period %llu, repairs %u\n",
        (unsigned long long)tpl,isr0,isr1,irr0,irr1,ticks,
        (unsigned long long)((wyse_ticks_us()-tick_at)/1000),(unsigned long long)p,tpl_repairs);
}

/* Explicit diagnostic recovery; the main loop does not reprogram the timer
 * merely because a long USB/SMM call delayed a heartbeat. */
void firmware_events_recover(void) {
    application_priority();
    if (!timer) { printf("firmware events: no Timer Architectural Protocol\n"); return; }
    EFI_STATUS st=timer->SetTimerPeriod(timer,period?period:100000);
    printf("firmware events: rearm hardware timer status %llx\n",(unsigned long long)st);
}
