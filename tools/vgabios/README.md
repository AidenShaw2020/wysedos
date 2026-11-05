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

To verify the local modification without overwriting the distributed ROM:

```powershell
python tools/vgabios/patch.py work/vgabios-rebuilt.bin
Get-FileHash disks/386/vgabios.bin,work/vgabios-rebuilt.bin -Algorithm SHA256
```

Verified on 2026-11-05: both hashes are
`cb8dd1dc261c247807b6986b515662eda7d2a626ff30c01af6fe9734767165c2`.

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
