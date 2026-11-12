# Release history

Selected verified changes, not a reconstruction of every development commit.
See the [documentation](docs/index.md) for current usage and architecture.

## v1.1.16 - 2026-11-13

- Report the allocated display surface capacity to the VGA BIOS, allowing
  Windows to enumerate modes above the initial 640x480 desktop.
- Correct Velocity9x's status panel to identify the discovered WGPU render
  channel and its hardware Direct3D default instead of generic VGA metadata.
  Include the complete G5k driver and corresponding sources.
- Enable Voodoo2, the 64 MB Windows accelerator, and both CPU Boost switches
  consistently in firmware and installation defaults. Preserve explicit
  saved choices during upgrades.

## v1.1.15 - 2026-11-12

- Add Windows 98 Direct3D, Glide and OpenGL through the Intel Gen8 WGPU
  channel and the bundled Velocity9x driver, with corresponding GPL sources,
  licenses and installer integration.
- Add DOS Voodoo2 Glide support with 8/12 MB memory and independent Windows
  GPU workspace settings. Provide CPU Boost switches for DOS Glide and
  Windows 3D, without a separate Windows program or utility on C:.
- Suspend the drained Windows GPU ring in DOS and restore its state on the
  next request, fixing the Tomb Raider slowdown after GPU use.
- Include measured Tomb Raider, NFS III DirectX/Glide and Quake II OpenGL
  recordings in the existing game showcase. Repeat Tomb Raider at 21.71 FPS
  and NFS III on the original DX5 path at 29.59 FPS; document sample limits.
- Remove obsolete FastVoodoo2, MesaFX and VMDisp9x packages.

## v1.1.14 - 2026-11-06

- Repair the VGA BIOS mode cache after temporary graphics-mode DOS startup,
  allowing automatic CLS to clear the complete text page instead of leaving
  old text underneath subsequent output.
- Preserve IF/DF around the real-mode VBE bank callback, fixing Paranoia's
  startup hang. Keep cleared VBE frames owned by software across both native
  fetch paths and display-core handoff, removing the stale startup image.
- Budget native guest instructions and cycles without charging monitor work;
  bound short-loop credit, fixing Lotus III's startup divide overflow.
- Use the software VGA window for 16-color planar graphics while retaining
  native CPU execution, text and existing chain4/Mode X choices. NOID's
  building-entry and automatic info transitions are responsive.
- Preserve timestamped direct PC-speaker bit writes across burst mixing,
  restoring NOID's laughter samples; retain ordinary PIT tones and mute.
  Respect PIT2's output for gate-low samples, including terminal-high mode 0
  inherited after Lotus III, so launching another game does not silence them.
- Qualify NOID's fast countdown and Settlers II Gold's brief old-title/new-
  palette transition against independent originals. Do not slow global clocks
  or blank game frames to hide those original behaviors.
- Add actual-source speaker, CPU/VGA and actual-ROM regression coverage.

## v1.1.13 — 2026-11-05

- Keep Wyse Sound Blaster auto-init DMA completion and its raw transfer cursor
  associated until the guest acknowledges the interrupt. Prevents copying a
  buffer while Blood's mixer clears/refills it; retains 250 ms recovery.
- Qualify the minimal build with matched Blood voice recordings and user
  listening confirmation. User also confirms Tyrian runs correctly.
- Add actual-code DMA completion/acknowledgment, timeout, ring-wrap and PCM
  rate regressions; run Duke3D and Doom demo playback without starvation.
- Keep raw DMA register behavior and producer-only DREQ ownership unchanged.

## v1.1.12 — 2026-11-05

- Clarify the upgrade confirmation on two lines and simplify the completion
  instruction to “Remove USB and press any key to reboot”.

## v1.1.11 — 2026-11-05

- Offer Upgrade alongside New install when a supported native eMMC installation
  is detected. Upgrade replaces firmware runtime files, preserving C:, settings,
  layout and the boot entry; clean installation retains a separate erase prompt.
- Stage and read back the complete firmware set, activate EFI last and journal
  activation for retry after interruption. Test layout rejection, preservation,
  I/O failures and interrupted update steps.
- Fix VBE protected-mode page addressing in Blood and use byte-safe physical VGA
  accesses for text mode, preventing corrupted saved/restored Setup dialogs.

## v1.1.10 — 2026-11-05

- Route RT5672 headphone playback through its powered volume stages instead
  of the direct DAC bypass. Headphones now changes analog loudness independently
  of master Volume and HDMI/DisplayPort PCM.
- Read back headphone volume writes and report failures.
- Add real codec-initialization/I2C regression coverage, including a failing
  original-bypass control, and document hardware listening/HDMI measurements.

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
