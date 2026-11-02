// USB keyboards and mice, read raw through the firmware's USB stack.
//
// UEFI's own keyboard driver only reports keystrokes, never releases, which is
// useless for games. So each boot-protocol HID interface is taken over (an
// EXCLUSIVE open disconnects the firmware driver), put into boot protocol and
// read with an asynchronous interrupt transfer. The callback runs in the
// firmware's timer context and only queues events; usbhid_poll() feeds them
// to the PC model from the main loop.

#include <string.h>
#include <stdio.h>
#include "pc.h"
#include "settingsui.h"
#include "diskui.h"
#include "../include/efi.h"

extern EFI_BOOT_SERVICES *gBS;
extern EFI_HANDLE gImage;

//--------------------------------------------------------------------+
// event ring: filled by the transfer callbacks, drained by the main loop
//--------------------------------------------------------------------+

typedef struct {
    uint8_t kind;       // 0 key, 1 mouse
    uint8_t down;
    uint8_t code;       // HID usage (key) / buttons (mouse)
    int16_t dx, dy, dz;
    uint32_t t;         // microseconds when the report came
} hid_event_t;

#define RING 256
static hid_event_t ring[RING];
static volatile uint32_t ring_head, ring_tail;

uint64_t wyse_ticks_us(void);
static uint32_t lat_max, lat_sum, lat_n;     // report -> guest, this second

static void push(hid_event_t e) {
    e.t = (uint32_t)wyse_ticks_us();
    uint32_t next = (ring_head + 1) % RING;
    if (next == ring_tail) return;
    ring[ring_head] = e;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ring_head = next;
}

typedef struct {
    EFI_HANDLE volatile handle; // NULL: a free slot
    EFI_USB_IO_PROTOCOL *io;
    bool mouse;
    uint8_t ep;
    uint8_t interval;
    uint16_t packet;
    volatile uint32_t transfer_error; // restart from the main loop, never the callback
    uint8_t prev[8];
    // the first reports as they came, for the log (usbhid_scan prints them)
    uint8_t raw[4][9];
    volatile uint32_t reports, errors;
    uint32_t raw_shown;
    // when reports come: how many this second and the longest gap between two
    volatile uint32_t cb_n, cb_gap_max;
    uint64_t cb_last;
} hid_dev_t;

static hid_dev_t devs[8];
static volatile int ndevs;
static volatile uint32_t g_usbhid_gone;
static volatile uint8_t mods_now;   // current modifier byte, for host hotkeys

static void keep_raw(hid_dev_t *d, const uint8_t *r, uint64_t len) {
    uint64_t now = wyse_ticks_us();
    if (d->cb_last && now - d->cb_last > d->cb_gap_max) d->cb_gap_max = (uint32_t)(now - d->cb_last);
    d->cb_last = now;
    d->cb_n++;
    uint32_t n = d->reports++;
    if (n >= 4) return;
    d->raw[n][0] = (uint8_t)len;
    memcpy(d->raw[n] + 1, r, len < 8 ? len : 8);
}

static EFI_STATUS kbd_cb(void *data, uint64_t len, void *ctx, uint32_t status) {
    hid_dev_t *d = ctx;
    const uint8_t *r = data;
    if (status) { d->errors++; d->transfer_error |= status; return EFI_SUCCESS; }
    if (!r || len < 8) return EFI_SUCCESS;
    // (the firmware hands over the length asked for, not the length received)
    keep_raw(d, r, len);
    if (r[2] == 1) return EFI_SUCCESS;     // rollover error report
    uint8_t changed = r[0] ^ d->prev[0];
    for (int b = 0; b < 8; b++)
        if (changed & (1 << b)) push((hid_event_t){0, (r[0] >> b) & 1, (uint8_t)(0xe0 + b), 0, 0, 0});
    for (int i = 2; i < 8; i++) {           // released
        uint8_t k = d->prev[i];
        if (k > 3 && !memchr(r + 2, k, 6)) push((hid_event_t){0, 0, k, 0, 0, 0});
    }
    for (int i = 2; i < 8; i++) {           // pressed
        uint8_t k = r[i];
        if (k > 3 && !memchr(d->prev + 2, k, 6)) push((hid_event_t){0, 1, k, 0, 0, 0});
    }
    mods_now = r[0];
    memcpy(d->prev, r, 8);
    return EFI_SUCCESS;
}

//--------------------------------------------------------------------+
// Mice in the report protocol. Some do not switch to the boot protocol
// however they are asked - one here answered GET_PROTOCOL with "boot" and
// went on sending report ID 1, buttons, then 16-bit X and Y. Read as a boot
// report, its X went nowhere, Y came from X, and the ID held the left button
// down for good. So a mouse's report descriptor is read, it is put in the
// report protocol, and its reports are taken apart by what the descriptor says.
//--------------------------------------------------------------------+

typedef struct {
    bool valid;
    uint8_t id;                 // 0: the device uses no report IDs
    uint16_t btn_off, btn_n;    // bit offsets after the ID byte
    uint16_t x_off, x_size, y_off, y_size, w_off, w_size;
} mouse_layout_t;

static mouse_layout_t layouts[8];

static void parse_mouse_descriptor(const uint8_t *d, int n, mouse_layout_t *L) {
    uint16_t page = 0, size = 0, count = 0;
    int id = 0, off = 0, found_id = -1;
    uint32_t usages[16];
    int nusages = 0;
    uint32_t umin = 0, umax = 0;
    memset(L, 0, sizeof(*L));
    for (int i = 0; i < n;) {
        uint8_t b = d[i];
        if (b == 0xfe) { i += 3 + (i + 1 < n ? d[i + 1] : 0); continue; }     // long item
        int len = (b & 3) == 3 ? 4 : b & 3;
        if (i + 1 + len > n) break;
        uint32_t v = 0;
        for (int k = 0; k < len; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        int type = (b >> 2) & 3, tag = b >> 4;
        i += 1 + len;
        if (type == 1) {                                    // global
            if (tag == 0) page = (uint16_t)v;
            else if (tag == 7) size = (uint16_t)v;
            else if (tag == 9) count = (uint16_t)v;
            else if (tag == 8) {                            // report ID: a new report
                id = (int)v;
                off = 0;
            }
        } else if (type == 2) {                             // local
            if (tag == 0 && nusages < 16) usages[nusages++] = v & 0xffff;
            else if (tag == 1) umin = v;
            else if (tag == 2) umax = v;
        } else if (type == 0) {                             // main
            if (tag == 8) {                                 // input
                bool constant = v & 1;
                if (!constant && (found_id < 0 || found_id == id)) {
                    if (page == 9 && !L->btn_n) {
                        L->btn_off = (uint16_t)off;
                        L->btn_n = count > 8 ? 8 : count;
                        found_id = id;
                    } else if (page == 1) {
                        for (int k = 0; k < count; k++) {
                            uint32_t u = k < nusages ? usages[k] : umin ? umin + k : nusages ? usages[nusages - 1] : 0;
                            uint16_t at = (uint16_t)(off + k * size);
                            if (u == 0x30 && !L->x_size) { L->x_off = at; L->x_size = size; found_id = id; }
                            else if (u == 0x31 && !L->y_size) { L->y_off = at; L->y_size = size; }
                            else if (u == 0x38 && !L->w_size) { L->w_off = at; L->w_size = size; }
                        }
                    }
                }
                off += size * count;
            }
            nusages = 0;
            umin = umax = 0;
        }
    }
    L->id = found_id > 0 ? (uint8_t)found_id : 0;
    L->valid = L->x_size && L->y_size && L->x_size <= 16 && L->y_size <= 16;
}

static int field(const uint8_t *p, unsigned off, unsigned size) {
    if (!size) return 0;
    uint32_t v = 0;
    for (unsigned k = 0; k < size; k++)
        if (p[(off + k) >> 3] >> ((off + k) & 7) & 1) v |= 1u << k;
    if (size < 32 && (v >> (size - 1) & 1)) v |= ~0u << size;     // signed
    return (int)v;
}

static EFI_STATUS mouse_cb(void *data, uint64_t len, void *ctx, uint32_t status) {
    hid_dev_t *d = ctx;
    const uint8_t *r = data;
    if (status) { d->errors++; d->transfer_error |= status; return EFI_SUCCESS; }
    if (!r || len < 3) return EFI_SUCCESS;
    keep_raw(d, r, len);
    const mouse_layout_t *L = &layouts[d - devs];
    if (L->valid) {
        if (L->id) {
            if (r[0] != L->id) return EFI_SUCCESS;          // another report of the device
            r++;
        }
        uint8_t buttons = 0;
        for (unsigned b = 0; b < L->btn_n; b++)
            if (r[(L->btn_off + b) >> 3] >> ((L->btn_off + b) & 7) & 1) buttons |= (uint8_t)(1u << b);
        push((hid_event_t){1, 0, buttons, (int16_t)field(r, L->x_off, L->x_size), (int16_t)field(r, L->y_off, L->y_size),
                           (int16_t)field(r, L->w_off, L->w_size)});
        return EFI_SUCCESS;
    }
    push((hid_event_t){1, 0, r[0], (int8_t)r[1], (int8_t)r[2], len > 3 ? (int8_t)r[3] : 0});
    return EFI_SUCCESS;
}

// The monitor as a UEFI driver, only so that the USB bus driver can take a
// keyboard back when it is pulled out: it removes the interface through
// DisconnectController, which calls Stop() of whoever holds it BY_DRIVER. An
// application's EXCLUSIVE open instead makes that removal fail, the dead
// device stays on its port, and a keyboard plugged in there later is never
// enumerated - the hang a user saw after replugging the keyboard.
static const EFI_GUID usbio_guid = EFI_USB_IO_PROTOCOL_GUID;

static EFI_STATUS binding_supported(EFI_DRIVER_BINDING_PROTOCOL *b, EFI_HANDLE c, void *rdp) {
    return EFI_UNSUPPORTED;     // nothing is connected to us; we take devices ourselves
}
static EFI_STATUS binding_start(EFI_DRIVER_BINDING_PROTOCOL *b, EFI_HANDLE c, void *rdp) {
    return EFI_UNSUPPORTED;
}
// A device that goes away lets go of what it held: keys and modifiers up, mouse
// buttons released. Otherwise the guest keeps a button pressed for good - a
// file manager then sits in a drag that never ends.
static void release_all(hid_dev_t *d) {
    if (d->mouse) {
        push((hid_event_t){1, 0, 0, 0, 0, 0});
        return;
    }
    for (int b = 0; b < 8; b++)
        if (d->prev[0] & (1 << b)) push((hid_event_t){0, 0, (uint8_t)(0xe0 + b), 0, 0, 0});
    for (int i = 2; i < 8; i++)
        if (d->prev[i] > 3) push((hid_event_t){0, 0, d->prev[i], 0, 0, 0});
    memset(d->prev, 0, sizeof(d->prev));
}

static EFI_STATUS binding_stop(EFI_DRIVER_BINDING_PROTOCOL *b, EFI_HANDLE c, uint64_t n, EFI_HANDLE *kids) {
    for (int i = 0; i < 8; i++) {
        hid_dev_t *d = &devs[i];
        if (d->handle != c) continue;
        d->io->UsbAsyncInterruptTransfer(d->io, d->ep, false, 0, 0, NULL, NULL);
        release_all(d);
        gBS->CloseProtocol(c, &usbio_guid, gImage, c);
        d->handle = NULL;
        ndevs--;
        g_usbhid_gone++;
    }
    return EFI_SUCCESS;
}

static EFI_DRIVER_BINDING_PROTOCOL binding = {binding_supported, binding_start, binding_stop, 0x10, NULL, NULL};

static bool install_binding(void) {
    static const EFI_GUID guid = EFI_DRIVER_BINDING_PROTOCOL_GUID;
    static int done;
    if (done) return done > 0;
    binding.ImageHandle = gImage;
    binding.DriverBindingHandle = gImage;
    EFI_HANDLE h = gImage;
    done = EFI_ERROR(gBS->InstallProtocolInterface(&h, &guid, EFI_NATIVE_INTERFACE, &binding)) ? -1 : 1;
    if (done < 0) printf("usbhid: no driver binding\n");
    return done > 0;
}

// Take a boot-protocol keyboard or mouse interface into a free slot
static void attach(EFI_HANDLE h) {
    EFI_USB_IO_PROTOCOL *io;
    USB_INTERFACE_DESCRIPTOR ifd;
    if (EFI_ERROR(gBS->HandleProtocol(h, &usbio_guid, (void **)&io))) return;
    if (EFI_ERROR(io->UsbGetInterfaceDescriptor(io, &ifd))) return;
    if (ifd.InterfaceClass != 3 || ifd.InterfaceSubClass != 1) return;
    if (ifd.InterfaceProtocol != 1 && ifd.InterfaceProtocol != 2) return;
    bool mouse = ifd.InterfaceProtocol == 2;
    hid_dev_t *d = NULL;
    for (int i = 0; i < 8 && !d; i++)
        if (!devs[i].handle) d = &devs[i];
    if (!d) return;

    // take it from the firmware's HID driver, as a driver (see binding_stop)
    uint32_t how = install_binding() ? EFI_OPEN_PROTOCOL_BY_DRIVER | EFI_OPEN_PROTOCOL_EXCLUSIVE
                                     : EFI_OPEN_PROTOCOL_EXCLUSIVE;
    if (EFI_ERROR(gBS->OpenProtocol(h, &usbio_guid, (void **)&io, gImage, how & EFI_OPEN_PROTOCOL_BY_DRIVER ? h : NULL,
                                    how))) {
        printf("usbhid: interface %u busy\n", ifd.InterfaceNumber);
        return;
    }
    uint8_t ep = 0;
    uint16_t maxp = 8;
    uint8_t interval = 10;
    for (uint8_t e = 0; e < ifd.NumEndpoints; e++) {
        USB_ENDPOINT_DESCRIPTOR epd;
        if (EFI_ERROR(io->UsbGetEndpointDescriptor(io, e, &epd))) continue;
        if ((epd.EndpointAddress & 0x80) && (epd.Attributes & 3) == 3) {
            ep = epd.EndpointAddress;
            maxp = epd.MaxPacketSize;
            interval = epd.Interval ? epd.Interval : 10;
            break;
        }
    }
    if (!ep) {
        gBS->CloseProtocol(h, &usbio_guid, gImage, how & EFI_OPEN_PROTOCOL_BY_DRIVER ? h : NULL);
        return;
    }
    // A mouse with a report descriptor that says where its buttons, X, Y and
    // wheel are goes into the report protocol and is read by it (see above);
    // keyboards, and mice without one, into the boot protocol, asked for until
    // the device says it is in it.
    uint32_t st;
    mouse_layout_t *L = &layouts[d - devs];
    memset(L, 0, sizeof(*L));
    if (mouse) {
        static uint8_t desc[512];
        USB_DEVICE_REQUEST get_desc = {0x81, 0x06, 0x2200, ifd.InterfaceNumber, sizeof(desc)};
        memset(desc, 0, sizeof(desc));
        if (!EFI_ERROR(io->UsbControlTransfer(io, &get_desc, EfiUsbDataIn, 300, desc, sizeof(desc), &st))) {
            int n = sizeof(desc);
            while (n > 0 && !desc[n - 1]) n--;
            parse_mouse_descriptor(desc, n, L);
            printf("usbhid: mouse descriptor %d bytes: %s, id %u, buttons %u at %u, x %u@%u, y %u@%u, wheel %u@%u\n", n,
                   L->valid ? "usable" : "not usable", L->id, L->btn_n, L->btn_off, L->x_size, L->x_off, L->y_size,
                   L->y_off, L->w_size, L->w_off);
        }
    }
    uint8_t want_proto = L->valid ? 1 : 0;
    uint8_t proto = 0xff;
    EFI_STATUS ps = EFI_SUCCESS;
    for (int attempt = 0; attempt < 3 && proto != want_proto; attempt++) {
        USB_DEVICE_REQUEST set_protocol = {0x21, 0x0b, want_proto, ifd.InterfaceNumber, 0};
        ps = io->UsbControlTransfer(io, &set_protocol, EfiUsbNoData, 200, NULL, 0, &st);
        USB_DEVICE_REQUEST get_protocol = {0xa1, 0x03, 0, ifd.InterfaceNumber, 1};
        if (EFI_ERROR(io->UsbControlTransfer(io, &get_protocol, EfiUsbDataIn, 200, &proto, 1, &st))) proto = 0xfe;
    }
    USB_DEVICE_REQUEST set_idle = {0x21, 0x0a, 0, ifd.InterfaceNumber, 0};
    io->UsbControlTransfer(io, &set_idle, EfiUsbNoData, 200, NULL, 0, &st);

    memset(d, 0, sizeof(*d));
    d->io = io;
    d->mouse = mouse;
    d->ep = ep;
    // the whole packet: asked for less, a longer report overruns the transfer
    uint16_t want = maxp > 64 ? 64 : maxp < 8 && !mouse ? 8 : maxp;
    d->interval = interval;
    d->packet = want;
    EFI_STATUS s = io->UsbAsyncInterruptTransfer(io, ep, true, interval, want, mouse ? mouse_cb : kbd_cb, d);
    printf("usbhid: %s on interface %u, endpoint %02x, %u ms, packet %u, protocol %s (set %llx): %s\n",
           mouse ? "mouse" : "keyboard", ifd.InterfaceNumber, ep, interval, maxp,
           proto == 0 ? "boot" : proto == 1 ? "REPORT" : "unknown", (unsigned long long)ps,
           EFI_ERROR(s) ? "failed" : "ok");
    if (!EFI_ERROR(s)) {
        d->handle = h;
        ndevs++;
    } else {
        gBS->CloseProtocol(h, &usbio_guid, gImage, how & EFI_OPEN_PROTOCOL_BY_DRIVER ? h : NULL);
    }
}

/* USB interrupt errors can stop the firmware's recurring transfer (a KVM
 * switch can cause them without immediately removing the interface). Recover
 * on the next scan, after the bus has had time to settle. Idle input alone is
 * never a reason to reset a keyboard or mouse. */
static void recover_transfer(hid_dev_t *d) {
    uint32_t error = d->transfer_error;
    if (!error) return;
    d->transfer_error = 0;
    d->io->UsbAsyncInterruptTransfer(d->io, d->ep, false, 0, 0, NULL, NULL);
    release_all(d);
    if (error & 2) { // EFI_USB_ERR_STALL: clear ENDPOINT_HALT
        USB_DEVICE_REQUEST clear = {0x02, 1, 0, d->ep, 0};
        uint32_t status;
        d->io->UsbControlTransfer(d->io, &clear, EfiUsbNoData, 200, NULL, 0, &status);
    }
    EFI_STATUS s = d->io->UsbAsyncInterruptTransfer(d->io, d->ep, true,
        d->interval, d->packet, d->mouse ? mouse_cb : kbd_cb, d);
    if (EFI_ERROR(s)) d->transfer_error |= error;
    printf("usbhid: %s transfer recovery after %x: %s\n",
           d->mouse ? "mouse" : "keyboard", error, EFI_ERROR(s) ? "retry" : "ok");
}

// Keyboards and mice plugged in since the last look are taken; the slots of
// ones pulled out (their handle is gone) are freed. Without this a keyboard
// plugged in while the PC runs is never read, which looks like a hang.
static bool first_scan_done;     // devices there at start are taken at once

int usbhid_scan(void) {
    uint64_t count = 0;
    EFI_HANDLE *handles = NULL;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &usbio_guid, NULL, &count, &handles))) count = 0;
    static uint64_t last_count = ~0ull;
    if (count != last_count) printf("usbhid: %llu USB interfaces\n", (unsigned long long)count);
    last_count = count;
    static uint32_t gone_seen;
    if (g_usbhid_gone != gone_seen) {
        printf("usbhid: %u device(s) released to the USB bus driver\n", g_usbhid_gone - gone_seen);
        gone_seen = g_usbhid_gone;
    }
    for (int i = 0; i < 8; i++) {
        if (!devs[i].handle) continue;
        bool there = false;
        for (uint64_t k = 0; k < count && !there; k++) there = handles[k] == devs[i].handle;
        if (there) { recover_transfer(&devs[i]); continue; }
        printf("usbhid: %s gone\n", devs[i].mouse ? "mouse" : "keyboard");
        release_all(&devs[i]);
        devs[i].handle = NULL;
        ndevs--;
    }
    // a new device is taken on the scan after the one that first saw it: the
    // firmware sets it up for a moment after it appears, and taken in the
    // middle of that it can be left in the report protocol
    static EFI_HANDLE seen[16];
    static int nseen;
    EFI_HANDLE now_seen[16];
    int nnow = 0;
    for (uint64_t k = 0; k < count; k++) {
        bool known = false;
        for (int i = 0; i < 8 && !known; i++) known = devs[i].handle == handles[k];
        if (known) continue;
        bool before = !first_scan_done;
        for (int i = 0; i < nseen && !before; i++) before = seen[i] == handles[k];
        if (before) attach(handles[k]);
        else if (nnow < 16) now_seen[nnow++] = handles[k];
    }
    memcpy(seen, now_seen, sizeof(EFI_HANDLE) * (size_t)nnow);
    nseen = nnow;
    first_scan_done = true;
    // what the devices have sent so far
    for (int i = 0; i < 8; i++) {
        hid_dev_t *d = &devs[i];
        if (!d->handle) continue;
        while (d->raw_shown < d->reports && d->raw_shown < 4) {
            const uint8_t *r = d->raw[d->raw_shown++];
            printf("usbhid: %s report %u: %u bytes %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   d->mouse ? "mouse" : "keyboard", d->raw_shown, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
        }
        d->cb_n = 0;
        d->cb_gap_max = 0;
    }
    // the hand-over from a report to the guest takes 1-2 ms (measured with a
    // real keyboard and mouse); only one that took long goes to the log
    if (lat_n && lat_max > 20000)
        printf("usbhid: slow input, report to guest %u us on average, %u us at most\n", lat_sum / lat_n, lat_max);
    lat_n = lat_sum = lat_max = 0;
    if (handles) gBS->FreePool(handles);
    return ndevs;
}

int usbhid_init(void) { return usbhid_scan(); }

// "hid" on the control port: every slot, and what the USB I/O handles are
void usbhid_report(void) {
    uint64_t count = 0;
    EFI_HANDLE *handles = NULL;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &usbio_guid, NULL, &count, &handles))) count = 0;
    printf("usbhid: %d devices, %llu USB interfaces, %u released so far\n", ndevs, (unsigned long long)count,
           g_usbhid_gone);
    for (int i = 0; i < 8; i++) {
        hid_dev_t *d = &devs[i];
        if (!d->handle) continue;
        bool there = false;
        for (uint64_t k = 0; k < count && !there; k++) there = handles[k] == d->handle;
        uint64_t idle = d->cb_last ? (wyse_ticks_us() - d->cb_last) / 1000 : 0;
        printf("usbhid: slot %d %s ep %02x, %u reports, %u errors, last %llu ms ago, handle %s\n", i,
               d->mouse ? "mouse" : "keyboard", d->ep, d->reports, d->errors, (unsigned long long)idle,
               there ? "present" : "GONE");
    }
    for (uint64_t k = 0; k < count; k++) {
        bool known = false;
        for (int i = 0; i < 8 && !known; i++) known = devs[i].handle == handles[k];
        EFI_USB_IO_PROTOCOL *io;
        USB_INTERFACE_DESCRIPTOR ifd;
        if (known || EFI_ERROR(gBS->HandleProtocol(handles[k], &usbio_guid, (void **)&io)) ||
            EFI_ERROR(io->UsbGetInterfaceDescriptor(io, &ifd)))
            continue;
        printf("usbhid: untaken interface %u: class %u/%u/%u\n", ifd.InterfaceNumber, ifd.InterfaceClass,
               ifd.InterfaceSubClass, ifd.InterfaceProtocol);
    }
    if (handles) gBS->FreePool(handles);
}

// HID usage -> Linux/AT key code, as X86Pi's ps2_put_keycode() takes it
static unsigned char hid_to_linux_keycode(unsigned char code) {
    static const unsigned char tab[0x68] = {
        [0x04] = 30, [0x05] = 48, [0x06] = 46, [0x07] = 32, [0x08] = 18, [0x09] = 33, [0x0a] = 34, [0x0b] = 35,
        [0x0c] = 23, [0x0d] = 36, [0x0e] = 37, [0x0f] = 38, [0x10] = 50, [0x11] = 49, [0x12] = 24, [0x13] = 25,
        [0x14] = 16, [0x15] = 19, [0x16] = 31, [0x17] = 20, [0x18] = 22, [0x19] = 47, [0x1a] = 17, [0x1b] = 45,
        [0x1c] = 21, [0x1d] = 44, [0x1e] = 2, [0x1f] = 3, [0x20] = 4, [0x21] = 5, [0x22] = 6, [0x23] = 7,
        [0x24] = 8, [0x25] = 9, [0x26] = 10, [0x27] = 11, [0x28] = 28, [0x29] = 1, [0x2a] = 14, [0x2b] = 15,
        [0x2c] = 57, [0x2d] = 12, [0x2e] = 13, [0x2f] = 26, [0x30] = 27, [0x31] = 43, [0x33] = 39, [0x34] = 40,
        [0x35] = 41, [0x36] = 51, [0x37] = 52, [0x38] = 53, [0x39] = 58, [0x3a] = 59, [0x3b] = 60, [0x3c] = 61,
        [0x3d] = 62, [0x3e] = 63, [0x3f] = 64, [0x40] = 65, [0x41] = 66, [0x42] = 67, [0x43] = 68, [0x44] = 87,
        [0x45] = 88, [0x46] = 99, [0x47] = 70, [0x48] = 119, [0x49] = 110, [0x4a] = 102, [0x4b] = 104, [0x4c] = 111,
        [0x4d] = 107, [0x4e] = 109, [0x4f] = 106, [0x50] = 105, [0x51] = 108, [0x52] = 103, [0x53] = 69, [0x54] = 98,
        [0x55] = 55, [0x56] = 74, [0x57] = 78, [0x58] = 96, [0x59] = 79, [0x5a] = 80, [0x5b] = 81, [0x5c] = 75,
        [0x5d] = 76, [0x5e] = 77, [0x5f] = 71, [0x60] = 72, [0x61] = 73, [0x62] = 82, [0x63] = 83, [0x64] = 86,
    };
    static const unsigned char mods[8] = {29, 42, 56, 125, 97, 54, 100, 126};
    if (code >= 0xe0 && code <= 0xe7) return mods[code - 0xe0];
    return code < sizeof(tab) ? tab[code] : 0;
}

uint8_t usbhid_modifiers(void) { return mods_now; }
int hid_debug = 0;

// Typematic: the guest's F3h byte (i8042.c). Delay (1 + bits 5-6) x 250 ms;
// period (8 + bits 0-2) x 2^(bits 3-4) x 4.17 ms - 33 ms to 500 ms.
extern uint8_t g_kbd_typematic;
static unsigned char rep_key;
static uint64_t rep_at;

static uint64_t typematic_delay_us(void) { return (1u + ((g_kbd_typematic >> 5) & 3)) * 250000u; }
static uint64_t typematic_period_us(void) {
    return (8u + (g_kbd_typematic & 7)) * (1u << ((g_kbd_typematic >> 3) & 3)) * 4170u;
}

// Returns a host hotkey (HID usage pressed with the Windows key) or 0.
int usbhid_poll(PC *pc) {
    int hotkey = 0;
    while (ring_tail != ring_head) {
        hid_event_t e = ring[ring_tail];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        ring_tail = (ring_tail + 1) % RING;
        {
            uint32_t lat = (uint32_t)wyse_ticks_us() - e.t;
            if (lat > lat_max) lat_max = lat;
            lat_sum += lat;
            lat_n++;
        }
        if (e.kind == 1) {
            while (ring_tail != ring_head) {
                hid_event_t next = ring[ring_tail];
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                if (next.kind != 1 || (next.code & 7) != (e.code & 7)) break;
                int32_t nx = (int32_t)e.dx + next.dx;
                int32_t ny = (int32_t)e.dy + next.dy;
                int32_t nz = (int32_t)e.dz + next.dz;
                if (nx < -32767 || nx > 32767 || ny < -32767 || ny > 32767 || nz < -32767 || nz > 32767) break;
                e.dx = (int16_t)nx;
                e.dy = (int16_t)ny;
                e.dz = (int16_t)nz;
                ring_tail = (ring_tail + 1) % RING;
            }
            if (pc->mouse_enabled)
                ps2_mouse_event(pc->mouse, e.dx, e.dy, e.dz, e.code & 7);
            continue;
        }
        // the modifiers as they were at this event, not as they are now: a
        // quick Win+R arrives as press and release before the poll sees it
        static uint8_t mods;
        if (e.code >= 0xe0 && e.code <= 0xe7) {
            uint8_t bit = (uint8_t)(1 << (e.code - 0xe0));
            mods = e.down ? mods | bit : mods & ~bit;
        }
        // Win + key belongs to the monitor (menus), like X86Pi's Win+F11/F12
        if ((mods & 0x88) && e.code < 0xe0) {
            if (e.down) hotkey = e.code;
            continue;
        }
        unsigned char k = hid_to_linux_keycode(e.code);
        if (hid_debug) printf("hid key %02x %s -> %u\n", e.code, e.down ? "down" : "up", k);
        // the key that repeats while held: the last one pressed, as on a PS/2 keyboard
        if (e.code < 0xe0 && k) {
            if (e.down) {
                rep_key = k;
                rep_at = wyse_ticks_us() + typematic_delay_us();
            } else if (k == rep_key) {
                rep_key = 0;
            }
        }
        // an open menu takes the keyboard
        if (settingsui_is_open()) { if (k) settingsui_handle_key(k, e.down); continue; }
        if (diskui_is_open()) { if (k) diskui_handle_key(k, e.down); continue; }
        if (k) ps2_put_keycode(pc->kbd, e.down, k);
    }
    // Typematic repeat. A USB keyboard reports a held key once and leaves the
    // repeating to the computer; a PS/2 keyboard sends its make code again at
    // the rate the guest set (F3h, i8042.c). So it is sent again here.
    if (rep_key) {
        uint64_t now = wyse_ticks_us();
        if (now >= rep_at) {
            if (settingsui_is_open()) settingsui_handle_key(rep_key, 1);
            else if (diskui_is_open()) diskui_handle_key(rep_key, 1);
            else ps2_put_keycode(pc->kbd, 1, rep_key);
            rep_at += typematic_period_us();
            if (rep_at < now) rep_at = now + typematic_period_us();     // fell behind: no burst
        }
    }
    return hotkey;
}
