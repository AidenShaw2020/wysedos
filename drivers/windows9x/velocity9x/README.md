# Velocity9x for the wyseDOS WGPU channel

This is a modified **Velocity9x 0.44.0** build, `wysedos-g5k`, for Windows 98
on wyseDOS. It adds an Intel Gen8 backend through the monitor's virtual WGPU
render channel. The display remains the emulated VGA/VBE adapter.

- Upstream: <https://github.com/michaeldale/velocity9x>
- Base commit: `d70d0424828806882019e2c89d2366934c21ff28`
- License: GNU GPL version 3; retain the included third-party `NOTICES.TXT`.
- This fork is independently packaged and is not an official upstream release.

The driver ZIP contains the complete unpacked Windows package, license,
provenance and `SRC.ZIP` with corresponding modified sources. A separate
source ZIP is also provided. `SHA256SUMS.txt` records archive hashes; each
driver package has its own `SHA256.TXT` manifest.

The source archive includes source, headers, tools, tests, build scripts and
documentation text and required upstream test reference fixtures. Other
historical captures and upstream release binaries are omitted. It contains no Microsoft DDK, Windows files, game assets or private
rig evidence. Building requires Open Watcom v2 and a separately obtained
Windows 98 DDK; see the archive's `docs/BUILDING.md`.

Read [INSTALL.TXT](INSTALL.TXT) for installation and recovery. The firmware
installer supplies this package under `C:\DRIVERS\VELOCITY`; installing
Windows and choosing its display driver remain separate steps.

Direct3D, Windows Glide and OpenGL share WGPU. DOS Glide continues to use the
virtual Voodoo2 path. Native pixel/regression tests and particular games have
been checked; universal API compatibility and a sustained 30 FPS are not
claimed. Benchmark recordings document their actual settings and measurement
method separately from capture frame rate.
