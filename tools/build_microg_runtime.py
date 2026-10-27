#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build API-28 zygote compatibility, or narrowly rebuild its microG Java code."""
import argparse
from pathlib import Path
import subprocess
import zipfile
from target import ROOT
from boot_class_manifest import write_manifest

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk',type=Path)
    p.add_argument('--out',type=Path,default=ROOT/'out/microg/runtime')
    p.add_argument('--java-only',action='store_true')
    args=p.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    def run(*args):subprocess.run(list(map(str,args)),cwd=ROOT,check=True)
    if not args.java_only:
        if not args.ndk:p.error('--ndk is required for a native runtime build')
        from build_init_loader import build
        build(args.ndk.resolve(),out,run,framework=True)
    else:
        dest=out/'init-zygote';classes=dest/'classes'
        if not (classes/'dev/kreijstal/aosppocket/guest/ZygoteBootstrap.class').is_file():
            p.error('Run the native runtime build once before --java-only')
        for path in (classes/'dev/kreijstal/aosppocket/guest').glob('MicrogSupport*.class'):path.unlink()
        android=ROOT.parent/'helloworldandroid/vendor/android-platforms/android-36/android.jar'
        r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
        run('javac','--release','8','-cp',android,'-d',classes,ROOT/'runtime/api28/MicrogSupport.java')
        run('jar','cf',dest/'classes.jar','-C',classes,'.')
        run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',dest/'dex',dest/'classes.jar')
        with zipfile.ZipFile(dest/'pocket-zygote.jar','w',zipfile.ZIP_DEFLATED) as z:z.write(dest/'dex/classes.dex','classes.dex')
        write_manifest(dest/'pocket-zygote.jar',dest/'pocket-zygote.classes')
    print('PASS: microG runtime build:',out/'init-zygote')

if __name__=='__main__':main()
