/* Actual USB HID module, mocked UI/PS2: monitor chords must not strand guest
 * modifiers or open Start. No firmware or privileged host instructions run. */
#ifndef USBHID_SOURCE
#define USBHID_SOURCE "../../efi/platform/usbhid.c"
#endif
#include USBHID_SOURCE
#include <stdlib.h>

EFI_BOOT_SERVICES *gBS;
EFI_HANDLE gImage;
uint8_t g_kbd_typematic;
static uint64_t clock_us = 100;
uint64_t wyse_ticks_us(void) { return clock_us; }
static bool settings_open, disks_open;
static unsigned menu_events;
bool settingsui_is_open(void) { return settings_open; }
bool diskui_is_open(void) { return disks_open; }
bool settingsui_handle_key(int key, bool down) {
    (void)key; (void)down; menu_events++; return true;
}
bool diskui_handle_key(int key, bool down) {
    (void)key; (void)down; menu_events++; return true;
}
static struct { int down, key; } sent[128];
static unsigned sent_count;
void ps2_put_keycode(PS2KbdState *state, int down, int key) {
    (void)state;
    if (sent_count >= sizeof(sent) / sizeof(sent[0])) abort();
    sent[sent_count].down = down;
    sent[sent_count++].key = key;
}
void ps2_mouse_event(PS2MouseState *state, int x, int y, int z, int buttons) {
    (void)state; (void)x; (void)y; (void)z; (void)buttons; abort();
}
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static void key(unsigned code, bool down) {
    push((hid_event_t){.kind=0, .code=(uint8_t)code, .down=(uint8_t)down});
}
static int event(unsigned index, int down, int code) {
    return index < sent_count && sent[index].down == down && sent[index].key == code;
}

int main(void) {
    PC pc = {0};
    hid_dev_t keyboard = {0};
    uint8_t report[8] = {0};

    /* Reproduce real callback ordering: modifiers precede ordinary key
     * releases. The menu opens after the hotkey poll returns. */
    report[0] = 0x08; report[2] = 0x45;
    kbd_cb(report, sizeof(report), &keyboard, 0);
    CHECK(usbhid_poll(&pc) == 0x45);
    disks_open = true;
    memset(report, 0, sizeof(report));
    kbd_cb(report, sizeof(report), &keyboard, 0);
    CHECK(usbhid_poll(&pc) == 0);
    CHECK(sent_count == 0); /* Old code leaves the guest's Win make held. */
    disks_open = false;

    /* A whole chord may also arrive before the UI starts running. */
    key(0xe7, true); key(0x44, true); key(0xe7, false); key(0x44, false);
    CHECK(usbhid_poll(&pc) == 0x44);
    CHECK(sent_count == 0);

    /* Win alone keeps its ordinary guest Start behavior on release. */
    key(0xe3, true); CHECK(usbhid_poll(&pc) == 0); CHECK(sent_count == 0);
    key(0xe3, false); usbhid_poll(&pc);
    CHECK(sent_count == 2 && event(0, 1, 125) && event(1, 0, 125));
    key(0xe7, true); key(0xe7, false); usbhid_poll(&pc);
    CHECK(sent_count == 4 && event(2, 1, 126) && event(3, 0, 126));
    sent_count = 0;

    /* Ctrl/Shift/Alt pressed in the guest must release through either UI. */
    for (unsigned right = 0; right < 2; right++) {
        unsigned first = right ? 0xe4 : 0xe0;
        int ctrl = right ? 97 : 29, shift = right ? 54 : 42, alt = right ? 100 : 56;
        key(first, true); key(first + 1, true); key(first + 2, true);
        usbhid_poll(&pc);
        CHECK(sent_count == 3 && event(0, 1, ctrl) && event(1, 1, shift) && event(2, 1, alt));
        settings_open = !right; disks_open = right != 0;
        key(first, false); key(first + 1, false); key(first + 2, false);
        usbhid_poll(&pc);
        CHECK(sent_count == 6 && event(3, 0, ctrl) && event(4, 0, shift) && event(5, 0, alt));
        settings_open = disks_open = false;
        sent_count = 0;
    }

    /* A modifier only pressed in UI does not produce a guest release. */
    settings_open = true; key(0xe1, true); usbhid_poll(&pc);
    settings_open = false; key(0xe1, false); usbhid_poll(&pc);
    CHECK(sent_count == 0 && menu_events != 0);
    disks_open = true; key(0xe3, true); usbhid_poll(&pc);
    disks_open = false; key(0xe3, false); usbhid_poll(&pc);
    CHECK(sent_count == 0);

    /* A normal key already held before Win still gets its release. */
    key(0x06, true); usbhid_poll(&pc); /* C -> Linux keycode 46 */
    key(0xe3, true); key(0x06, false); key(0x45, true);
    CHECK(usbhid_poll(&pc) == 0x45);
    key(0xe3, false); key(0x45, false); usbhid_poll(&pc);
    CHECK(sent_count == 2 && event(0, 1, 46) && event(1, 0, 46));
    sent_count = 0;

    /* Ordinary guest Ctrl+C remains balanced after leaving both menus. */
    key(0xe0, true); key(0x06, true); key(0x06, false); key(0xe0, false);
    usbhid_poll(&pc);
    CHECK(sent_count == 4 && event(0, 1, 29) && event(1, 1, 46) &&
          event(2, 0, 46) && event(3, 0, 29));
    puts("PASS: monitor Win chords isolated; Win alone preserved; guest modifiers released across menus");
    return 0;
}
