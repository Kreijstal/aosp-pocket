#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Convert an already extracted, trusted Ananbox/AOSP rootfs into a Pocket ZIP.

Does not follow symlinks, include sockets/devices, or alter the input image.
No proprietary VMOS image or binaries are used.
"""
import argparse
import json
from pathlib import Path
import stat
import zipfile

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("rootfs", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = args.rootfs.resolve()
    if not (source / "system/build.prop").is_file():
        parser.error("rootfs must contain system/build.prop")
    properties = {}
    for line in (source / "system/build.prop").read_text().splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            properties[key.strip()] = value.strip()
    try:
        sdk = int(properties["ro.build.version.sdk"])
    except (KeyError, ValueError):
        parser.error("system/build.prop must declare a valid ro.build.version.sdk")
    if args.output.resolve().is_relative_to(source):
        parser.error("output must be outside rootfs")
    entries = {}
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
        for path in sorted(source.rglob("*")):
            name = path.relative_to(source).as_posix()
            metadata = path.lstat()
            item = {"mode": stat.S_IMODE(metadata.st_mode) & 0o777}
            if stat.S_ISLNK(metadata.st_mode):
                item.update(type="symlink", target=str(path.readlink()))
            elif stat.S_ISDIR(metadata.st_mode):
                item["type"] = "directory"
            elif stat.S_ISREG(metadata.st_mode):
                item["type"] = "file"
                archive.write(path, name)
            else:
                raise SystemExit(f"Unsupported image node: {name}")
            entries[name] = item
        archive.writestr("pocket-layout.json", json.dumps({"format": 1, "abi": "arm64-v8a", "guest_sdk": sdk, "entries": entries}))
    print(f"Packed {len(entries)} image entries into {args.output} ({args.output.stat().st_size} bytes)")

if __name__ == "__main__":
    main()
