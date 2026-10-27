#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the isolated ordinary-app AppFuse/mmap feasibility probe."""
import argparse
from pathlib import Path
import shutil
import subprocess
import zipfile
from target import ROOT


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,required=True)
    args=parser.parse_args()
    out=ROOT/'out/storage-probe';out.mkdir(exist_ok=True)
    source=ROOT/'runtime/tests/storage-probe'
    toolchain=ROOT.parent/'helloworldandroid'
    android=toolchain/'vendor/android-platforms/android-36/android.jar'
    def run(*argv):subprocess.run(list(map(str,argv)),cwd=ROOT,check=True)
    for name in ('classes','tools'):
        shutil.rmtree(out/name,ignore_errors=True)
        (out/name).mkdir()
    clang=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    run(clang,'-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-pthread',
        '-Wl,-z,max-page-size=16384',source/'probe.c','-o',out/'libpocketstorageprobe.so')
    run('javac','--release','8','-cp',android,'-d',out/'classes',source/'MainActivity.java',
        ROOT/'runtime/storage/src/dev/kreijstal/aosppocket/storage/EncryptedStore.java',
        ROOT/'runtime/tests/EncryptedStoreTest.java')
    run('java','-cp',toolchain/'build/dx.jar','com.android.dx.command.Main',
        '--dex','--output='+str(out/'classes.dex'),out/'classes')
    run('javac','--release','8','-cp',toolchain/'build/tools','-d',out/'tools',ROOT/'runtime/tools/PocketManifest.java')
    run('java','-cp',str(toolchain/'build/tools')+':'+str(out/'tools'),'PocketManifest',
        source/'AndroidManifest.xml',out/'AndroidManifest.xml','28','28','1','0.1')
    run('java','-cp',toolchain/'build/tools','com.example.build.ApkBuilder',
        out/'AndroidManifest.xml',out/'classes.dex',out/'unsigned.apk')
    with zipfile.ZipFile(out/'unsigned.apk','a',zipfile.ZIP_DEFLATED) as apk:
        apk.write(out/'libpocketstorageprobe.so','lib/arm64-v8a/libpocketstorageprobe.so')
    signer=['java','-cp',toolchain/'build/apksigner.jar','com.android.apksigner.ApkSignerTool']
    run(*signer,'sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android',
        '--out',out/'storage-probe.apk',out/'unsigned.apk')
    run(*signer,'verify',out/'storage-probe.apk')
    print('PASS: isolated storage-probe.apk built; Manager and guest data unchanged')


if __name__=='__main__':main()
