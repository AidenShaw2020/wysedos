/* Actual USB HID module with mocked firmware: recovery and balanced ownership.
 * clang -O2 -flto -fuse-ld=lld -Ipc tools/tests/usbhid_recovery.c
 *       -o work/usbhid_recovery_test.exe */
#include "../../efi/platform/usbhid.c"
#include <stdlib.h>
EFI_BOOT_SERVICES *gBS;
EFI_HANDLE gImage=(void *)42;
uint64_t wyse_ticks_us(void) { return 100; }
uint8_t g_kbd_typematic;
void ps2_put_keycode(PS2KbdState *s,int down,int key) { (void)s;(void)down;(void)key;abort(); }
void ps2_mouse_event(PS2MouseState *s,int dx,int dy,int dz,int buttons) {
    (void)s;(void)dx;(void)dy;(void)dz;(void)buttons;abort();
}
bool settingsui_is_open(void) { abort(); }
bool diskui_is_open(void) { abort(); }
bool settingsui_handle_key(int key,bool down) { (void)key;(void)down;abort(); }
bool diskui_handle_key(int key,bool down) { (void)key;(void)down;abort(); }
static EFI_USB_IO_PROTOCOL usb;
static unsigned opens,closes,starts,cancels,clears;
static int fail_start,no_endpoint;
static EFI_STATUS handle(EFI_HANDLE h,const EFI_GUID *g,void **p) { (void)h;(void)g;*p=&usb;return 0; }
static EFI_STATUS openp(EFI_HANDLE h,const EFI_GUID *g,void **p,EFI_HANDLE a,EFI_HANDLE c,uint32_t f) {
    (void)h;(void)g;(void)a;(void)c;(void)f; opens++;*p=&usb;return 0;
}
static EFI_STATUS closep(EFI_HANDLE h,const EFI_GUID *g,EFI_HANDLE a,EFI_HANDLE c) {
    (void)h;(void)g;(void)a;(void)c;closes++;return 0;
}
static EFI_STATUS install(EFI_HANDLE *h,const EFI_GUID *g,uint32_t t,void *p) { (void)h;(void)g;(void)t;(void)p;return 0; }
static EFI_STATUS iface(EFI_USB_IO_PROTOCOL *u,USB_INTERFACE_DESCRIPTOR *d) {
    (void)u;memset(d,0,sizeof(*d));d->InterfaceClass=3;d->InterfaceSubClass=1;
    d->InterfaceProtocol=1;d->NumEndpoints=no_endpoint?0:1;return 0;
}
static EFI_STATUS endpoint(EFI_USB_IO_PROTOCOL *u,uint8_t i,USB_ENDPOINT_DESCRIPTOR *d) {
    (void)u;(void)i;memset(d,0,sizeof(*d));d->EndpointAddress=0x81;
    d->Attributes=3;d->MaxPacketSize=8;d->Interval=10;return 0;
}
static EFI_STATUS control(EFI_USB_IO_PROTOCOL *u,USB_DEVICE_REQUEST *r,uint32_t dir,
                         uint32_t timeout,void *p,uint64_t n,uint32_t *status) {
    (void)u;(void)dir;(void)timeout;*status=0;
    if (r->RequestType==2 && r->Request==1 && r->Index==0x81) clears++;
    if (r->Request==3 && p && n==1) *(uint8_t *)p=0;return 0;
}
static EFI_STATUS transfer(EFI_USB_IO_PROTOCOL *u,uint8_t ep,bool start,uint64_t interval,
                          uint64_t len,USB_ASYNC_CB cb,void *ctx) {
    (void)u;(void)ep;(void)interval;(void)len;(void)cb;(void)ctx;
    if (start) { starts++;return fail_start?EFI_DEVICE_ERROR:0; } cancels++;return 0;
}
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n",__LINE__); return 1; } } while (0)
int main(void) {
    static EFI_BOOT_SERVICES bs;
    bs.HandleProtocol=handle;bs.OpenProtocol=openp;bs.CloseProtocol=closep;bs.InstallProtocolInterface=install;gBS=&bs;
    usb.UsbGetInterfaceDescriptor=iface;usb.UsbGetEndpointDescriptor=endpoint;
    usb.UsbControlTransfer=control;usb.UsbAsyncInterruptTransfer=transfer;
    no_endpoint=1;attach((void *)1);CHECK(opens==closes && ndevs==0);
    no_endpoint=0;fail_start=1;attach((void *)1);CHECK(opens==closes && ndevs==0);
    fail_start=0;attach((void *)1);CHECK(ndevs==1 && opens==closes+1);
    hid_dev_t *d=&devs[0];unsigned before=starts;
    recover_transfer(d);CHECK(starts==before); // idle never restarts
    kbd_cb(NULL,0,d,2);CHECK(d->transfer_error==2);
    recover_transfer(d);CHECK(starts==before+1 && clears==1 && !d->transfer_error);
    fail_start=1;kbd_cb(NULL,0,d,4);recover_transfer(d);CHECK(d->transfer_error==4);
    fail_start=0;recover_transfer(d);CHECK(!d->transfer_error);
    binding_stop(&binding,(void *)1,0,NULL);CHECK(ndevs==0 && opens==closes);
    for (int i=0;i<100;i++) { attach((void *)1);CHECK(ndevs==1);binding_stop(&binding,(void *)1,0,NULL); }
    CHECK(ndevs==0 && opens==closes && !devs[0].handle);
    puts("PASS: idle preserved, STALL recovery, retry, failed attach cleanup, 100 reconnects balanced");return 0;
}
