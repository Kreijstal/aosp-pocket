#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Add guest audio output to a byte-verified Manager: host broker/media classes and the guest audio payload."""
import argparse
from pathlib import Path
import shutil
import subprocess
import zipfile
from target import ROOT
from build_guest_audio import build as build_audio

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-apk',type=Path,required=True)
    p.add_argument('--base-classes',type=Path,required=True)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--version',required=True)
    args=p.parse_args();out=ROOT/('out/audio-'+args.version);out.mkdir(parents=True,exist_ok=True)
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
    sources=ROOT/'runtime/app/src/dev/kreijstal/aosppocket'
    for name in ('runtime/HostBroker','runtime/HostMedia','bridge/HostWire'):
        for file in (classes/'dev/kreijstal/aosppocket').glob(name+'*.class'):file.unlink()
    run('javac','--release','8','-cp',str(android)+':'+str(classes),'-d',classes,
        *[sources/(name+'.java') for name in ('runtime/HostBroker','runtime/HostMedia','bridge/HostWire')])
    updated=dex(classes,'host')
    payload=build_audio(args.base_apk.resolve(),args.ndk.resolve(),out/'guest-audio')
    # Codec binaries are already in the base; add only audio files and the rebuilt HIDL library/init script.
    wanted=[k for k in payload if k.startswith(('vendor/lib64/hw/','vendor/etc/audio','vendor/etc/default_volume','vendor/lib64/libeffects'))
            and 'media_codecs' not in k]+['system/bin/audioserver','system/lib64/libaudioflinger.so','system/lib64/libpocket_hidl_api28.so','system/etc/init/init.syscall.rc']
    files=[a for k in wanted for a in ('--guest-file',k+'='+payload[k])]
    run('python3',ROOT/'tools/build_manager_update.py','--base-apk',args.base_apk.resolve(),'--out',out,
        '--host-dex',updated,*files,'--version-code',args.version.split('.')[-1],'--version-name',args.version)
    print('PASS: host broker/media/wire classes and guest audio files replaced:',' '.join(sorted(wanted)))

if __name__=='__main__':main()
