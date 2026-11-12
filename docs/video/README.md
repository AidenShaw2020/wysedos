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
