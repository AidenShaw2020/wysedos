"""Build the unpacked C:\\DRIVERS payload from pinned upstream packages.

Only reads local archives and writes tools/installer/out/DRIVERS. It never
opens physical disks or runs any executable from a driver package.
"""
import hashlib
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'tools/installer/out'
DEST = OUT / 'DRIVERS'
PACKAGES = {
    'VGA': ('drivers/windows9x/vmdisp9x/vmdisp9x-1.2025.0.119b-driver-2d.zip',
            'e0d698a6089347a6a619ed439c092f247392fbafac6b342f4651c1380911fb23'),
    'USBDOS': ('drivers/dos/usbdos/usbdos.zip',
               '45937257ac508443b8c30d31e9b492e50ee50ccc9250e7f8d831fd4aa3cf46ba'),
    'CTMOUSE': ('drivers/dos/ctmouse.zip',
                'f5d00e98e81d06060db0d1f36e4f51d44447fd0c794dc2cedd9a1cc696e3bee2'),

}


def unpack_zip(archive, target):
    with zipfile.ZipFile(archive) as package:
        for entry in package.infolist():
            path = (target / entry.filename).resolve()
            if not path.is_relative_to(target.resolve()):
                raise ValueError(f'Archive path escapes destination: {entry.filename}')
        package.extractall(target)


def build():
    # Check every input before replacing the previous generated payload.
    for relative, expected in PACKAGES.values():
        path = ROOT / relative
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f'Driver archive checksum mismatch: {path}')
    OUT.mkdir(parents=True, exist_ok=True)
    # A fixed, resolved build-only directory; never accept a caller's path.
    if DEST.resolve().parent != OUT.resolve() or DEST.is_symlink():
        raise ValueError('Unsafe driver staging directory')
    if DEST.exists():
        shutil.rmtree(DEST)
    DEST.mkdir()
    for name, (relative, _) in PACKAGES.items():
        target = DEST / name
        target.mkdir()
        archive = ROOT / relative
        unpack_zip(archive, target)
    shutil.copy2(DEST / 'CTMOUSE/BIN/CTMOUSE.EXE', DEST / 'CTMOUSE/CTMOUSE.EXE')
    # Make the GPL sources available unpacked alongside the tested binary.
    unpack_zip(DEST / 'CTMOUSE/SOURCE/CTMOUSE/SOURCES.ZIP', DEST / 'CTMOUSE/SRC')
    shutil.copy2(ROOT / 'LICENSES/nusb.txt', DEST / 'EXCLUDED.TXT')
    for filename in ('README.TXT', 'USBON.BAT', 'USBOFF.BAT', 'WINUSB.BAT'):
        value=(ROOT / 'tools/installer/driver-docs' / filename).read_text(encoding='ascii')
        (DEST / filename).write_bytes(value.replace('\n','\r\n').encode('ascii'))
    manifest = []
    for path in sorted(DEST.rglob('*')):
        if path.is_file():
            manifest.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(DEST).as_posix()}')
    (DEST / 'SHA256.TXT').write_bytes(('\r\n'.join(manifest) + '\r\n').encode('ascii'))
    print(f'Unpacked driver payload: {len(manifest)} files in {DEST}')


if __name__ == '__main__':
    build()
