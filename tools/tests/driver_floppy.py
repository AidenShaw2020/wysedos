"""Read generated FAT12 chains independently, including cluster boundaries."""
import importlib.util
import pathlib
import struct
import tempfile

spec = importlib.util.spec_from_file_location(
    "floppy", pathlib.Path(__file__).parents[1] / "usb/mkdriverfloppy.py")
floppy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(floppy)

work = pathlib.Path(__file__).parents[2] / "work"
work.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=work, prefix="floppy-test-") as directory:
    source = pathlib.Path(directory) / "PAYLOAD.BIN"
    expected = bytes(range(256)) * 33 + b"last cluster"
    source.write_bytes(expected)
    for kib in (1440, 2880):
        image = floppy.build([source], kib)
        assert len(image) == kib * 1024 and image[510:512] == b"\x55\xaa"
        sector, spc, reserved, fats, roots, total = struct.unpack_from("<HBHBHH", image, 11)
        sectors_per_fat = struct.unpack_from("<H", image, 22)[0]
        assert sector == 512 and total * sector == len(image)
        root = (reserved + fats * sectors_per_fat) * sector
        data = root + (roots * 32 + sector - 1) // sector * sector
        assert image[root:root+11] == b"PAYLOAD BIN"
        cluster = struct.unpack_from("<H", image, root + 26)[0]
        length = struct.unpack_from("<I", image, root + 28)[0]
        result = bytearray()
        seen = set()
        while cluster < 0xff8:
            assert cluster >= 2 and cluster not in seen
            seen.add(cluster)
            start = data + (cluster - 2) * spc * sector
            result.extend(image[start:start + spc * sector])
            at = reserved * sector + cluster * 3 // 2
            pair = int.from_bytes(image[at:at+2], "little")
            cluster = pair >> 4 if cluster & 1 else pair & 0xfff
        assert result[:length] == expected
print("PASS: FAT12 1.44/2.88MB headers, exact size, root entry and cross-cluster payload")
