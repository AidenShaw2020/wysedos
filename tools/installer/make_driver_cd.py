"""Create one DOS-readable CD with the pinned DOS/9x and selected NT5 packages."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import re

import pycdlib


NT_FILES = (
    'WGPUNT.INF', 'WGPUNT.SYS', 'WGPUDSP.INF', 'WGPUVBE.SYS', 'WGPUVBE.DLL',
    'NTINST.EXE', 'README.TXT', 'DISPSRC.ZIP',
)


def read_iso(image, path):
    data = io.BytesIO()
    image.get_file_from_iso_fp(data, iso_path=path)
    return data.getvalue()


def nt_payload(source):
    image = pycdlib.PyCdlib()
    image.open(str(source))
    try:
        manifest = json.loads(read_iso(image, '/SOURCES.JSN;1'))
        has_icd = 'WGGLNT.DLL' in manifest
        files = (*NT_FILES, 'WGGLNT.DLL') if has_icd else NT_FILES
        for filename in files:
            data = read_iso(image, '/' + filename + ';1')
            if not data or hashlib.sha256(data).hexdigest() != manifest[filename]['sha256']:
                raise ValueError(f'NT driver payload mismatch: {filename}')
            if filename == 'WGPUDSP.INF' and (b'wgglnt.dll' in data.lower()) != has_icd:
                raise ValueError('NT OpenGL ICD and display INF must be packaged together')
            yield ('NT5', filename), data
    finally:
        image.close()


def add_payload(image, files):
    parents = {(): ''}
    used = set()

    def add_name(parent, name, directory=False):
        stem, suffix = (name, '') if directory else (Path(name).stem, Path(name).suffix)
        stem = re.sub(r'[^A-Z0-9_]', '_', stem.upper())[:8]
        ext = re.sub(r'[^A-Z0-9_]', '_', suffix[1:].upper())[:3]
        tail = '' if directory else '.' + ext + ';1'
        alias = f'{parent}/{stem}{tail}'
        for index in range(1, 1000):
            if alias not in used:
                used.add(alias)
                return alias
            number = str(index)
            alias = f'{parent}/{stem[:7 - len(number)]}_{number}{tail}'
        raise ValueError(f'Too many ISO names for {parent}/{name}')

    for parts, data in files:
        for depth in range(1, len(parts)):
            prefix = parts[:depth]
            if prefix not in parents:
                location = add_name(parents[prefix[:-1]], prefix[-1], directory=True)
                image.add_directory(iso_path=location, joliet_path='/' + '/'.join(prefix))
                parents[prefix] = location
        location = add_name(parents[parts[:-1]], parts[-1])
        image.add_fp(io.BytesIO(data), len(data), iso_path=location,
                     joliet_path='/' + '/'.join(parts))
        yield location, hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('drivers', type=Path)
    parser.add_argument('nt_media', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if not (args.drivers / 'SHA256.TXT').is_file():
        parser.error('Run prepare_drivers.py first')
    if args.output.resolve() == args.nt_media.resolve():
        parser.error('The output must be distinct from the NT driver source')

    payload = [(path.relative_to(args.drivers).parts, path.read_bytes())
               for path in sorted(args.drivers.rglob('*'))
               if path.is_file() and path != args.drivers / 'SHA256.TXT']
    payload.extend(nt_payload(args.nt_media))
    checksums = ''.join(f"{hashlib.sha256(data).hexdigest()}  {'/'.join(parts)}\r\n"
                        for parts, data in payload)
    payload.append((('SHA256.TXT',), checksums.encode('ascii')))
    image = pycdlib.PyCdlib()
    image.new(interchange_level=3, joliet=3, vol_ident='WYSEDOS_DRIVERS')
    manifest = list(add_payload(image, payload))
    image.write(str(args.output))
    image.close()
    image.open(str(args.output))
    try:
        for location, expected in manifest:
            if hashlib.sha256(read_iso(image, location)).hexdigest() != expected:
                raise ValueError(f'Driver CD readback mismatch: {location}')
    finally:
        image.close()
    print(f'Verified {len(manifest)} files in {args.output}')


if __name__ == '__main__':
    main()
