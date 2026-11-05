# VBE protected-mode page flipping

The local SeaVGABIOS patch supplies the VBE 2.0 protected-mode interface
returned by INT 10h AX=4F0Ah. A game copies its table/code into a 32-bit code
segment and calls the entry for display start. In packed-pixel modes DX:CX
contains the framebuffer offset divided by four; BL=80h requests a retrace
wait. This differs from the real-mode function's pixel/scan-line coordinates.
See the [VESA VBE 2.0 standard](https://www.phatcode.net/res/221/files/vbe20.pdf).

## Retrace port corrupted the incoming address

The routine saved caller registers on its stack, but changed DX to 03DAh to
poll VGA retrace before constructing the input address. With BL=80h it then
used that port number as the high word of the page address. Saving/restoring
EDX at the function boundary did not protect the calculation inside it.

Blood 1.21 at 640x480/8-bit exposed this as vertical lines with working sound.
For example, an input low word of 5800h produced DISPI X=0100h, Y=2A26h and
renderer start=1A5800h. Substituting DX=03DAh reproduces those exact values.
The requested page address must be captured before DX is reused for port I/O.

The corrected routine constructs the byte offset in ECX before the retrace
poll. Its register preservation, immediate path and later conversion to DISPI
pixel/row offsets remain intact. This corrects the BIOS interface rather than
changing Blood's renderer or hiding the fault with another video mode.

## Regression

`tools/tests/vgabios_pm.py` obtains the table through the actual 16-bit ROM
entry, relocates it, and executes its assembled 32-bit code in Unicorn. Tests
exercise BL=0 and BL=80h, both address words, several virtual widths and packed
pixel depths, preserved registers/stack, and refusal of invalid calls.
The original ROM fails on the first retrace-wait case; the corrected one passes.
The separate DPMS test still executes both patched interrupt entries.

```powershell
python tools/vgabios/patch.py work/vgabios-rebuilt.bin
python tools/tests/vgabios_pm.py work/vgabios-rebuilt.bin
python tools/tests/vgabios_dpms.py work/vgabios-rebuilt.bin
```

## Text setup screen copies

Blood Setup 1.10 separately exposed incorrect native text-memory accesses.
On the tested Wyse, a scratch-page hardware probe found two bad 16-bit read
alignments and six bad 32-bit read alignments, plus word/dword stores lost
across an eight-byte boundary. For example, a text dword read at +2 returned
2D2C2322h instead of 25242322h; a dword at +6 returned FFFFFFFFh. Screen-save
and restore copies therefore damaged the data behind nested setup dialogs.

When the legacy VGA is in text mode, guest aperture accesses now exit to the
existing instruction helper and reach the actual hardware using volatile byte
transactions. Font/plane storage remains native VGA. Entering a graphics mode
restores its existing native or software mapping; VBE and mode 13h keep their
own paths. This corrects read as well as write semantics, rather than forcing
periodic full redraws of already damaged memory.

`tools/tests/hwvga_access.py` compiles the production access helpers and runs
the generated x86 instructions in Unicorn. It verifies transaction sizes,
little-endian values and untouched neighboring bytes at all word/dword
alignments. Old direct wide-access controls are rejected even though ordinary
RAM would conceal this hardware fault.

Hardware verification used 486/66, 32 MB guest RAM and native eMMC C:. Blood
rendered its 640x480/8-bit attract gameplay and menus after the VBE correction.
The user confirmed clean setup dialog restoration after the text-access fix;
the game was then relaunched with both fixes and rendered correctly again.
