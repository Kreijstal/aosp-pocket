#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package a source-pinned host PRoot regression in an ordinary Android app."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile
from target import ROOT
from build_host_native import load_manifest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host-native-dir', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    native = load_manifest(args.host_native_dir)
    source = ROOT / 'runtime/tests/host-native'
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for name in ['classes', 'dex', 'manifest-tools']:
        (out / name).mkdir(exist_ok=True)
    tool = ROOT.parent / 'helloworldandroid'
    android = tool / 'vendor/android-platforms/android-36/android.jar'
    r8 = ROOT.parent / 's20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    def run(*cmd):
        subprocess.run(list(map(str, cmd)), check=True)
    run('javac', '--release', '8', '-cp', android, '-d', out / 'classes', source / 'MainActivity.java')
    run('jar', 'cf', out / 'classes.jar', '-C', out / 'classes', '.')
    run('java', '-cp', r8, 'com.android.tools.r8.D8', '--lib', android, '--min-api', '28', '--output', out / 'dex', out / 'classes.jar')
    run('javac', '--release', '8', '-cp', tool / 'build/tools', '-d', out / 'manifest-tools', ROOT / 'runtime/tools/PocketManifest.java')
    run('java', '-cp', str(tool / 'build/tools') + ':' + str(out / 'manifest-tools'), 'PocketManifest', source / 'AndroidManifest.xml', out / 'AndroidManifest.xml', '28', '28', '1', '0.1')
    with zipfile.ZipFile(out / 'unsigned.apk', 'w', zipfile.ZIP_STORED) as z:
        for name, path in [('AndroidManifest.xml', out / 'AndroidManifest.xml'), ('classes.dex', out / 'dex/classes.dex'), ('lib/arm64-v8a/libproot.so', args.host_native_dir / 'libproot.so')]:
            z.write(path, name)
    signer = ['java', '-cp', str(tool / 'build/apksigner.jar'), 'com.android.apksigner.ApkSignerTool']
    apk = out / 'host-proot-probe.apk'
    run(*signer, 'sign', '--ks', ROOT / 'out/debug.keystore', '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', apk, out / 'unsigned.apk')
    run(*signer, 'verify', '--min-sdk-version', '28', apk)
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    with zipfile.ZipFile(apk) as z:
        assert hashlib.sha256(z.read('lib/arm64-v8a/libproot.so')).hexdigest() == native['libraries']['libproot.so']['sha256']
    pins = [source / 'MainActivity.java', source / 'AndroidManifest.xml', ROOT / 'tools/build_host_proot_probe.py', ROOT / 'runtime/tools/PocketManifest.java']
    (out / 'build.json').write_text(json.dumps({'package': 'dev.kreijstal.pockethostnativeprobe', 'minSdk': 28, 'targetSdk': 28, 'apkSha256': digest(apk), 'proot': native['libraries']['libproot.so'], 'hostNativeSourceBuild': native, 'sources': {p.relative_to(ROOT).as_posix(): digest(p) for p in pins}, 'scope': 'Non-debuggable ordinary host app, source-built packaged PRoot, fake UID/path binding/nested override/fork-exec and wrong-parent rejection. No guest image, Android 7/9/10 boot or legacy HostBroker integration claim.', 'runtimeValidated': False}, indent=2) + '\n')
    print('HOST_PROOT_PROBE_BUILD_PASS', apk)


if __name__ == '__main__':
    main()
