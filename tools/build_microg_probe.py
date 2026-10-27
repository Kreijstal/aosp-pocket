#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build an ordinary Android app testing Pocket's microG package APIs."""
from pathlib import Path
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from target import ROOT

def main():
    out=ROOT/'out/microg/probe';out.mkdir(parents=True,exist_ok=True)
    classes=out/'classes';classes.mkdir(exist_ok=True)
    t=ROOT.parent/'helloworldandroid';android=t/'vendor/android-platforms/android-36/android.jar'
    source=ROOT/'runtime/tests/microg'
    def run(*args):subprocess.run(list(map(str,args)),check=True,cwd=ROOT)
    run('javac','--release','8','-cp',android,'-d',classes,source/'MainActivity.java',source/'ContactsTiming.java')
    run('jar','cf',out/'classes.jar','-C',classes,'.')
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',out,out/'classes.jar')
    run('java','-cp',str(t/'build/tools')+':'+str(ROOT/'out/runtime/tools'),'PocketManifest',source/'AndroidManifest.xml',out/'AndroidManifest.xml','28','28','1','0.1')
    run('java','-cp',t/'build/tools','com.example.build.ApkBuilder',out/'AndroidManifest.xml',out/'classes.dex',out/'unsigned.apk')
    signer=['java','-cp',t/'build/apksigner.jar','com.android.apksigner.ApkSignerTool']
    run(*signer,'sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android','--out',out/'microg-api-probe.apk',out/'unsigned.apk')
    run(*signer,'verify',out/'microg-api-probe.apk')
    owner=out/'owner-classes';owner.mkdir(exist_ok=True)
    run('javac','--release','8','-cp',android,'-d',owner,source/'MicrogOwner.java')
    run('jar','cf',out/'owner-classes.jar','-C',owner,'.')
    dex=out/'owner-dex';dex.mkdir(exist_ok=True)
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',dex,out/'owner-classes.jar')
    run('jar','cf',out/'microg-owner.jar','-C',dex,'classes.dex')
    print('PASS:',out/'microg-api-probe.apk')

if __name__=='__main__':main()
