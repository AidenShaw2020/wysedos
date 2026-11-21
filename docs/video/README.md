# 3D preview recordings

The original DOS and Windows 98 clips were recorded on 2026-11-12 with
128 MB guest RAM, 8 MB VGA memory, 12 MB
Voodoo2 and a 64 MB WGPU workspace. The base CPU setting is 133 MHz;
automatic CPU acceleration was enabled for DOS Glide and Windows 3D in the earlier recordings.
No separate Windows booster program is installed.

These are scene samples, not whole-game averages or controlled comparisons
between renderers. The videos are 1280x720 at 60 fps. The earlier clips have a
digital audio track. Game FPS is measured separately. Game data is not
distributed.

## Full Speed measurements

Doom `demo3`, 320×200 high detail with sound off, finished 1091 game ticks
in 440 real ticks at 35 Hz from eMMC on the Wyse 3040, yielding **86.78 FPS**.
The [22-second Doom clip](01-doom-full-speed.mp4) shows the same demo at
normal speed with digital game audio.

[Quake 1.06](02-quake-comparison.mp4) ran the sound-off `demo1` timedemo
from a DOS 6.22 PXE disk. It completed 969 frames in both runs, with
**211.6 FPS at 320×200** and **21.1 FPS at 640×480** in VESA mode.
The video shows normal playback with game audio in both modes.

[Duke Nukem 3D 1.3D](03-duke3d-full-speed.mp4) ran from eMMC at 320×200.
DNRATE showed **698 FPS** in one stationary sample on the E1L1 rooftop.
The 25-second clip shows the live counter as the view changes and includes
digital game audio.

[Tyrian 2000](04-tyrian-full-speed.mp4) ran from the clean MS-DOS 6.22
profile in the Torm level at 320×200 on Normal difficulty. The
in-game debug display reached **34.746 FPS** in the recorded section.
The clip includes the game's original music and effects.

[Tomb Raider / DOS Glide](05-tombraider-dos-glide.mp4) completed 450
Voodoo2 frames in 20.501 s at the stationary Caves entrance (**21.95 FPS**).
The moving clip uses the same DOS Glide renderer and includes Sound Blaster
effects.

[Need for Speed III on Windows 98](06-nfs3-directx.mp4) used 640×480×16
and the native Direct3D 5 renderer on Hometown with the Corvette.
The driver counted 597 handled flips over 20.152 s (**29.63 FPS**) from
the eMMC installation at Full Speed. This clip is silent and omits the
bottom row of the race leaderboard.

[Need for Speed III on Windows 2000](09-nfs3-windows2000-directx.mp4)
used the same Hometown route and 640×480×16 graphics settings. It
completed 832 VGA page flips over 30.009 s (**27.73 FPS**) at Full
Speed. The clip includes digital game audio and crops the bottom
leaderboard row.

[Quake II on Windows 2000](11-quake2-windows2000-opengl.mp4) used
the WGPU OpenGL ICD at 640×480. The Outer Base `timerefresh` rotation
took 4.638735 s for 128 views (**27.59 FPS**) at default lightmap
intensity. The Comm Center gameplay clip uses `gl_modulate 2` and
includes digital game audio.

## Earlier measurements

| Recording | Game mode | Measurement |
| --- | --- | --- |
| DOS Tomb Raider / Glide (earlier clip) | 640x480, Voodoo2 emulation, CPU span renderer | 437 completed buffer changes / 20.129 s = **21.71 FPS**, original stationary level start; sound disabled |
| Windows NFS III / DirectX (earlier clip) | 640x480, native **DX5**, Hometown / Corvette | 621 handled Flips / 20.984 s = **29.59 FPS**, driving; zero declined Flips throughout the run |
| [Windows NFS III / Glide](07-nfs3-glide.mp4) | 640x480, Velocity9x Glide3, Hometown / Corvette | 247 swap calls / 15.001 s = **16.47 swaps/s**, complete periodic interval inside the marked race sample |
| [Windows Quake II test / OpenGL](08-quake2-opengl.mp4) | 640x480, Velocity9x Intel Gen8 ICD | Separate opening-scene rotation: 128 views / 3.647994 s = **35.087779 FPS** |
| Windows 2000 NFS III / DirectX (earlier clip) | 640x480x16, Direct3D 5, Hometown / Corvette | **27.65 FPS** over 30.02 s of a race with the Windows 98 detail preset |
| [Windows XP NFS III / DirectX](10-nfs3-windowsxp-directx.mp4) | 640x480x16, Direct3D 5, Hometown / Corvette | **27.94 FPS** over 30.03 s with the same detail preset |
| Windows 2000 Quake II / OpenGL (earlier clip) | 640x480, RGB565/Z16, WGPU OpenGL ICD | **29.44 FPS** in the separate 128-view opening-scene rotation |
| [Windows XP Quake II / OpenGL](12-quake2-windowsxp-opengl.mp4) | 640x480, RGB565/Z16, same ICD | **29.53 FPS** in the separate 128-view opening-scene rotation |

NFS retains medium graphics, mirror, VSync and sound. Its clips contain
different geometry and do not establish an API speed comparison. Quake II's
number applies to its opening rotation, not the subsequent moving footage.
Its bundled demo has a malformed ending, so a loaded map and normal exit
were used for the opening rotation. The NT footage shows selected earlier
passages from that demo with `gl_modulate 2` for visibility. The benchmark
uses the normal `gl_modulate 1`. The NT clips use direct 60 fps HDMI
capture and normal playback speed with sound. The cropped NFS footage
retains the measurement footer and removes the lowest leaderboard row.

Earlier runs reached 22.2 FPS at the same Tomb Raider level start and
29.92 FPS driving NFS III on DX5. The replacement tests restore comparable
paths. The previous DX6 sample (20.79 handled Flips/s) used a different
renderer and has been replaced. The previous Tomb Raider moving sample
(6.45 FPS) also ran with a regression now identified and corrected.

Opening the unused Windows render engine alone reproduced the Tomb Raider
slowdown, even with no submitted work. Closing it restored performance.
Releasing forcewake alone did not fix it. The firmware now disables the idle
command ring only after its fence completes and its hardware head reaches
the submitted tail. A later Windows request restores the same ring control,
contents, positions, fences and mappings. The replacement Tomb Raider sample
ran after two GPU tests with automatic ring suspension; it completed 437
frames and 437 swaps, at about 377 triangles per frame. Host elapsed time
independently gives 21.7077 FPS. No CPU-frequency mechanism is inferred from
this experiment.

The verified DOS launch route is a DOS boot with the CD driver loaded. An
additional direct launch from Windows' Restart in MS-DOS session stalled
before Glide initialization; that route is not claimed working. A guest DOS
boot retains the monitor's GPU state and avoids that session-start issue.
A repeat after native Windows 2D, Direct3D, Glide2 and OpenGL checks measured
436 completed frames / 20.115 s = 21.68 FPS in the same level-start scene.

## Measurement and capture limits

The new Tomb Raider clip uses digital capture at 60 fps with 48 kHz audio.
The stationary Tomb Raider benchmark ran separately from the moving footage.
Capture may still lose or duplicate frames and cannot independently prove
the game's frame cadence. Playback remains at original speed.

DOS counts completed buffer changes, with matching processed swap deltas.
DirectX counts successfully handled Flips; the new DX5 run also records zero
declined Flips, so declined-Flip fallback is excluded for that run. Neither
counter measures physical monitor refresh. Windows Glide counts swap calls
before the operation without checking successful completion, so its caption
is a provisional backend rate. Quake II reports its own rotation benchmark.

The retained Windows Glide and OpenGL clips used a shared MJPEG input with
a 1/25-second time base before conversion to 60 fps. That input could drop
or duplicate capture frames and retain only the latest frame. Those clips
cannot independently validate game FPS. Pixel-change analysis is also only
a capture cross-check, especially for static or partially scanned frames.

DOS uses the default Voodoo2 software/span-JIT path; the experimental manual
Intel GPU backend remains disabled. Windows Velocity9x accelerates its APIs
independently. Dedicated native 2D, mixed Direct3D, Glide2/3 and OpenGL checks
are separate from these scene samples and do not establish full conformance.

See [SHA256SUMS.txt](SHA256SUMS.txt) for the published MP4 files.
