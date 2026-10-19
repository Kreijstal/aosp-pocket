#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Separate experimental APK; the PRoot binary is used only for comparison."""
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
out = ROOT / "out/direct"
with zipfile.ZipFile(out / "base.apk") as base, zipfile.ZipFile(out / "packed.apk", "w", zipfile.ZIP_DEFLATED, compresslevel=1) as apk:
    for entry in base.infolist():
        apk.writestr(entry, base.read(entry))
    apk.write(ROOT / "out/runtime-native/libproot.so", "lib/arm64-v8a/libproot.so")
    apk.write(out / "abi.json", "assets/direct-abi.json")
    for path in sorted((out / "rootfs").rglob("*")):
        if path.is_file():
            apk.write(path, "assets/direct-rootfs/" + path.relative_to(out / "rootfs").as_posix() + ".payload")
    for source, target in (("sources.lock.json", "sources.lock.json"),
                           ("runtime/NOTICE", "licenses/NOTICE"),
                           ("vendor/proot/COPYING", "licenses/proot.txt"),
                           ("runtime/licenses/talloc.txt", "licenses/talloc.txt"),
                           ("runtime/licenses/GPL-3.0-only.txt", "licenses/runtime.txt"),
                           ("out/api28/reference/system/etc/NOTICE.xml.gz", "licenses/android9-NOTICE.xml.gz")):
        apk.write(ROOT / source, "assets/" + target)
print("Packed direct backend probe:", (out / "packed.apk").stat().st_size, "bytes")
