#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Rebuild only host input classes, retaining the installed image's other code."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import zipfile
from target import ROOT


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-apk',type=Path,default=ROOT/'out/keypad-0.74/aosp-pocket-manager-0.74.apk')
    parser.add_argument('--out',type=Path,default=ROOT/'out/input-queue')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    tool=ROOT.parent/'helloworldandroid'
    android=tool/'vendor/android-platforms/android-36/android.jar'
    prebuilts=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts'
    r8=prebuilts/'r8/r8.jar';smali=prebuilts/'extract-tools/common/smali'
    def run(*argv):subprocess.run(list(map(str,argv)),check=True,cwd=ROOT)
    def fresh(name):
        path=out/name
        if path.exists():shutil.rmtree(path)
        path.mkdir();return path
    def compile_java(name,sources):
        classes=fresh(name+'-classes');dex=fresh(name+'-dex')
        run('javac','--release','8','-cp',str(android)+':'+str(ROOT/'out/runtime/classes'),'-d',classes,*sources)
        jar=out/(name+'-classes.jar');run('jar','cf',jar,'-C',classes,'.')
        run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28',
            '--classpath',ROOT/'out/runtime/classes','--output',dex,jar)
        return dex/'classes.dex'
    def disassemble(dex,name):
        path=fresh(name);run('java','-jar',smali/'baksmali.jar','disassemble',dex,'-o',path);return path
    with zipfile.ZipFile(args.base_apk) as apk:(out/'base.dex').write_bytes(apk.read('classes.dex'))
    base=disassemble(out/'base.dex','base-smali')
    source=ROOT/'runtime/app/src/dev/kreijstal/aosppocket/runtime'
    patch=disassemble(compile_java('optimized',[source/'DirectSession.java',source/'InputCommandQueue.java']),'optimized-smali')
    merged=fresh('merged-smali');shutil.copytree(base,merged,dirs_exist_ok=True)
    package=Path('dev/kreijstal/aosppocket/runtime')
    for old in (merged/package).glob('DirectSession*.smali'):old.unlink()
    for path in patch.rglob('*.smali'):
        assert path.name.startswith(('DirectSession','InputCommandQueue')),path
        shutil.copyfile(path,merged/path.relative_to(patch))
    run('java','-jar',smali/'smali.jar','assemble',merged,'-o',out/'optimized-host.dex')
    audited=disassemble(out/'optimized-host.dex','audited-smali')
    changed=[]
    for path in base.rglob('*.smali'):
        relative=path.relative_to(base);actual=audited/relative
        if not actual.exists() or actual.read_bytes()!=path.read_bytes():
            assert relative.parent==package and path.name.startswith('DirectSession'),relative
            changed.append(str(relative))
    added=[str(p.relative_to(audited)) for p in audited.rglob('*.smali') if not (base/p.relative_to(audited)).exists()]
    assert all(Path(p).parent==package and Path(p).name.startswith(('DirectSession','InputCommandQueue')) for p in added)
    (out/'host-code-audit.json').write_text(json.dumps({'validation':'PASS','changedClasses':changed,'addedClasses':added},indent=2)+'\n')
    test=disassemble(compile_java('fixture',[ROOT/'runtime/tests/input-latency/InputQueueTest.java']),'fixture-smali')
    signer=['java','-cp',tool/'build/apksigner.jar','com.android.apksigner.ApkSignerTool']
    for version,(label,production) in enumerate([('baseline',base),('optimized',audited)],1):
        combined=fresh(label+'-probe-smali');shutil.copytree(production,combined,dirs_exist_ok=True)
        for path in test.rglob('*.smali'):
            destination=combined/path.relative_to(test);assert not destination.exists(),destination
            destination.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,destination)
        dex=out/(label+'-probe.dex');run('java','-jar',smali/'smali.jar','assemble',combined,'-o',dex)
        manifest=out/(label+'-manifest.bin')
        run('java','-cp',str(tool/'build/tools')+':'+str(ROOT/'out/runtime/tools'),'PocketManifest',
            ROOT/'runtime/tests/input-latency/queue-manifest.xml',manifest,'28','28',version,'0.'+str(version))
        unsigned=out/(label+'-probe-unsigned.apk');apk=out/(label+'-probe.apk')
        run('java','-cp',tool/'build/tools','com.example.build.ApkBuilder',manifest,dex,unsigned)
        run(*signer,'sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android','--out',apk,unsigned)
        run(*signer,'verify',apk)
    print('PASS: focused host DEX audit and baseline/optimized queue probes',out)


if __name__=='__main__':main()
