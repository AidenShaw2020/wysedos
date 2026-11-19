"""Verify the installer image and create the public release ZIPs and checksums."""
import argparse
import hashlib
import io
from pathlib import Path
import shutil
import struct
import zipfile

from pyfatfs.PyFatFS import PyFatFS
import pycdlib


ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ("bios.bin", "vgabios.bin", "logo.bin", "gm.sf2", "gm.sf2.LICENSE.txt",
           "SeaBIOS-COPYING.txt", "SeaBIOS-COPYING.LESSER.txt", "SeaBIOS-NOTICE.txt")
INSTALLER = "wysedos-installer.img"
NT_PACKAGE = "wysedos-nt5-driver-package.iso"
DRIVERS = "wysedos-drivers.zip"
LICENCES = "wysedos-licenses.zip"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def unpack_installer(image, efi):
    with image.open("rb") as stream:
        mbr = stream.read(512)
    if len(mbr) != 512 or mbr[510:] != b"\x55\xaa":
        raise ValueError("Installer has no valid MBR signature")
    offset = struct.unpack_from("<I", mbr, 446 + 8)[0] * 512
    expected = {"/386/" + name: (ROOT / "disks/386" / name).read_bytes() for name in RUNTIME}
    expected["/386/config.ini"] = (ROOT / "tools/installer/runtime.ini").read_bytes()
    expected["/EFI/BOOT/BOOTX64.EFI"] = efi.read_bytes()
    with PyFatFS(str(image), offset=offset, read_only=True) as fs:
        names = {name.casefold(): name for name in fs.walk.files("/")}
        expected_names = {name.casefold() for name in expected} | {"/install.ini", "/386/drivers.iso"}
        if names.keys() != expected_names:
            raise ValueError(f"Unexpected installer layout: {sorted(names.keys() ^ expected_names)}")
        for name, data in expected.items():
            if fs.readbytes(names[name.casefold()]) != data:
                raise ValueError(f"Installer file mismatch: {name}")
        return fs.readbytes(names["/386/drivers.iso"])


def unpack_driver_cd(data):
    cd = pycdlib.PyCdlib()
    cd.open_fp(io.BytesIO(data))
    try:
        files = {}
        for folder, _, names in cd.walk(joliet_path="/"):
            for name in names:
                path = folder.rstrip("/") + "/" + name
                output = io.BytesIO()
                cd.get_file_from_iso_fp(output, joliet_path=path)
                files[path.lstrip("/")] = output.getvalue()
    finally:
        cd.close()
    manifest = dict(line.split("  ", 1)[::-1]
                    for line in files["SHA256.TXT"].decode("ascii").splitlines())
    actual = {name: sha(value) for name, value in files.items() if name != "SHA256.TXT"}
    if manifest != actual:
        raise ValueError("Shared driver CD manifest differs from its contents")
    return files


def make_zip(path, entries):
    with zipfile.ZipFile(path, "x", zipfile.ZIP_DEFLATED) as archive:
        for name, value in sorted(entries.items()):
            archive.writestr(name, value)
    with zipfile.ZipFile(path) as archive:
        if archive.testzip() is not None or set(archive.namelist()) != set(entries):
            raise ValueError(f"ZIP readback failed: {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("efi", type=Path)
    parser.add_argument("nt_package", type=Path)
    parser.add_argument("output", type=Path, help="New output directory")
    args = parser.parse_args()
    files = unpack_driver_cd(unpack_installer(args.image, args.efi))
    for name, expected in unpack_nt_package(args.nt_package).items():
        if files["NT5/" + name] != expected:
            raise ValueError(f"Installed NT5 driver differs from release input: {name}")
    licences = [ROOT / "LICENSE", *sorted((ROOT / "LICENSES").glob("*")),
                ROOT / "pc/voodoo/86box/COPYING", ROOT / "pc/voodoo/UPSTREAM.md"]
    licences += [ROOT / "disks/386" / name for name in RUNTIME if name.endswith(".txt")]
    notices = {path.relative_to(ROOT).as_posix(): path.read_bytes() for path in licences if path.is_file()}
    notices.update({name: data for name, data in files.items()
                    if Path(name).name.upper().startswith(("LICENSE", "COPYING", "NOTICES", "USBINTRO"))
                    or name == "NT5/README.TXT"})
    args.output.mkdir(parents=True, exist_ok=False)
    for source, name in ((args.image, INSTALLER), (args.efi, "BOOTX64.EFI"),
                         (args.nt_package, NT_PACKAGE)):
        shutil.copyfile(source, args.output / name)
    make_zip(args.output / DRIVERS, files)
    make_zip(args.output / LICENCES, notices)
    checksums = "".join(f"{sha(path.read_bytes())}  {path.name}\n"
                        for path in sorted(args.output.iterdir()))
    (args.output / "SHA256SUMS.txt").write_bytes(checksums.encode("ascii"))
    print(f"Verified installer and {len(files)} driver files")
    print(checksums, end="")


def unpack_nt_package(path):
    cd = pycdlib.PyCdlib()
    cd.open(str(path))
    try:
        output = {}
        for folder, _, names in cd.walk(joliet_path="/"):
            if folder != "/":
                raise ValueError("NT5 package must be flat")
            for name in names:
                if name != "SOURCES.JSN":
                    data = io.BytesIO()
                    cd.get_file_from_iso_fp(data, joliet_path="/" + name)
                    output[name] = data.getvalue()
        return output
    finally:
        cd.close()


if __name__ == "__main__":
    main()
