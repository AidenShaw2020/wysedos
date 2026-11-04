# Native VGA page flips

The native VGA backend lets the guest draw into the Atom legacy VGA memory.
The display core copies that memory and converts it to the GOP surface.

Monster Bash (EGA 320x200, 16 colours) exposed a frame lifetime error: the
renderer requested the previous front page at vertical retrace, while the
game immediately reused that page as its back buffer. The copy could contain
the background restoration but only part of the moving object. Static images
looked correct. This was a page-selection error, not an unsupported EGA mode.

For disjoint planar front/back pages, scanout now uses the newly latched
front page, which stays intact during the next display period. The request
is published at retrace end, after both the CRTC start address and fine pixel
panning have been latched. A single atomic word publishes the pair to the
display core.

Page separation is calculated from the visible height, repeated scan lines,
pitch and width in plane addresses, with an extra group for fine panning.
The calculation handles the 64 KB plane-address wrap. Overlapping scroll
windows, split screens with a shared bottom panel, banked modes and unknown
layouts retain the previous earlier snapshot timing. That timing protects
single-buffer drawing, whose visible memory can be modified after retrace.
The software/VBE path keeps its existing retrace-end scheduling.

This remains a frame snapshot renderer, not cycle-accurate CRT beam emulation.
It does not guarantee coherent output from arbitrary writes to the visible
page throughout a display period or from a renderer slower than a frame.

## Wide virtual planar surfaces

Commander Keen 4's opening letters exposed a separate, pre-existing native
VGA problem: horizontal damage and trails remained with the original 1.1.7
firmware and with corrected page selection. A full software VGA A/B run was
clean and was confirmed by the user. Cherry Trail's legacy aperture is known
to drop bitmap word stores crossing an eight-byte boundary; wide planar
bitmap drawing already uses the software backend for Windows VGA modes.

That backend selection now considers the virtual row pitch as well as the
visible width. Keen's 320-pixel intro uses 1984-pixel virtual rows (CR13=7Ch),
so it takes the same safe path. Its game uses 336-pixel rows (CR13=15h) and
returns to native VGA. Text, Bash's 384-pixel virtual rows and 256-colour
modes keep their existing backend. No game name or executable is detected.

## Regression test

`tools/tests/vga_page_flip.c` exercises the real retrace state machine and
planar renderer. It erases the old page after retrace begins and verifies
that the new page and its newly latched pan still produce the expected
pixels. It also covers no-flip periods, overlapping windows, plane wrap,
Mode X classification, shared panels, banked fallback and software timing.
Returning the scheduler to the old behaviour fails this test. Existing
`tools/tests/vga_planar.c` covers latches, blanking, DPMS and the native VBE
cursor cache.

Build on Windows with LLVM:

```powershell
& 'C:/Program Files/LLVM/bin/clang.exe' -O2 -ffunction-sections -DBPP=32 -DWYSE_BUILD=1 tools/tests/vga_page_flip.c '-Wl,/opt:ref' -o work/vga_page_flip.exe
& ./work/vga_page_flip.exe
```
