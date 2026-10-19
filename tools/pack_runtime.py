#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package the separately built GPL runtime, keeping the bootstrap APK separate."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--guest", type=Path, required=True)
    args = parser.parse_args()
    out = ROOT / "out/runtime"
    with zipfile.ZipFile(out / "base.apk") as base, zipfile.ZipFile(
            out / "packed.apk", "w", zipfile.ZIP_DEFLATED, compresslevel=1) as apk:
        for entry in base.infolist():
            apk.writestr(entry, base.read(entry.filename))
        for name in ("libpocket_runtime.so", "libproot.so"):
            copied = out / name
            shutil.copy2(ROOT / "out/runtime-native" / name, copied)
            subprocess.run([str(args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"), str(copied)], check=True)
            apk.write(copied, "lib/arm64-v8a/" + name)
        for source, target in [
            ("runtime/guest.sh", "guest.sh"),
            ("out/runtime-native/libpocket_binder_bootstrap.so", "guest-binder-bootstrap.so"),
            ("sources.lock.json", "sources.lock.json"),
            ("runtime/licenses/GPL-3.0-only.txt", "licenses/anbox-and-runtime.txt"),
            ("LICENSE", "licenses/apache-2.0.txt"),
            ("runtime/NOTICE", "licenses/NOTICE"),
            ("vendor/proot/COPYING", "licenses/proot.txt"),
            ("runtime/licenses/talloc.txt", "licenses/talloc.txt"),
            ("out/ananbox-native/boost_1_83_0/LICENSE_1_0.txt", "licenses/boost.txt"),
        ]:
            apk.write(ROOT / source, "assets/" + target)
        # Place individual compressed guest entries inside the APK, allowing
        # Android to install directly from it without staging another 266 MiB ZIP.
        with zipfile.ZipFile(args.guest) as guest:
            layout = json.loads(guest.read("pocket-layout.json"))
            if layout['format'] != 1 or layout['abi'] != 'arm64-v8a':
                raise SystemExit('Unsupported bundled guest format/ABI')
            if layout.get('guest_sdk', 25) != 25:
                raise SystemExit('The bootable runtime requires API 25; use the separate API 28 bring-up target')
            for entry in guest.infolist():
                # Android scans *all* APK paths ending in .bc and forces a
                # 32-bit host ABI for legacy RenderScript. Keep guest names
                # opaque to that scanner, restoring them only during extraction.
                stored = entry.filename + ('' if entry.filename == 'pocket-layout.json' else '.payload')
                with guest.open(entry) as source, apk.open('assets/default-guest/' + stored, 'w') as target:
                    shutil.copyfileobj(source, target, 65536)
        print('Bundled default guest:', args.guest)

if __name__ == "__main__":
    main()
