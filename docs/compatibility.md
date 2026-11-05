# Compatibility and limits

[Documentation index](index.md)

Observations through v1.1.8 on the Wyse 3040 are not complete certification.
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
| Monster Bash | Intro and scrolling demo after completed-front-page snapshot correction |
| Commander Keen 4 | Clean intro and attract gameplay; wide intro uses software VGA, gameplay returns to native |
| Prince of Persia | Walking, crouching and jumping checked with VGA changes |
| Teenagent | Braking intro PCM corrected through DMA ownership; capture and direct listening confirmed |
| One Must Fall | Menu sound/animation tempo checked with display-audio clock correction |
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

These are not fresh v1.1.8 benchmarks or performance guarantees. Comparisons
need matching game versions, scenes, detail, CPU profile and sound settings.

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
