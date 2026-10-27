#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build owner-side certificate/data checks and positive/negative shared-UID APKs."""
import argparse
from pathlib import Path
import subprocess
from target import ROOT

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--key-dir',type=Path,required=True)
    p.add_argument('--test-keys',type=Path,required=True)
    args=p.parse_args();out=ROOT/'out/guest-signing-probe';out.mkdir(exist_ok=True)
    tool=ROOT.parent/'helloworldandroid';android=tool/'vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    def run(*argv):subprocess.run(list(map(str,argv)),check=True,cwd=ROOT)
    classes=out/'classes';classes.mkdir(exist_ok=True)
    run('javac','--release','8','-cp',android,'-d',classes,ROOT/'runtime/tests/GuestSigningProbe.java')
    run('jar','cf',out/'classes.jar','-C',classes,'.')
    dex=out/'dex';dex.mkdir(exist_ok=True)
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',dex,out/'classes.jar')
    run('jar','cf',out/'guest-signing-probe.jar','-C',dex,'classes.dex')
    helpers=out/'tools';helpers.mkdir(exist_ok=True)
    run('javac','--release','8','-cp',str(android)+':'+str(tool/'build/tools'),'-d',helpers,
        ROOT/'runtime/tools/PocketManifest.java',ROOT/'runtime/tests/SigningFixtureManifest.java')
    for name,keys in [('oldkey',args.test_keys),('newkey',args.key_dir)]:
        manifest=out/(name+'.xml')
        manifest.write_text('<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
            'package="dev.kreijstal.pocketresign.'+name+'" android:sharedUserId="android.uid.system">'
            '<application android:hasCode="false" android:label="Signing fixture" /></manifest>')
        binary=out/(name+'-manifest.bin');unsigned=out/(name+'-unsigned.apk');apk=out/(name+'.apk')
        run('java','-cp',str(tool/'build/tools')+':'+str(helpers),'SigningFixtureManifest',manifest,binary,'28','28','1','0.1')
        run('java','-cp',tool/'build/tools','com.example.build.ApkBuilder',binary,dex/'classes.dex',unsigned)
        signer=['java','-cp',tool/'build/apksigner.jar','com.android.apksigner.ApkSignerTool']
        run(*signer,'sign','--key',keys/'platform.pk8','--cert',keys/'platform.x509.pem',
            '--min-sdk-version','28','--v4-signing-enabled','false','--out',apk,unsigned)
        run(*signer,'verify','--min-sdk-version','28','--max-sdk-version','28',apk)
    print('PASS: owner signing/data probe and old/new shared-UID fixtures',out)

if __name__=='__main__':main()
