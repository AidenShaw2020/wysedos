# Display audio clock and CPU speed

The HDMI/DisplayPort audio engine paces the mixer and Sound Blaster PCM.
Its rate must remain 48 kHz regardless of the framebuffer's resolution.
Some games also advance animation or music from sound-card interrupts, so
a wrong output clock can accelerate both sound and gameplay.

The previous HDMI code always calculated CTS for a 148.5 MHz pixel clock
(1920x1080 at 60 Hz). On the tested direct monitor, firmware instead selected
3840x1080 with an approximately 269 MHz pixel clock. The resulting audio
consumption was about 87,000 frames/s rather than 48,000 frames/s. Changing
the CPU profile could not correct this independent clock error.

The audio driver now measures eight complete scanout frames against the TSC
wall clock and reads horizontal/vertical totals, including blanking, from
the active transcoder. HDMI CTS is calculated from that measured pixel
clock. DisplayPort link M/N is also combined with the measured clock rather
than assuming 1080p; the resulting link clock selects 1.62/2.7/5.4 Gbit/s
audio M/N parameters. A stopped or invalid scanout fails display-audio
initialization instead of silently assuming a different mode.

The selected firmware mode is used, not an EDID preferred-mode assumption.
The development command `emmcupd keepconfig` updates the firmware and ROMs
while leaving the installed configuration byte-for-byte unchanged.
Cherry Trail pipe C uses separate pipe/transcoder offsets (74000/63000).
Register references: Linux i915
[display device offsets](https://github.com/torvalds/linux/blob/v6.6/drivers/gpu/drm/i915/display/intel_display_device.c),
[register definitions](https://github.com/torvalds/linux/blob/v6.6/drivers/gpu/drm/i915/i915_reg.h)
and [frame-counter use](https://github.com/torvalds/linux/blob/v6.6/drivers/gpu/drm/i915/display/intel_vblank.c).

## Validation on 2026-11-04

- Regression: `clang -O2 tools/tests/lpe_clock.c -o work/lpe_clock_test.exe`.
  Simulated 1080p, 3840x1080, 4K30/60 and 59.94 Hz timing on all three pipes;
  frame-counter wrap, stopped-pipe timeout and HDMI CTS calculations passed.
- HDMI grabber at 1920x1080: measured pixel clock 148547 kHz. A captured
  1000 Hz test tone measured 1000 Hz at 48 kHz capture rate.
- Direct monitor at 3840x1080: measured pixel clock 269034 kHz, CTS 269034,
  audio DMA consumption approximately 48,000 frames/s. The user confirmed
  OMF menu music and animation ran at the same correct tempo as the grabber,
  with the same Pentium 166 profile.
- Direct DisplayPort audio has not been physically retested with this change.

## CPU profiles

The VT-x limiter budgets retired instructions against wall time. Its nominal
MHz conversion is based on the Turbo Pascal delay-calibration loop for each
CPU generation. It does not reproduce the complete pipeline, caches,
memory/bus waits or instruction timings of the named historical CPU.

An independent DOS test runs an `INC/CMP/JB` loop for 18 BIOS timer ticks
(roughly one second), without sound, graphics or I/O in the loop. Measured:

| Profile | Completed loop iterations |
|---|---:|
| 386 / 16 MHz | 883,913 |
| Pentium / 60 MHz | 23,815,239 |
| Pentium / 166 MHz | 66,699,031 |

The two Pentium profiles differ by 2.80x versus the configured 2.77x ratio.
The short BIOS-tick window introduces start-phase variation. These results
verify that throttling is active; they do not establish cycle accuracy or
equivalence to physical machines for every workload.
