#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Add the guest Vibrator HAL to a byte-verified Manager: HostBroker and the guest HIDL library only."""
import argparse
from pathlib import Path
import shutil
import subprocess
import zipfile
from target import ROOT
from build_guest_media import build as build_media

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-apk',type=Path,required=True)
    p.add_argument('--base-classes',type=Path,required=True)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--version',required=True)
    args=p.parse_args();out=ROOT/('out/vibrator-'+args.version);out.mkdir(parents=True,exist_ok=True)
    android=ROOT.parent/'helloworldandroid/vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    def run(*argv):subprocess.run(list(map(str,argv)),cwd=ROOT,check=True)
    def dex(classes,tag):
        jar=out/(tag+'-classes.jar');dest=out/(tag+'-dex');dest.mkdir(exist_ok=True)
        run('jar','cf',jar,'-C',classes,'.')
        run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',dest,jar)
        return dest/'classes.dex'
    baseline=dex(args.base_classes.resolve(),'baseline')
    with zipfile.ZipFile(args.base_apk) as apk:
        if baseline.read_bytes()!=apk.read('classes.dex'):p.error('Cached classes do not reproduce the base APK DEX; refusing unrelated changes')
    classes=out/'host-classes';shutil.rmtree(classes,ignore_errors=True);shutil.copytree(args.base_classes,classes)
    package=classes/'dev/kreijstal/aosppocket/runtime'
    for file in package.glob('HostBroker*.class'):file.unlink()
    run('javac','--release','8','-cp',str(android)+':'+str(classes),'-d',classes,
        ROOT/'runtime/app/src/dev/kreijstal/aosppocket/runtime/HostBroker.java')
    updated=dex(classes,'host')
    library='system/lib64/libpocket_hidl_api28.so'
    payload=build_media(args.base_apk.resolve(),args.ndk.resolve(),out/'guest-media')
    run('python3',ROOT/'tools/build_manager_update.py','--base-apk',args.base_apk.resolve(),'--out',out,
        '--host-dex',updated,'--guest-file',library+'='+payload[library],
        '--version-code',args.version.split('.')[-1],'--version-name',args.version)
    print('PASS: only HostBroker classes and '+library+' replaced; other guest files and native libraries retained')

if __name__=='__main__':main()
