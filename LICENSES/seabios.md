# SeaBIOS / SeaVGABIOS

The bundled ROMs are separate upstream firmware works. The checked source
entry points at revision `106549a4362f6b499da522f8f8f5ed9f98388f87` state
LGPLv3. The upstream distribution supplies both complete licence texts:

- [GNU GPL version 3](../disks/386/SeaBIOS-COPYING.txt), upstream `COPYING`.
- [GNU LGPL version 3](../disks/386/SeaBIOS-COPYING.LESSER.txt), upstream
  `COPYING.LESSER`, which incorporates GPLv3 terms.
- [ROM notices, authors, hashes and source-provenance status](../disks/386/SeaBIOS-NOTICE.txt).

These files are copied with the ROMs into the installation image and the
installed firmware partition. A separate `wysedos-licenses.zip` release
asset provides these texts alongside the project and other recorded licences.

The texts were retrieved unchanged from that pinned upstream revision.
Their SHA-256 values are:

| Upstream file | SHA-256 |
|---|---|
| `COPYING` | `8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903` |
| `COPYING.LESSER` | `a853c2ffec17057872340eee242ae4d96cbf2b520ae27d903e1b2fef1a5f9d1c` |

Source: `coreboot/seabios`, revision
`106549a4362f6b499da522f8f8f5ed9f98388f87`, files `COPYING` and
`COPYING.LESSER`. Relevant headers checked: `src/romlayout.S`,
`vgasrc/vbe.c` and `vgasrc/vgaentry.S`.

## Verified local modification and remaining source gap

The preserved original VGA image plus `tools/vgabios/patch.py` and
`tools/vgabios/vbe_pm.S` reproduce `disks/386/vgabios.bin` byte for byte.
See [the patch documentation](../tools/vgabios/README.md).

Both original ROMs contain a `dirty` build identifier. The identified upstream
base and existing patch sources are not proof of the complete historical
build inputs. The original patched source tree, configuration and toolchain
still require verification. This notice deliberately does not certify complete
corresponding-source compliance or treat a binary ROM as its own source code.
