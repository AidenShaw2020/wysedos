"""Pack flat 8.3-named files in a new 1.44MB FAT12 image (no physical I/O).

Usage: python tools/usb/mkdriverfloppy.py output.img input-file [...]
This is a transfer disk, not boot media. Existing output files are refused.
"""
import datetime
import pathlib
import struct
import sys


def build(paths, size_kib=1440):
    if size_kib not in (1440, 2880):
        raise ValueError('Supported floppy sizes: 1440 and 2880 KiB')
    cluster_sectors = 1 if size_kib == 1440 else 2
    cluster_bytes = 512 * cluster_sectors
    image = bytearray(size_kib * 1024)
    image[:11] = b"\xeb\x3c\x90WYSEUSB ".ljust(11, b" ")
    struct.pack_into("<HBHBHHBHHHII", image, 11,
                     512, cluster_sectors, 1, 2, 224, size_kib * 2,
                     0xF0, 9, 18 if size_kib == 1440 else 36, 2, 0, 0)
    image[38] = 0x29
    image[43:54] = b"USB DRIVERS".ljust(11, b" ")
    image[54:62] = b"FAT12   "
    # If accidentally booted, return to the BIOS instead of executing zeros.
    image[62:64] = b"\xcd\x18"
    image[510:512] = b"\x55\xaa"
    fat = bytearray(4608)
    fat[:3] = b"\xf0\xff\xff"

    def set_fat(cluster, value):
        pos = cluster * 3 // 2
        old = int.from_bytes(fat[pos:pos + 2], "little")
        value = ((old & 15) | value << 4) if cluster & 1 else ((old & 0xF000) | value)
        fat[pos:pos + 2] = value.to_bytes(2, "little")

    names = set()
    cluster = 2
    now = datetime.datetime.now()
    date = (now.year - 1980) << 9 | now.month << 5 | now.day
    time = now.hour << 11 | now.minute << 5 | now.second // 2
    for index, path in enumerate(paths):
        if index >= 224:
            raise ValueError("Root directory is full")
        name = path.name.upper()
        stem, dot, extension = name.partition(".")
        if not stem or len(stem) > 8 or len(extension) > 3 or "." in extension:
            raise ValueError(f"Not an 8.3 filename: {name}")
        packed_name = stem.ljust(8).encode("ascii") + extension.ljust(3).encode("ascii")
        if packed_name in names:
            raise ValueError(f"Duplicate filename: {name}")
        names.add(packed_name)
        data = path.read_bytes()
        count = (len(data) + cluster_bytes - 1) // cluster_bytes
        if cluster + count > (size_kib * 2 - 33) // cluster_sectors + 2:
            raise ValueError("Files exceed floppy capacity")
        entry = 19 * 512 + index * 32
        image[entry:entry + 11] = packed_name
        image[entry + 11] = 0x20
        struct.pack_into("<HHHI", image, entry + 22, time, date, cluster if count else 0, len(data))
        start = (33 + (cluster - 2) * cluster_sectors) * 512
        image[start:start + len(data)] = data
        for i in range(count):
            set_fat(cluster + i, cluster + i + 1 if i + 1 < count else 0xFFF)
        cluster += count
    image[512:5120] = fat
    image[5120:9728] = fat
    return image


if __name__ == "__main__":
    output = pathlib.Path(sys.argv[1])
    data = build([pathlib.Path(p) for p in sys.argv[2:]])
    with output.open("xb") as target:
        target.write(data)
    print(f"Created {output}: {len(data)} bytes, {len(sys.argv)-2} files")
