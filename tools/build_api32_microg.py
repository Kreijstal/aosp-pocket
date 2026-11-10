#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build Android 12L microG package-query compatibility.

Outputs, in --out (default out/api32/microg):
  libpocket_zygote_api32.so  the 12L zygote JNI library, whose system_server
                             child calls runtime/api32/microg_bootstrap32.cpp
  pocket-microg.jar          runtime/api32/MicrogSupport32.java, loaded only
                             by that hook (not on BOOTCLASSPATH)

The hook is linked by tools/build_api32_native.py's normal zygote build. --zygote-patch builds from another
copy of runtime/api32/zygote-rootless.patch (for example the one a shipped
image was built from).
"""
import argparse
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_api32_native as native  # noqa: E402
from prepare_api32 import ROOT, CACHE  # noqa: E402


def build_zygote(ndk, patch):
    real = subprocess.run

    def run(command, *args, **kwargs):
        command = [str(patch) if str(part).endswith('runtime/api32/zygote-rootless.patch') else part
                   for part in command]
        result = real(command, *args, **kwargs)
        return result

    native.subprocess.run = run
    try:
        out = native.build_zygote(ndk)
    finally:
        native.subprocess.run = real
    if b'pocketSystemServerMicrog32' not in (out / 'libpocket_zygote_api32.so').read_bytes():
        raise RuntimeError('microG hook was not compiled')
    return out / 'libpocket_zygote_api32.so'


def build_jar(work):
    android = ROOT.parent / 'helloworldandroid/vendor/android-platforms/android-36/android.jar'
    r8 = ROOT.parent / 's20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    work.mkdir(parents=True, exist_ok=True)
    classes, dex = work / 'classes', work / 'dex'
    for path in (classes, dex):
        shutil.rmtree(path, ignore_errors=True)
        path.mkdir(parents=True)
    subprocess.run(['javac', '--release', '8', '-cp', str(android), '-d', str(classes),
                    str(ROOT / 'runtime/api32/MicrogSupport32.java')], check=True)
    subprocess.run(['jar', 'cf', str(work / 'classes.jar'), '-C', str(classes), '.'], check=True)
    subprocess.run(['java', '-cp', str(r8), 'com.android.tools.r8.D8', '--lib', str(android),
                    '--min-api', '32', '--output', str(dex), str(work / 'classes.jar')], check=True)
    jar = work / 'pocket-microg.jar'
    with zipfile.ZipFile(jar, 'w', zipfile.ZIP_DEFLATED) as archive:
        info = zipfile.ZipInfo('classes.dex', date_time=(1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, (dex / 'classes.dex').read_bytes())
    return jar


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--zygote-patch', type=Path, default=ROOT / 'runtime/api32/zygote-rootless.patch')
    parser.add_argument('--out', type=Path, default=CACHE / 'microg')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    library = build_zygote(args.ndk.resolve(), args.zygote_patch.resolve())
    shutil.copy2(library, args.out / 'libpocket_zygote_api32.so')
    jar = build_jar(args.out)
    print('API32_MICROG_BUILD_PASS', args.out / 'libpocket_zygote_api32.so', jar)
