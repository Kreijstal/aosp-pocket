#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the ordinary installed API 32 test app (no host or system UID)."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import zipfile
from prepare_api32 import ROOT, CACHE


def build_java_app(source,out,package,filename,native_libraries=None):
    tool = ROOT.parent / 'helloworldandroid'
    android = tool / 'vendor/android-platforms/android-36/android.jar'
    r8 = ROOT.parent / 's20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    out.mkdir(parents=True, exist_ok=True)
    for name in ['classes', 'dex', 'manifest-tools']:
        path = out / name
        if path.exists():shutil.rmtree(path)
        path.mkdir()
    def run(*args):subprocess.run(list(map(str, args)), check=True)
    run('javac', '--release', '8', '-cp', android, '-d', out / 'classes', *sorted(source.glob('*.java')))
    run('jar', 'cf', out / 'classes.jar', '-C', out / 'classes', '.')
    run('java', '-cp', r8, 'com.android.tools.r8.D8', '--lib', android, '--min-api', '32', '--output', out / 'dex', out / 'classes.jar')
    run('javac', '--release', '8', '-cp', tool / 'build/tools', '-d', out / 'manifest-tools', ROOT / 'runtime/tools/PocketManifest.java')
    run('java', '-cp', str(tool / 'build/tools')+':'+str(out / 'manifest-tools'), 'PocketManifest',
        source / 'AndroidManifest.xml', out / 'AndroidManifest.xml', '32', '32', '1', '0.1')
    # These applications need no aapt resources. Fixed ZIP metadata
    # keeps the production package observer, and hence its image, reproducible.
    with zipfile.ZipFile(out/'unsigned.apk','w',compression=zipfile.ZIP_STORED) as apk:
        for name,path in [('AndroidManifest.xml',out/'AndroidManifest.xml'),('classes.dex',out/'dex/classes.dex')]:
            entry=zipfile.ZipInfo(name,(1980,1,1,0,0,0));entry.external_attr=0o100644<<16
            apk.writestr(entry,path.read_bytes())
        for name,path in sorted((native_libraries or {}).items()):
            if not name.startswith('lib/arm64-v8a/') or '/' in name[len('lib/arm64-v8a/'):]:
                raise ValueError('Expected a single ARM64 library filename')
            entry=zipfile.ZipInfo(name,(1980,1,1,0,0,0));entry.external_attr=0o100644<<16
            apk.writestr(entry,path.read_bytes())
    signer = ['java', '-cp', str(tool / 'build/apksigner.jar'), 'com.android.apksigner.ApkSignerTool']
    target = out / filename
    run(*signer, 'sign', '--ks', ROOT / 'out/debug.keystore', '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', target, out / 'unsigned.apk')
    run(*signer, 'verify', '--min-sdk-version', '32', '--max-sdk-version', '32', target)
    (out / 'build.json').write_text(json.dumps({'package':package, 'minSdk':32, 'targetSdk':32,
        'apkSha256':hashlib.file_digest(target.open('rb'), 'sha256').hexdigest(), 'apkBytes':target.stat().st_size}, indent=2)+'\n')
    print('API32_GUEST_APP_BUILD_PASS', target)
    return target


def build_guest_app():
    return build_java_app(ROOT/'runtime/tests/api32/guestapp',CACHE/'guest-app',
        'dev.kreijstal.pocketapi32guesttest','guest-test.apk')


def build_guest_bridge():
    return build_java_app(ROOT/'runtime/api32/bridge',CACHE/'guest-bridge',
        'dev.kreijstal.aosppocket.guestbridge','pocket-services.apk')


if __name__ == '__main__':build_guest_app()
