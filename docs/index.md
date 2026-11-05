# wyseDOS documentation

These guides describe the v1.1.10 implementation on the Dell Wyse 3040.
They distinguish supported mechanisms from configurations actually tested on
hardware. Detailed engineering reports retain their original test conditions.

## User guides

| Guide | Contents |
|---|---|
| [Installation](installation.md) | Release files, eMMC preparation, DOS and Windows setup |
| [Configuration](configuration.md) | Controls, persistent settings, CPU, RAM and sound |
| [Storage](storage.md) | Native C:, USB disks, images and exclusive ownership |
| [Compatibility](compatibility.md) | Tested systems/games, performance and limits |
| [Troubleshooting](troubleshooting.md) | Boot, display, sound, USB and bug reports |

## Technical documentation

| Guide | Contents |
|---|---|
| [Architecture](architecture.md) | VT-x, firmware dependencies, core allocation and source map |
| [Development](development.md) | Build requirements, regression tests and release verification |
| [Release history](../CHANGELOG.md) | Selected verified changes through v1.1.8 |
| [Third-party notices](../README.md#acknowledgements-and-licence) | Bundled components and excluded packages |
| [SeaBIOS provenance](../LICENSES/seabios.md) | Licence texts, ROM hashes and remaining source gap |

## Engineering reports

- [USB storage and image ownership qualification](usb-validation.md)
- [Input timing and PS/2 idle handling](input-timing.md)
- [Display-derived audio clocks](audio-clock.md)
- [Sound Blaster DMA ownership](sound-dma.md)
- [Analog headphone volume and codec routing](headphone-volume.md)
- [VGA page flips and wide virtual surfaces](vga-page-flips.md)

Reports describe specific runs, not universal compatibility. Earlier pending
installation steps in a dated report are historical; consult the guides for
the current overview.
