# Compatibility and limits

[Documentation index](index.md)

Observations through v1.1.14 on the Wyse 3040 are not complete certification.
Many tests used 16 MB guest RAM and native eMMC. CPU profile, guest drivers
and physical output matter.

## Systems and devices

| Area | Evidence and scope |
|---|---|
| DOS | Boot/file access from image and native storage; DOS 7.1 FAT32 USB read/write |
| Windows 98 FE | Installation, VMDisp9x, USB read/write and orderly Stop/Eject with a separately supplied driver |
| Windows 98 SE | Installed system, VMDisp9x 800x600/32-bit desktop and input timing |
| Windows 95 / Me | Model/driver support does not establish equivalent full OS qualification |
| Power | Normal Windows shutdown reached physical power-off; DPMS blanks guest pixels/cursor, retaining the host signal |
| HID | Physical keyboard/mouse with PicoHID; KVM reconnect and long-idle PS/2 correction; possible enumeration pauses remain |
| USB floppy | Physical 1.44 MB boot/read/write; separate from flash and image ownership paths |
| USB CD-ROM | Physical USB CD drive with a real disc supported and tested; presented to the guest as an ATAPI CD-ROM |

## Games and programs

| Title | Recorded observation |
|---|---|
| Doom, Quake, Duke Nukem 3D, Tyrian | Demo/gameplay showcase with sound; measurements below |
| Blood 1.21 / Setup 1.10 | 640x480/8-bit attract gameplay and menus; clean nested text dialogs after VBE address and byte-safe aperture fixes; 32 MB RAM |
| Monster Bash | Intro and scrolling demo after completed-front-page snapshot correction |
| Commander Keen 4 | Clean intro and attract gameplay; wide intro uses software VGA, gameplay returns to native |
| Prince of Persia | Walking, crouching and jumping checked with VGA changes |
| Teenagent | Braking intro PCM corrected through DMA ownership; capture and direct listening confirmed |
| One Must Fall | Menu sound/animation tempo checked with display-audio clock correction |
| Avoid the Noid | EGA gameplay, building entry and automatic info transitions; direct PC-speaker laughter restored and heard; Pentium/166, 32 MB, native eMMC, software 16-color VGA window |
| Lotus III | Startup without divide overflow, original manual-code acceptance, menus and actual driving; Pentium/166, 32 MB, native eMMC |
| Paranoia | Startup hang and stale-image flash corrected; intro, story, map/gameplay and menu reached with the patched VGA BIOS and monitor |
| Settlers II Gold 1.51 | VBE splash/menu reached; brief old-title/new-menu-palette transition independently reproduced in the original on DOS Zone, not hidden by a monitor workaround |
| M602 | Native-disk file manager, input and idle/screen-saver use |
| Lemmings | Historical raster-palette work; latest VGA regression reached menus, not newly qualified gameplay |

## Measurements

The README showcase uses 720p60 capture with sound. Capture frame rate is not
game frame rate. Separate historical measurements were:

| Test | Result | Conditions |
|---|---|---|
| Doom demo3 | 64.50 FPS | 320x200, high detail, unlocked timedemo, sound off |
| Quake demo1 | 62.4 / 16.6 FPS | 320x200 / 640x480, software renderer, sound off |
| Duke3D DNRATE | Scene-dependent | Counter in the separately marked final showcase scene |
| Tyrian | 31.2 FPS | Early-level debug sample, normal speed/low detail; not an unlocked benchmark |

These are historical benchmarks, not fresh v1.1.14 performance guarantees. Comparisons
need matching game versions, scenes, detail, CPU profile and sound settings.

NOID's countdown also runs faster than wall time in the original DOSBox
reference. A no-input Wyse interval lost 47 displayed seconds over 20.42 real
seconds; the reference lost 11 over 5.11 seconds measured from its own BIOS
ticks, avoiding the browser's background slowdown. The user confirmed the
fast countdown in the original. Global PIT/audio clocks were not retuned.
A temporary 386/16 test made the game excessively slow and was withdrawn;
the Pentium/166 test profile was restored. A possible slight slowing of the
laughter remains a subjective observation, not a measured pitch correction.

## Limits and open verification

- CPU profiles approximate execution rates, not instruction/bus cycles.
- Firmware USB enumeration can pause input. Orderly Stop/Eject has stronger
  qualification than abrupt removal with open files or DOS reconnect.
- External HDD-backed image-source combinations need more hardware coverage
  than their local lifecycle tests provide. This concerns image-file sources,
  not the tested physical USB CD-ROM path.
- Snapshot rendering is not cycle-accurate CRT scanout; arbitrary visible-page
  writes can exceed its coherence guarantees.
- Older local notes report Epic Pinball graphics and Blockout background
  artifacts. These remain unqualified reports pending reproduction/retest.
- Other PCs, USB devices, adapters and displays need independent qualification.

New entries should name OS/game version, CPU/RAM, guest driver, graphics path,
media backend and the actions actually exercised.
