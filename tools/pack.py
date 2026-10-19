#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Embed native PIE executables in the package's installed native-code directory."""
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with zipfile.ZipFile(ROOT / "out/base.apk") as src, zipfile.ZipFile(
            ROOT / "out/packed.apk", "w", zipfile.ZIP_DEFLATED) as dst:
        for info in src.infolist():
            dst.writestr(info, src.read(info.filename))
        for path in sorted((ROOT / "out/lib").glob("*/*.so")):
            # Compression forces normal installer extraction; execute immutable APK code,
            # not binaries downloaded into the writable app home directory.
            dst.write(path, "lib/" + str(path.relative_to(ROOT / "out/lib")))
        dst.write(ROOT / "sources.lock.json", "assets/sources.lock.json")
        dst.write(ROOT / "vendor/toybox/LICENSE", "assets/licenses/toybox.txt")
        dst.write(ROOT / "LICENSE", "assets/licenses/aosp-pocket.txt")


if __name__ == "__main__":
    main()
