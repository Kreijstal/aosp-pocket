#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the API32-specific external Manager operations, outside the boot classpath."""
import argparse
from pathlib import Path
import shutil
import subprocess
import zipfile
from prepare_api32 import ROOT,CACHE


def build(out):
    out=out.resolve();out.mkdir(parents=True,exist_ok=True)
    android=ROOT.parent/'helloworldandroid/vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    for name in ('classes','dex'):
        directory=out/name
        if directory.exists():shutil.rmtree(directory)
        directory.mkdir()
    def run(*argv):subprocess.run(list(map(str,argv)),check=True)
    run('javac','--release','8','-cp',android,'-d',out/'classes',ROOT/'runtime/api32/ManagerTools32.java',
        ROOT/'runtime/api32/StartupReadiness32.java',
        ROOT/'runtime/api32/PackageEvents32.java',
        ROOT/'runtime/api32/GuestCredential32.java',
        ROOT/'runtime/direct/HostDevices.java',ROOT/'runtime/app/src/dev/kreijstal/aosppocket/bridge/HostWire.java')
    run('jar','cf',out/'classes.jar','-C',out/'classes','.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','32','--output',out/'dex',out/'classes.jar')
    jar=out/'pocket-manager.jar'
    with zipfile.ZipFile(jar,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as archive:
        entry=zipfile.ZipInfo('classes.dex',(1980,1,1,0,0,0));entry.compress_type=zipfile.ZIP_DEFLATED
        archive.writestr(entry,(out/'dex/classes.dex').read_bytes(),compresslevel=1)
    return jar


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,default=CACHE/'manager-tools')
    args=p.parse_args();print('API32_MANAGER_TOOLS_BUILD_PASS',build(args.out))
