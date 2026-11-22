"""Fetch and verify the upstream x64 NTFS-3G UEFI driver and its exact source."""
import argparse
import hashlib
from pathlib import Path
from urllib.request import urlopen

UPSTREAM = "https://github.com/pbatard/ntfs-3g"
COMMIT = "a2226ecd8344a427a7ad79fbb878f3244c7af393"
DRIVER_SHA256 = "bea8e78bd1e68a0f3506faab9f2e88274b83cb159b1e7e06191f57228e8086cd"
SOURCE_SHA256 = "ee0266aade1d9210b8d0abd29630df9d215e15b386aa1c5b64c599eb4844cc4b"
SOURCE_NAME = "ntfs-3g-uefi-1.9-source.zip"


def verify(data, expected, name):
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"SHA-256 mismatch for {name}")
    return data


def fetch(path, url, expected):
    if path.exists():
        verify(path.read_bytes(), expected, path.name)
        return
    with urlopen(url, timeout=45) as response:
        data = verify(response.read(), expected, path.name)
    scratch = path.with_suffix(path.suffix + ".new")
    scratch.write_bytes(data)
    scratch.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    fetch(args.driver, f"{UPSTREAM}/releases/download/1.9/ntfs_x64.efi", DRIVER_SHA256)
    fetch(args.source, f"https://codeload.github.com/pbatard/ntfs-3g/zip/{COMMIT}", SOURCE_SHA256)


if __name__ == "__main__":
    main()
