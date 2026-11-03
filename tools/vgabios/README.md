# Local SeaVGABIOS modification

`patch.py` assembles `vbe_pm.S` with LLVM and appends a VBE 2.0 protected-mode
interface to `seavgabios.orig.bin`. It redirects the original INT 10h entries
and updates ROM size fields and checksums. The modification sources are
distributed under the project's GPLv3 terms.

To verify the local modification without overwriting the distributed ROM:

```powershell
python tools/vgabios/patch.py work/vgabios-rebuilt.bin
Get-FileHash disks/386/vgabios.bin,work/vgabios-rebuilt.bin -Algorithm SHA256
```

Verified on 2026-11-03: both hashes are
`25a0b94795cb9bed6a68ef4e4a6846bf71e541f6293ea1917ce74c258d25b6f3`.

This verifies the modification relative to the preserved base binary; it
does not rebuild that original binary from upstream source. Its embedded
base revision is `106549a4362f6b499da522f8f8f5ed9f98388f87` with a `dirty`
marker. See the [licence and source-provenance notice](../../LICENSES/seabios.md).
