/* Actual network logger with a cable-aware firmware SNP model. */
#include <stdio.h>
#include <stdlib.h>
#include "../../efi/src/net.c"
EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_HANDLE gImage;
void *gImageBase;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static unsigned status_calls, transmits;
static bool cable;
static EFI_SIMPLE_NETWORK_MODE mode;
static EFI_STATUS status(EFI_SIMPLE_NETWORK_PROTOCOL *s,uint32_t *irq,void **done) {
    CHECK(++status_calls <= 8);
    s->Mode->MediaPresent=cable;
    if (irq) *irq=0;
    if (done) *done=cable?(void *)tx_frame:NULL;
    return EFI_SUCCESS;
}
static EFI_STATUS transmit(EFI_SIMPLE_NETWORK_PROTOCOL *s,uint64_t h,uint64_t n,void *b,
                           EFI_MAC_ADDRESS *src,EFI_MAC_ADDRESS *dst,uint16_t *p) {
    (void)s;(void)h;(void)n;(void)b;(void)src;(void)dst;(void)p;
    transmits++;
    return EFI_SUCCESS;
}
int main(void) {
    EFI_SIMPLE_NETWORK_PROTOCOL device={0};
    device.Mode=&mode;device.GetStatus=status;device.Transmit=transmit;snp=&device;
    mode.State=EfiSimpleNetworkInitialized;mode.MediaPresentSupported=true;
    /* Stale link-up must be refreshed before attempting a log transmission. */
    mode.MediaPresent=true;cable=false;
    net_broadcast(5555,"offline",7);
    CHECK(status_calls==1 && transmits==0);
    /* Reconnecting must recover without restarting firmware. */
    status_calls=0;cable=true;
    net_broadcast(5555,"online",6);
    CHECK(transmits==1 && status_calls<=2);
    status_calls=0;cable=false;
    net_broadcast(5555,"offline",7);
    CHECK(status_calls==1 && transmits==1);
    /* A NIC without cable reporting retains the existing send path. */
    status_calls=0;cable=true;mode.MediaPresentSupported=false;
    net_broadcast(5555,"unknown",7);
    CHECK(transmits==2 && status_calls==1);
    puts("PASS offline logs skip transmit waits, refresh stale link state and recover on cable reconnection");
    return 0;
}
