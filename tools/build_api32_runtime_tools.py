#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the version-matched runtime tool classpath used by normal 12L services."""
import argparse
from pathlib import Path
import shutil
import subprocess
import zipfile
from prepare_api32 import ROOT,CACHE,prepare_system_sdk

SOURCES=('UEventProbe.java','FrameworkReadyProbe.java','KeystoreProbe32.java',
    'TraceProbe32.java','TombstonedProbe32.java','AppLifecycleProbe32.java','InputControl32.java',
    'UiRuntimeProbe32.java','BootArtifactsProbe32.java','SystemServerDexopt32.java','HostNetwork32.java')


def build(out):
    out=out.resolve();out.mkdir(parents=True,exist_ok=True)
    android=prepare_system_sdk()
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    for name in ('classes','dex'):
        path=out/name
        if path.exists():shutil.rmtree(path)
        path.mkdir()
    def run(*argv):subprocess.run(list(map(str,argv)),check=True)
    java=[ROOT/'runtime/api32'/name for name in SOURCES]
    java.append(ROOT/'runtime/app/src/dev/kreijstal/aosppocket/bridge/HostWire.java')
    run('javac','--release','8','-cp',android,'-d',out/'classes',*java)
    run('jar','cf',out/'classes.jar','-C',out/'classes','.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','32','--output',out/'dex',out/'classes.jar')
    target=out/'pocket-tools.jar'
    with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as archive:
        entry=zipfile.ZipInfo('classes.dex',(1980,1,1,0,0,0));entry.compress_type=zipfile.ZIP_DEFLATED
        archive.writestr(entry,(out/'dex/classes.dex').read_bytes(),compresslevel=1)
    return target


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,default=CACHE/'runtime-tools')
    args=parser.parse_args();print('API32_RUNTIME_TOOLS_BUILD_PASS',build(args.out))
