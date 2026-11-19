"""Build the NT5 package ISO accepted by the wyseDOS USB installer builder."""
import argparse
import hashlib
import io
import json
from pathlib import Path
from subprocess import check_output
import zipfile

import pycdlib


ROOT = Path(__file__).resolve().parents[2]
WINVBE_PIN = "25ee72d97cc6129b79329b249cea337ae88a4e25"
VELOCITY = ROOT / "drivers/windows9x/velocity9x/velocity9x-wysedos-g5k-src.zip"


def corresponding_sources(winvbe):
    revision = check_output(["git", "-C", str(winvbe), "rev-parse", "HEAD"],
                            text=True).strip()
    if revision != WINVBE_PIN:
        raise ValueError(f"Use WINVBE revision {WINVBE_PIN}")
    vendor_files = check_output(["git", "-C", str(winvbe), "ls-files",
                                 "SRC/nt", "SRC/d3d", "SRC/patch",
                                 "LICENSE", "README.md"], text=True).splitlines()
    changes = check_output(["git", "-C", str(winvbe), "status", "--porcelain", "--",
                            "SRC/nt", "SRC/d3d", "SRC/patch", "LICENSE", "README.md"],
                           text=True)
    if changes:
        raise ValueError("WINVBE source and notices must match the pinned checkout")
    result = io.BytesIO()
    with zipfile.ZipFile(result, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in vendor_files:
            archive.write(winvbe / name, f"winvbe/{name}")
        project_files = [ROOT / "LICENSE", ROOT / "LICENSES/winvbe.md",
                         ROOT / "LICENSES/velocity9x.txt", VELOCITY,
                         ROOT / "docs/nt5-support.md",
                         ROOT / "tools/installer/build_nt5_drivers.py",
                         ROOT / "tools/installer/build_nt5_opengl.ps1",
                         ROOT / "tools/installer/make_nt5_package.py",
                         ROOT / "tools/installer/driver-docs/NT5.TXT",
                         ROOT / "efi/platform/gen8_asm.c",
                         ROOT / "efi/platform/gen8_asm.h"]
        project_files.extend(sorted((ROOT / "drivers/nt").rglob("*")))
        project_files.extend(sorted((ROOT / "efi/platform/igt").glob("*.h")))
        for path in project_files:
            if path.is_file():
                archive.write(path, "project/" + path.relative_to(ROOT).as_posix())
    return result.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("drivers", type=Path, help="Output of build_nt5_drivers.py")
    parser.add_argument("icd", type=Path, help="WGGLNT.DLL built with build_nt5_opengl.ps1")
    parser.add_argument("winvbe", type=Path, help="Pinned WINVBE git checkout")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    files = {name: (args.drivers / name.lower()).read_bytes() for name in
             ("NTINST.EXE", "WGPUNT.INF", "WGPUNT.SYS", "WGPUDSP.INF",
              "WGPUVBE.SYS", "WGPUVBE.DLL")}
    files["WGGLNT.DLL"] = args.icd.read_bytes()
    if b"wgglnt.dll" not in files["WGPUDSP.INF"].lower():
        raise ValueError("The display INF must register the packaged OpenGL ICD")
    files["DISPSRC.ZIP"] = corresponding_sources(args.winvbe)
    readme = (ROOT / "tools/installer/driver-docs/NT5.TXT").read_text(encoding="ascii")
    files["README.TXT"] = readme.replace("\n", "\r\n").encode("ascii")
    files["SOURCES.JSN"] = json.dumps({
        name: {"sha256": hashlib.sha256(data).hexdigest()}
        for name, data in sorted(files.items())
    }, indent=2).encode("ascii")
    iso = pycdlib.PyCdlib()
    try:
        iso.new(interchange_level=3, joliet=3, vol_ident="WYSEDOS_NT5")
        for name, data in files.items():
            iso.add_fp(io.BytesIO(data), len(data), iso_path=f"/{name};1", joliet_path=f"/{name}")
        iso.write(str(args.output))
    finally:
        iso.close()
    iso.open(str(args.output))
    try:
        for name, data in files.items():
            extracted = io.BytesIO()
            iso.get_file_from_iso_fp(extracted, iso_path=f"/{name};1")
            if extracted.getvalue() != data:
                raise ValueError(f"NT5 package readback mismatch for {name}")
    finally:
        iso.close()
    print(f"Verified {len(files)} files in {args.output}")


if __name__ == "__main__":
    main()
