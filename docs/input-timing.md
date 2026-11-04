# Input timing and cursor updates

The Wyse firmware's original hardware timer period was 54.9254 ms. USB mouse
reports arrived at about 125 Hz but were delivered to the guest in batches.
The monitor now requests a maximum hardware timer period of 5 ms at startup.
An already faster timer is preserved; rejection or an unavailable timer
protocol leaves the existing timer in use and is recorded in the log.

The `[pc]` setting `firmware_timer_us=5000` selects this period. `0` retains
the firmware timer. The setting is preserved when saving the configuration.
This changes runtime firmware timer delivery, not the guest PIT or the
machine's persistent BIOS settings.

Native writes to the VBE linear framebuffer do not pass through the emulated
VRAM write counter. Windows can draw a cursor and halt before the CPU slice
epilogue. The renderer therefore compares framebuffer rows whenever a native
mapping exists, including while the guest is idle. Only changed rows are
converted and copied to the display. `vga` reports `native_vram=1` for this path.

## Measurement on Wyse 3040

Windows 98 SE was tested at 800x600, 32-bit colour, from the native eMMC disk.
The shorter timer improved cursor movement. A subsequent idle-desktop cursor
redraw regression was reproduced by a renderer test and fixed independently.
Repeated KVM disconnection/reconnection restored input; enumeration still
caused a user-observed pause of about 3–4 seconds. Small occasional movement
hesitations remain a possible limitation; this release does not promise zero
latency or eliminate all firmware USB pauses.

During a reproduced slow vertical-motion hesitation, the signed movement
totals were conserved from HID input through PS/2 packets, both queues were
empty afterwards, all queued PS/2 bytes were consumed, and no discarded motion
or USB error was recorded. This excludes a counted queue loss in that sample;
it does not establish the timing of individual Windows cursor updates.

## Development diagnostics

With the development control port enabled, `hostirq` reads the active hardware
timer period in 100 ns units. `hid` also reports bounded cumulative `motion:`
counters: submitted events, merged events, ring peak/overflow, disabled input,
signed XY totals, PS/2 packet totals and bytes queued/consumed. Compare before
and after samples once movement and queues stop. PS/2 Y has the opposite sign
to HID Y. Signed totals alone cannot prove that an intermediate trajectory
was displayed; use them together with event/drop counters and visual checks.

Regression coverage includes unchanged/faster/rejected firmware timers,
native VRAM writes followed by guest inactivity, unchanged-row skipping, HID
coalescing and overflow accounting, complete PS/2 movement under backpressure,
and individual positive/negative vertical steps.
