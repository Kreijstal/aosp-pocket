#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Rebuild encrypted storage against a byte-verified cached Manager Java build."""
import argparse
from pathlib import Path
import shutil
import subprocess
import zipfile
from target import ROOT

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-apk',type=Path,required=True)
    p.add_argument('--base-classes',type=Path,required=True)
    p.add_argument('--version',required=True)
    args=p.parse_args();out=ROOT/('out/manager-'+args.version);out.mkdir(parents=True,exist_ok=True)
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
    sources=[]
    for package,name in [('storage','EncryptedStore'),('storage','CeNamespace'),('runtime','CeStorage')]:
        for file in (classes/('dev/kreijstal/aosppocket/'+package)).glob(name+'*.class'):file.unlink()
        directory='runtime/storage/src' if package=='storage' else 'runtime/app/src'
        sources.append(ROOT/directory/('dev/kreijstal/aosppocket/'+package)/(name+'.java'))
    run('javac','--release','8','-cp',str(android)+':'+str(classes),'-d',classes,*sources)
    updated=dex(classes,'host')
    run('python3',ROOT/'tools/build_manager_update.py','--base-apk',args.base_apk.resolve(),'--out',out,
        '--host-dex',updated,'--version-code',args.version.split('.')[-1],'--version-name',args.version)
    print('PASS: only EncryptedStore, CeNamespace and CeStorage class files replaced; guest image and native libraries retained')

if __name__=='__main__':main()
