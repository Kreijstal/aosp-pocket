#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package the API 28 integration probe without replacing the API 25 APK."""
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
out = ROOT / "out/api28"
with zipfile.ZipFile(out / "base.apk") as base, zipfile.ZipFile(
        out / "packed.apk", "w", zipfile.ZIP_DEFLATED, compresslevel=1) as apk:
    for entry in base.infolist():
        apk.writestr(entry, base.read(entry))
    for name in ("libpocket_runtime.so", "libproot.so"):
        apk.write(ROOT / "out/runtime-native" / name, "lib/arm64-v8a/" + name)
    for path in sorted((out / "probe-rootfs").rglob("*")):
        if path.is_file():
            apk.write(path, "assets/api28-rootfs/" + path.relative_to(out / "probe-rootfs").as_posix() + ".payload")
    apk.write(ROOT / "sources.lock.json", "assets/sources.lock.json")
    apk.write(out / "art-probe.dex", "assets/api28-rootfs/data/pocket-art-probe.dex.payload")
    apk.write(out / "bootclasspath.txt", "assets/bootclasspath.txt")
    for source, target in (("LICENSE", "apache-2.0.txt"), ("vendor/proot/COPYING", "proot.txt"),
                           ("runtime/NOTICE", "NOTICE"), ("runtime/licenses/GPL-3.0-only.txt", "runtime.txt"),
                           ("runtime/licenses/talloc.txt", "talloc.txt"),
                           ("out/ananbox-native/boost_1_83_0/LICENSE_1_0.txt", "boost.txt")):
        apk.write(ROOT / source, "assets/licenses/" + target)
    notice = out / "reference/system/etc/NOTICE.xml.gz"
    if not notice.is_file():
        raise SystemExit("Missing Android 9 reference license notices")
    apk.write(notice, "assets/licenses/android9-NOTICE.xml.gz")
print("Packed API 28 integration probe:", (out / "packed.apk").stat().st_size, "bytes")
