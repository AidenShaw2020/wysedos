# 3D preview recordings

Recorded on 2026-11-12 with the final G5l firmware and G5j Windows HAL,
128 MB guest RAM, 8 MB VGA memory, 12 MB Voodoo2 and a 64 MB WGPU workspace.
The base CPU setting is 133 MHz; each API activates the selected CPU Boost
automatically. No separate Windows booster program is installed.

These are scene samples, not whole-game averages or controlled comparisons
between the different renderers. Audio is enabled. The videos are 1280x720,
60 fps with captured digital audio; their container frame rate is not game FPS.
The capture pipeline can duplicate or drop capture frames. Playback is at
original speed. Game data is not distributed with the firmware or driver.

## Earlier baselines and comparison still required

Earlier hardware runs recorded **22.2 FPS** in the DOS Tomb Raider level-start
scene with automatic CPU Boost (2026-11-08), and **29.92 FPS** while driving
NFS III through the previously selected **DX5** path (601 Flips / 20.086 s,
G4j, 2026-11-11). These earlier results must not be replaced by the numbers
below as though they were equivalent repeat measurements.

The new NFS DirectX recording used **DX6**, rather than the earlier DX5 path.
Its 20.79 FPS does not establish a slowdown on DX5. The new Tomb Raider
measurement used a moving scene rather than the earlier stationary level-start
scene. Its lower result is real for that sample, but the size and cause of the
difference remain unresolved. A matched scene/configuration retest is required
before claiming either preserved performance or a firmware regression.

## Measurements in the new recordings

**Methodology audit:** the captions below report backend counters or the game's
rotation benchmark; they are not independently verified display FPS. The
shared MJPEG input was timestamped on a 1/25-second time base and converted
to a 60 fps output with dropped/duplicated frames. HTTP streaming also keeps
only the latest frame. These recordings cannot independently validate frame
cadence, even though average playback duration followed wall time.

DirectX counts handled Flips; it does not include presentation through a
DirectDraw fallback copy (`Blt`). Fallback activity was not measured in this
recording, so 20.79 Flips/s must not be equated unconditionally with all
presented frames/s. The Windows Glide census counts swap calls before the
device operation and does not check successful display completion. The DOS
Voodoo2 sample had matching deltas of 130 swap commands and 130 completed
buffer changes. Quake II's number is the separate game rotation benchmark.

The DirectX time denominator agrees between host wall time (20.105110 s) and
guest ticks (20.102 s). The DOS interval is 20.155 s with the host video start
within approximately 0.12 s of its first counter snapshot. No several-fold
time-denominator error was found. Pixel-change analysis is only a capture
cross-check: it cannot recover frames already lost by the capture pipeline
or distinguish every newly rendered static image.

A replacement comparison must use the earlier DX5 path and the same DOS
scene/configuration, direct capture with an explicitly negotiated frame rate,
synchronized host timestamps, and presentation/fallback counters. The current
MP4 captions remain the original, provisional counter-derived annotations.

| Recording | Game mode | Measurement |
| --- | --- | --- |
| [DOS Tomb Raider / Glide](05-tombraider-dos-glide.mp4) | 640x480, Voodoo2 emulation, CPU span renderer | 130 completed swaps / 20.155 s = **6.45 FPS**, moving sample |
| [Windows NFS III / DirectX](06-nfs3-directx.mp4) | 640x480, native DX6 renderer, Hometown / Corvette | 418 handled Flips / 20.102 guest seconds = **20.79 FPS**; host wall-time result 20.79 FPS |
| [Windows NFS III / Glide](07-nfs3-glide.mp4) | 640x480, Velocity9x Glide3, Hometown / Corvette | 247 swaps / 15.001 s = **16.47 FPS**, complete periodic interval inside the marked race sample |
| [Windows Quake II test / OpenGL](08-quake2-opengl.mp4) | 640x480, Velocity9x Intel Gen8 ICD | Separate opening-scene `timerefresh`: 128 views / 3.647994 s = **35.087779 FPS** |

The NFS clips retain medium graphics, mirror, VSync and sound. Their race
windows contain different geometry and are not a controlled API speed test.
Quake II's result applies to the opening rotation, not the subsequent moving
footage. The bundled demo has a malformed ending, so this recording uses a
loaded map and normal exit instead of claiming a completed demo benchmark.

The DOS recording uses the default Voodoo2 software/span-JIT path. The
experimental manual Intel GPU backend is not enabled in the released defaults.
The Windows driver accelerates Windows APIs independently of that DOS path.

All three Windows games returned normally to the desktop after capture.
Dedicated native 2D, mixed Direct3D, Glide2/3 and OpenGL checks passed separately;
these recordings do not establish complete API conformance.

See [SHA256SUMS.txt](SHA256SUMS.txt) for the exact published MP4 files.
