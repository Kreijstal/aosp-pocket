#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

parser = argparse.ArgumentParser(description="Validate the isolated controlled native guest APK")
parser.add_argument("apk", type=Path)
parser.add_argument("--ndk", type=Path, required=True)
args = parser.parse_args()
readelf = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
with zipfile.ZipFile(args.apk) as apk, tempfile.TemporaryDirectory() as tmp:
    assert len(apk.namelist()) == len(set(apk.namelist()))
    manifest = apk.read("AndroidManifest.xml")
    assert "dev.kreijstal.aosppocket.directprobe".encode("utf-16le") in manifest
    assert "android.permission.".encode("utf-16le") not in manifest
    prefix = "assets/direct-rootfs/"
    names = {name[len(prefix):-8] for name in apk.namelist() if name.startswith(prefix)}
    assert names == {"system/bin/pocket-direct-probe", "system/bin/linker64", "system/lib64/libc.so", "system/lib64/libdl.so", "system/lib64/libm.so", "system/build.prop"}, names
    needed = set()
    for name in names - {"system/build.prop"}:
        file = Path(tmp) / Path(name).name
        file.write_bytes(apk.read(prefix + name + ".payload"))
        elf = subprocess.check_output([readelf, "-hldW", file], text=True)
        assert "AArch64" in elf
        needed.update(re.findall(r"Shared library: \[([^]]+)\]", elf))
        if name.endswith("pocket-direct-probe"):
            assert "/pocket/" + "x" * 504 in elf
            assert all(int(line.split()[-1], 16) >= 16384 for line in elf.splitlines() if line.strip().startswith("LOAD "))
    # Android 9 libdl resolves ld-android.so against the loader itself.
    assert "Library soname: [ld-android.so]" in subprocess.check_output(
        [readelf, "-d", Path(tmp) / "linker64"], text=True)
    assert needed <= {"libc.so", "libm.so", "libdl.so", "ld-android.so"}, needed
    assert "ro.build.version.sdk=28\n" in apk.read(prefix + "system/build.prop.payload").decode()
    assert len(apk.read("assets/licenses/android9-NOTICE.xml.gz")) > 1000
    assert b"LESSER GENERAL PUBLIC LICENSE" in apk.read("assets/licenses/talloc.txt")
print("PASS: separate permission-free ARM64 direct probe; pinned Android 9 linker/bionic closure and reserved interpreter")
