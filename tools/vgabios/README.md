# Local SeaVGABIOS modification

`patch.py` assembles `vbe_pm.S` with LLVM and appends a VBE 2.0 protected-mode
interface to `seavgabios.orig.bin`. It also implements VBE/DPMS (INT 10h
`4F10h`) through the wyseDOS DISPI extension at index `0Bh`. The original
SeaVGABIOS records DPMS in the BIOS data area without changing scanout;
Windows 98 then leaves a visible text cursor while the monitor is off.
The extension keeps display power independent of the VGA register restore
Windows performs after that BIOS call. On/standby/suspend/off, capability
queries and state queries are supported; unsupported requests fail.

The patch redirects both original INT 10h entries
and updates ROM size fields and checksums. The modification sources are
distributed under the project's GPLv3 terms.

The legacy clear/scroll path also refreshes its cached mode descriptor when
the standard BIOS data area's mode differs from the private cached mode.
DOS startup temporarily selects mode 93h and restores the standard BDA
directly; the old cache then still described graphics mode 13h while the
screen and BDA were text mode 03h. Automatic CLS consequently did not clear
the text page. The patch uses the original BIOS mode lookup without changing
VGA registers or clearing other pages. Unchanged cached modes retain the
original path; unknown modes retain the existing descriptor.

The advertised real-mode VBE `WinFuncPtr` also preserves flags around the
original bank callback. That callback starts with CLI/CLD but returns with
RETF without restoring flags; a direct far caller therefore loses IF and DF.
Paranoia consequently waited for a timer interrupt with interrupts disabled.
The appended wrapper saves flags, far-calls the unchanged callback, restores
flags and far-returns. Its code is outside the copied protected-mode table.

To verify the local modification without overwriting the distributed ROM:

```powershell
python tools/vgabios/patch.py work/vgabios-rebuilt.bin
Get-FileHash disks/386/vgabios.bin,work/vgabios-rebuilt.bin -Algorithm SHA256
```

Verified on 2026-11-06: both hashes are
`776c35bc955142daf1402bb4409a1346a6449a4bad4013d5619b6a4a725c8c38`.

`python tools/tests/vgabios_bankcall.py work/vgabios-rebuilt.bin` obtains the
actual WinFuncPtr through both INT10 entries and checks set/get/invalid-window
calls with IF and DF enabled and disabled. All 24 cases pass; the prior ROM
fails 22 cases (two invalid calls coincidentally produce the expected flags).
Native-VGA hardware now proceeds into Paranoia's animated intro and story.
The separately captured stale Windows-image flash is fixed by the monitor's
VBE frame-ownership guards on both native-fetch paths and the display-core
handoff. It was not caused by the bank callback itself.

`python tools/tests/vgabios_text_clear.py work/vgabios-rebuilt.bin` checks
18 original clear cases and two temporary-mode/restored-BDA regressions.
All 20 cases pass; the previous ROM fails both restored-BDA cases. The fix
was also verified on Wyse boot option 3 with software and native VGA,
without enabled tracing. These changes are included in v1.1.14; upgrades must
replace the VGA ROM as well as the monitor.

`python tools/tests/vgabios_dpms.py work/vgabios-rebuilt.bin` executes both
patched 16-bit interrupt entries using Unicorn and checks state changes,
invalid requests, register preservation and ROM checksums. The VGA renderer
regression in `tools/tests/vga_planar.c` checks blanking across VGA register
restores and native-framebuffer wake without losing video memory.

This verifies the modification relative to the preserved base binary; it
does not rebuild that original binary from upstream source. Its embedded
base revision is `106549a4362f6b499da522f8f8f5ed9f98388f87` with a `dirty`
marker. See the [licence and source-provenance notice](../../LICENSES/seabios.md).

The protected-mode display-start routine captures DX:CX before polling retrace.
See [page-flip and text-copy fixes](../../docs/vbe-protected-mode.md) and run
`python tools/tests/vgabios_pm.py work/vgabios-rebuilt.bin` to check the actual relocated code.
