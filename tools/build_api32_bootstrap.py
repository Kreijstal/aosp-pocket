#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the small 12L bootclasspath shim separately from runtime tools."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import zipfile
from prepare_api32 import ROOT,CACHE,prepare_system_sdk

def build(out):
    out=out.resolve();out.mkdir(parents=True,exist_ok=True)
    android=prepare_system_sdk()
    for name in ['classes','dex']:
        path=out/name
        if path.exists():shutil.rmtree(path)
        path.mkdir()
    def run(*args):subprocess.run(list(map(str,args)),check=True)
    sources=[ROOT/'runtime/api32'/name for name in ['ZygoteBootstrap32.java','Api32Child.java']]
    run('javac','--release','8','-cp',android,'-d',out/'classes',*sources)
    run('jar','cf',out/'classes.jar','-C',out/'classes','.')
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','32','--output',out/'dex',out/'classes.jar')
    target=out/'pocket-zygote.jar'
    entry=zipfile.ZipInfo('classes.dex',(1980,1,1,0,0,0))
    entry.create_system=3;entry.external_attr=0o100644<<16
    entry.extra=struct.pack('<HH',0xffff,3)+bytes(3)
    with zipfile.ZipFile(target,'w') as z:z.writestr(entry,(out/'dex/classes.dex').read_bytes())
    return target

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,default=CACHE/'bootstrap')
    args=parser.parse_args();print('API32_BOOTSTRAP_BUILD_PASS',build(args.out))
