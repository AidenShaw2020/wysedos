# Release history

Selected verified changes, not a reconstruction of every development commit.
See the [documentation](docs/index.md) for current usage and architecture.

## v1.1.9 — 2026-11-05

- Save menu settings under `[wyseDOS]`; continue reading legacy `[X86Pi]`
  configurations. Existing values are retained when saving with the new monitor.
- Use `[wyseDOS]` in the default configuration and rebuilt installer.
- Expand installation, configuration, storage, architecture, compatibility,
  troubleshooting and build documentation, correcting earlier platform claims.
- Document tested physical USB CD-ROM support separately from ISO image sources.

## v1.1.8 — 2026-11-04

- Capture the completed front page for disjoint native VGA page flips and
  publish CRTC start/fine pan together. Fixes incomplete moving Bash objects.
- Include virtual pitch in planar safety selection. Keen 4's wide intro uses
  software VGA; ordinary gameplay returns to native VGA.
- Add page-lifetime and backend-selection regression coverage.

## v1.1.7 — 2026-11-04

- Keep Sound Blaster DMA requests owned by the device/controller side, avoiding
  stale mixer requests restarting a stopped PCM transfer.
- Qualify Teenagent's braking intro with diagnostic capture removed.

## v1.1.6

- Derive display-audio timing from actual scanout rather than a fixed pixel
  clock, correcting display-dependent sound/game acceleration.
- Add the development `emmcupd keepconfig` operation.

## v1.1.5

- Handle VBE/DPMS blanking consistently, including the text cursor, for Windows
  monitor-power saving.

## v1.1.4

- Retire inactive PS/2 byte-hold deadlines using unsigned elapsed time.
  Fix input stoppage after long idle periods while retaining the actual hold.

## Earlier foundations

UEFI startup, VT-x, legacy PC devices, bounded native eMMC, graphical installer,
native/software VGA, display and analog sound, NE2000 ISA PnP, guest USB storage,
physical USB floppy and exclusive external-image ownership preceded these
focused fixes. [USB qualification](docs/usb-validation.md) records detailed
conditions and remaining limits.
