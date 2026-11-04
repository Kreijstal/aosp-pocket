#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build Android 12L microG package-query compatibility.

Outputs, in --out (default out/api32/microg):
  libpocket_zygote_api32.so  the 12L zygote JNI library, whose system_server
                             child calls runtime/api32/microg_bootstrap32.cpp
  pocket-microg.jar          runtime/api32/MicrogSupport32.java, loaded only
                             by that hook (not on BOOTCLASSPATH)

The zygote is tools/build_api32_native.py's build_zygote with one edit to the
patched com_android_internal_os_Zygote.cpp. --zygote-patch builds from another
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

HOOK = 'pocketSystemServerMicrog32'


def add_hook(source):
    path = source / 'core/jni/com_android_internal_os_Zygote.cpp'
    text = path.read_text()
    server = 'static jint com_android_internal_os_Zygote_nativeForkSystemServer('
    start = text.index(server)
    end = text.index('\n}\n', start)
    block = text[start:end]
    if block.count('  return pid;') != 1:
        raise RuntimeError('Unexpected system-server fork body')
    block = block.replace('  return pid;', f'  if (pid == 0) {HOOK}(env);\n  return pid;', 1)
    text = text[:start] + block + text[end:]
    if text.count('namespace {') < 1:
        raise RuntimeError('Unexpected zygote source layout')
    text = text.replace('namespace {', f'extern "C" void {HOOK}(JNIEnv*);\nnamespace {{', 1)
    text += '\n' + (ROOT / 'runtime/api32/microg_bootstrap32.cpp').read_text()
    path.write_text(text)


def build_zygote(ndk, patch):
    real = subprocess.run

    def run(command, *args, **kwargs):
        command = [str(patch) if str(part).endswith('runtime/api32/zygote-rootless.patch') else part
                   for part in command]
        result = real(command, *args, **kwargs)
        # startup-trace.patch is the last source patch before compilation.
        if any(str(part).endswith('runtime/api32/startup-trace.patch') for part in command):
            add_hook(Path(kwargs['cwd']))
        return result

    native.subprocess.run = run
    try:
        out = native.build_zygote(ndk)
    finally:
        native.subprocess.run = real
    source = out / 'zygote-source/core/jni/com_android_internal_os_Zygote.cpp'
    if HOOK not in source.read_text():
        raise RuntimeError('microG hook was not compiled')
    return out / 'libpocket_zygote_api32.so'


def build_jar(work):
    android = ROOT.parent / 'helloworldandroid/vendor/android-platforms/android-36/android.jar'
    r8 = ROOT.parent / 's20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
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
