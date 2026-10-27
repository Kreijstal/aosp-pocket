#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build an image-free Manager while preserving the tested native runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import urllib.parse
import zipfile
from target import ROOT

NAMES=('DirectImage','DirectSession','GuestImage','GuestService','MainActivity',
       'ImageDownload','ImageArchive','RemoteImage','InputCommandQueue')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-apk',type=Path,default=ROOT/'out/input-0.75/aosp-pocket-manager-0.75.apk')
    parser.add_argument('--catalog',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--probe',action='store_true',help='Isolated Pi test APK with loopback HTTP enabled')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    catalog=json.loads(args.catalog.read_text());assert catalog['schema']==1 and catalog['images']
    for image in catalog['images']:
        url=urllib.parse.urlsplit(image['url'])
        assert url.scheme=='https' or (args.probe and url.scheme=='http' and url.hostname=='127.0.0.1'),'Production images require HTTPS'
        assert image['profile']=='api28-direct' and image['sdk']==28 and image['runtime']==1 and image['abi']=='arm64-v8a'
        assert len(image['sha256'])==64 and image['bytes']>0 and image['manifest']['entries']
    tool=ROOT.parent/'helloworldandroid';android=tool/'vendor/android-platforms/android-36/android.jar'
    prebuilts=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts'
    smali=prebuilts/'extract-tools/common/smali';r8=prebuilts/'r8/r8.jar'
    def run(*argv):subprocess.run(list(map(str,argv)),check=True,cwd=ROOT)
    def fresh(name):
        path=out/name
        if path.exists():shutil.rmtree(path)
        path.mkdir();return path
    classes=fresh('classes');dex=fresh('dex');patch=fresh('patch-smali');base=fresh('base-smali')
    source=ROOT/'runtime/app/src/dev/kreijstal/aosppocket/runtime'
    sources=[source/(name+'.java') for name in NAMES]
    if args.probe:sources.append(ROOT/'runtime/tests/image-download/ImageInstallProbe.java')
    run('javac','--release','8','-cp',str(android)+':'+str(ROOT/'out/runtime/classes'),'-d',classes,*sources)
    run('jar','cf',out/'classes.jar','-C',classes,'.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--classpath',ROOT/'out/runtime/classes','--output',dex,out/'classes.jar')
    with zipfile.ZipFile(args.base_apk) as apk:(out/'base.dex').write_bytes(apk.read('classes.dex'))
    run('java','-jar',smali/'baksmali.jar','disassemble',out/'base.dex','-o',base)
    original={str(p.relative_to(base)):hashlib.sha256(p.read_bytes()).hexdigest() for p in base.rglob('*.smali')}
    run('java','-jar',smali/'baksmali.jar','disassemble',dex/'classes.dex','-o',patch)
    package=Path('dev/kreijstal/aosppocket/runtime');allowed=NAMES+(('ImageInstallProbe',) if args.probe else ())
    for path in (base/package).glob('*.smali'):
        if path.name.split('$')[0].removesuffix('.smali') in allowed:path.unlink()
    for path in patch.rglob('*.smali'):
        assert path.parent.relative_to(patch)==package and path.name.split('$')[0].removesuffix('.smali') in allowed,path
        shutil.copyfile(path,base/path.relative_to(patch))
    run('java','-jar',smali/'smali.jar','assemble',base,'-o',out/'host.dex')
    audit=fresh('audit-smali')
    run('java','-jar',smali/'baksmali.jar','disassemble',out/'host.dex','-o',audit)
    for name,sha in original.items():
        if Path(name).name.split('$')[0].removesuffix('.smali') not in allowed:
            assert hashlib.sha256((audit/name).read_bytes()).hexdigest()==sha,'Unrelated host class changed: '+name
    helpers=fresh('manifest-tools')
    run('javac','--release','8','-cp',tool/'build/tools','-d',helpers,ROOT/'runtime/tools/PocketManifest.java')
    manifest=ROOT/('runtime/tests/image-download/AndroidManifest.xml' if args.probe else 'runtime/app/AndroidManifest.xml')
    run('java','-cp',str(tool/'build/tools')+':'+str(helpers),'PocketManifest',manifest,out/'AndroidManifest.xml','28','28',
        '1' if args.probe else '76','0.1' if args.probe else '0.76')
    removed=[]
    with zipfile.ZipFile(args.base_apk) as before,zipfile.ZipFile(out/'unsigned.apk','w',zipfile.ZIP_DEFLATED,compresslevel=1) as after:
        for entry in before.infolist():
            if entry.filename.startswith(('assets/api28-rootfs/','assets/default-guest/')):
                removed.append(entry.filename);continue
            if entry.filename in ('classes.dex','AndroidManifest.xml','assets/pocket-images.json') or entry.filename.startswith('META-INF/'):continue
            after.writestr(entry,before.read(entry))
        for name,path in [('classes.dex',out/'host.dex'),('AndroidManifest.xml',out/'AndroidManifest.xml')]:after.write(path,name)
        after.writestr('assets/pocket-images.json',args.catalog.read_bytes())
    target=out/('image-install-probe.apk' if args.probe else 'aosp-pocket-manager-0.76.apk')
    signer=['java','-cp',tool/'build/apksigner.jar','com.android.apksigner.ApkSignerTool']
    run(*signer,'sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android','--out',target,out/'unsigned.apk')
    run(*signer,'verify','--verbose',target)
    with zipfile.ZipFile(args.base_apk) as before,zipfile.ZipFile(target) as after:
        assert not any(n.startswith(('assets/default-guest/','assets/api28-rootfs/')) for n in after.namelist())
        for name in before.namelist():
            if name.startswith('lib/'):assert before.read(name)==after.read(name),'Native runtime changed: '+name
    report={'validation':'PASS','apk':str(target),'bytes':target.stat().st_size,
            'sha256':hashlib.file_digest(target.open('rb'),'sha256').hexdigest(),
            'removedImageEntries':len(removed),'bundledImageEntries':0,'nativeLibrariesUnchanged':True,'unrelatedHostClassesUnchanged':True,'probe':args.probe}
    (out/'package-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))


if __name__=='__main__':main()
