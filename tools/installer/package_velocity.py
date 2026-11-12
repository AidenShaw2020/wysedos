"""Package the local Velocity fork with corresponding source and notices.

This is a maintainer operation, not a downloader. No disks are opened.
Run the Velocity build gates and native checks before publishing its output.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / 'drivers/windows9x/velocity9x'
VERSION = 'wysedos-g5j'
UPSTREAM = 'https://github.com/michaeldale/velocity9x'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def archive(path, files):
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED,
                         compresslevel=9) as target:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            target.writestr(info, data)


def package(source, binaries, tested_dlls):
    source = source.resolve(strict=True)
    binaries = binaries.resolve(strict=True)
    commit = subprocess.check_output(
        ['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    tracked = subprocess.check_output(
        ['git', '-C', str(source), 'ls-files', '-z']).decode().split('\0')
    added = subprocess.check_output(
        ['git', '-C', str(source), 'ls-files', '--others', '--exclude-standard',
         '-z', '--', 'src', 'include', 'tests', 'scripts', 'packaging', 'tools']
    ).decode().split('\0')
    files = {}
    # Historical probe captures and old releases are not build inputs. Keep
    # upstream documentation text, plus every source/build/tool input.
    roots = {'src', 'include', 'tests', 'scripts', 'packaging', 'tools', 'logo'}
    top = {'LICENSE', 'README.md', 'CHANGELOG.md', 'PLAN.md', '.gitignore'}
    for name in sorted(set(tracked + added) - {''}):
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError(f'Unsafe source path: {name}')
        keep = relative.parts[0] in roots or name in top
        keep |= name.startswith('docs/') and relative.suffix in {'.md', '.psd1'}
        keep |= name.startswith('docs/probe/references/')
        keep |= name.startswith('docs/decisions/') and relative.suffix == '.ini'
        if keep:
            path = source / relative
            if path.is_symlink() or not path.resolve().is_relative_to(source):
                raise ValueError(f'Unsafe source input: {name}')
            files[f'velocity9x/{name}'] = path.read_bytes()
    provenance = (
        f'Velocity9x wyseDOS fork: {VERSION}\r\n'
        f'Upstream: {UPSTREAM}\r\nBase commit: {commit}\r\n'
        'Modified sources are included, not an unmodified upstream release.\r\n'
        'GPL version 3; see LICENSE and NOTICES.TXT.\r\n'
        'Build: Open Watcom v2 + separately obtained Windows 98 DDK.\r\n'
        'See velocity9x/docs/BUILDING.md and scripts/run-checks.ps1.\r\n'
        'The DDK, toolchain, games and guest operating system are not included.\r\n'
        'Required upstream test references are included; other historical\r\n'
        'captures and upstream release binaries are omitted.\r\n'
    ).encode('ascii')
    files['SOURCE.TXT'] = provenance
    VENDOR.mkdir(parents=True, exist_ok=True)
    src = VENDOR / f'velocity9x-{VERSION}-src.zip'
    archive(src, files)
    payload = {p.name.upper(): p.read_bytes() for p in binaries.iterdir()
               if p.is_file() and p.name.upper() != 'SHA256.TXT'}
    if tested_dlls:
        for name in ('V9XHAL.DLL', 'V9XGL.DLL', 'GLIDE2X.DLL', 'GLIDE3X.DLL'):
            payload[name] = (tested_dlls / name).read_bytes()
    payload['LICENSE.TXT'] = (source / 'LICENSE').read_bytes()
    payload['SOURCE.TXT'] = provenance
    payload['SRC.ZIP'] = src.read_bytes()
    payload['WYSE.TXT'] = (VENDOR / 'INSTALL.TXT').read_text().replace(
        '\n', '\r\n').encode('ascii')
    payload['SHA256.TXT'] = ('\r\n'.join(
        f'{digest(data)}  {name}' for name, data in sorted(payload.items())
    ) + '\r\n').encode('ascii')
    output = VENDOR / f'velocity9x-{VERSION}-driver.zip'
    archive(output, payload)
    checksums = {p.name: digest(p.read_bytes()) for p in (src, output)}
    (VENDOR / 'SHA256SUMS.txt').write_text(''.join(
        f'{sha}  {name}\n' for name, sha in checksums.items()), encoding='ascii')
    for name, sha in checksums.items():
        print(f'{sha}  {name}')


if __name__ == '__main__':
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--source', type=Path, required=True)
    cli.add_argument('--binaries', type=Path, required=True)
    cli.add_argument('--tested-dlls', type=Path)
    args = cli.parse_args()
    package(args.source, args.binaries, args.tested_dlls)
