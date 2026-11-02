"""Turn the wyseDOS logo into 386/logo.bin for the monitor's splash screen.

  mklogo.py [png] [out]

Format (little endian): "WDLG", width u16, height u16, background BGRX u32,
256 palette entries BGRX u32, then width*height palette indexes.
"""
import struct
import sys
from PIL import Image

src = sys.argv[1] if len(sys.argv) > 1 else "wysedos-logo.png"
out = sys.argv[2] if len(sys.argv) > 2 else "../../disks/386/logo.bin"
im = Image.open(src).convert("RGB")
w = 1280
h = round(im.height * w / im.width)
im = im.resize((w, h), Image.LANCZOS)
bg = im.getpixel((4, 4))
q = im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
pal = q.getpalette()[: 256 * 3] + [0] * (768 - len(q.getpalette()[: 256 * 3]))
with open(out, "wb") as f:
    f.write(b"WDLG" + struct.pack("<HHI", w, h, bg[2] | bg[1] << 8 | bg[0] << 16))
    for i in range(256):
        r, g, b = pal[3 * i : 3 * i + 3]
        f.write(struct.pack("<I", b | g << 8 | r << 16))
    f.write(q.tobytes())
print(out, w, h, "background", bg)
