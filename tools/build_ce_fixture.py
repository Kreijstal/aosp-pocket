#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build isolated tests of production encrypted VFS, loader and SQLite."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zipfile
import xml.etree.ElementTree as ET
from target import ROOT
from build_boot import reserve_interpreter
from validate_native_loader import validate


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--base-apk',type=Path,required=True)
    p.add_argument('--out',type=Path,default=ROOT/'out/ce-fixture')
    p.add_argument('--package',default='dev.kreijstal.aosppocket.cefixture',help='Use a new numeric suffix to preserve previous fixture data')
    args=p.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if not re.fullmatch(r'dev\.kreijstal\.aosppocket\.cefixture[0-9]*',args.package):p.error('Expected an isolated CE fixture package')
    cc=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    def run(*argv):subprocess.run(list(map(str,argv)),cwd=ROOT,check=True)
    run(cc,'-O2','-g','-Wall','-Wextra','-Werror','-mno-outline-atomics','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic',
        '-nostdlib','-static','-Wl,-T,runtime/loader/loader.ld,-z,max-page-size=16384',
        ROOT/'runtime/loader/loader.c',ROOT/'runtime/loader/entry.S','-o',out/'pocket-loader')
    validate(out/'pocket-loader')
    run(cc,'-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-pthread','-Wl,-z,max-page-size=16384',
        ROOT/'runtime/storage/native/ce_fuse.c','-llog','-o',out/'libpocket_ce.so')
    run(cc,'-O2','-Wall','-Wextra','-Werror','-Wl,-z,max-page-size=16384',ROOT/'runtime/tests/storage-integration/probe.c','-ldl','-o',out/'ce-test')
    reserve_interpreter(out/'ce-test')
    root='/data/user/0/'+args.package+'/files/fixture'
    data=bytearray((out/'ce-test').read_bytes());phoff=struct.unpack_from('<Q',data,32)[0];stride,count=struct.unpack_from('<HH',data,54)
    for i in range(count):
        ph=struct.unpack_from('<IIQQQQQQ',data,phoff+i*stride)
        if ph[0]==3:
            name=(root+'/system/bin/pocket-loader').encode()+b'\0';assert len(name)<=ph[5]
            data[ph[2]:ph[2]+ph[5]]=name.ljust(ph[5],b'\0');break
    else:raise RuntimeError('Missing interpreter')
    (out/'ce-test').write_bytes(data)
    toolchain=ROOT.parent/'helloworldandroid';android=toolchain/'vendor/android-platforms/android-36/android.jar'
    for name in ('classes','dex','tools'):
        shutil.rmtree(out/name,ignore_errors=True);(out/name).mkdir()
    java=[*sorted((ROOT/'runtime/storage/src').rglob('*.java')),
        *[ROOT/'runtime/app/src/dev/kreijstal/aosppocket/runtime'/name for name in ('CeStorage.java','CeFuse.java','CeMigration.java','CeMigrationSpace.java','CeDefaultKey.java','NativeLoader.java')],
        ROOT/'runtime/tests/storage-integration/CeFixtureActivity.java']
    run('javac','--release','8','-cp',android,'-d',out/'classes',*java)
    run('jar','cf',out/'classes.jar','-C',out/'classes','.')
    run('java','-cp',ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar',
        'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',out/'dex',out/'classes.jar')
    run('javac','--release','8','-cp',toolchain/'build/tools','-d',out/'tools',ROOT/'runtime/tools/PocketManifest.java')
    manifest=ET.parse(ROOT/'runtime/tests/storage-integration/AndroidManifest.xml');manifest.getroot().set('package',args.package)
    manifest.write(out/'AndroidManifest.source.xml',encoding='utf-8',xml_declaration=True)
    run('java','-cp',str(toolchain/'build/tools')+':'+str(out/'tools'),'PocketManifest',
        out/'AndroidManifest.source.xml',out/'AndroidManifest.xml','28','28','1','0.1')
    run('java','-cp',toolchain/'build/tools','com.example.build.ApkBuilder',out/'AndroidManifest.xml',out/'dex/classes.dex',out/'unsigned.apk')
    libraries=out/'lib64';libraries.mkdir(exist_ok=True);needed={};queue=[out/'ce-test']
    with zipfile.ZipFile(args.base_apk) as base:
        for name in ('libsqlite.so',):
            dest=libraries/name;dest.write_bytes(base.read('assets/api28-rootfs/system/lib64/'+name+'.payload'));needed[name]=dest;queue.append(dest)
        while queue:
            elf=queue.pop();dynamic=subprocess.check_output(['readelf','-dW',elf],text=True)
            for name in re.findall(r'Shared library: \[([^]]+)\]',dynamic):
                if name in needed:continue
                dest=libraries/name;dest.write_bytes(base.read('assets/api28-rootfs/system/lib64/'+name+'.payload'));needed[name]=dest;queue.append(dest)
        with zipfile.ZipFile(out/'unsigned.apk','a',zipfile.ZIP_DEFLATED) as apk:
            apk.write(out/'libpocket_ce.so','lib/arm64-v8a/libpocket_ce.so')
            for name in ('ce-test','pocket-loader'):apk.write(out/name,'assets/root/system/bin/'+name)
            apk.writestr('assets/root/system/bin/linker64',base.read('assets/api28-rootfs/system/bin/linker64.payload'))
            for name,path in needed.items():apk.write(path,'assets/root/system/lib64/'+name)
            for name in base.namelist():
                if name.startswith('assets/api28-rootfs/system/usr/icu/') and name.endswith('.payload'):
                    apk.writestr('assets/root/'+name[len('assets/api28-rootfs/'):-8],base.read(name))
    signer=['java','-cp',toolchain/'build/apksigner.jar','com.android.apksigner.ApkSignerTool']
    run(*signer,'sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android','--out',out/'ce-fixture.apk',out/'unsigned.apk')
    run(*signer,'verify',out/'ce-fixture.apk')
    inputs=set(java)|{ROOT/'tools/build_ce_fixture.py',ROOT/'runtime/tests/storage-integration/AndroidManifest.xml',
        ROOT/'runtime/tests/storage-integration/probe.c',ROOT/'runtime/storage/native/ce_fuse.c',ROOT/'runtime/tools/PocketManifest.java'}
    for folder in ['runtime/loader','runtime/storage/native']:
        inputs.update(path for path in (ROOT/folder).rglob('*') if path.is_file() and path.suffix in {'.c','.h','.S','.ld'})
    receipt={'package':args.package,'sha256':hashlib.sha256((out/'ce-fixture.apk').read_bytes()).hexdigest(),
        'baseApkSha256':hashlib.file_digest(args.base_apk.open('rb'),'sha256').hexdigest(),
        'sources':{path.relative_to(ROOT).as_posix():hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(inputs)},
        'runtimeVerified':False,'scope':'Isolated production encrypted VFS, five-root migration and native transport; no existing Manager profile or Android framework credential changes'}
    (out/'build.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print('PASS: isolated encrypted VFS fixture built',args.package,receipt['sha256'])


if __name__=='__main__':main()
