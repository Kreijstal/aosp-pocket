#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build a freestanding interpreter and an isolated, non-debuggable Android test."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import zipfile
import re
from target import ROOT,reference_dir
from build_boot import reserve_interpreter

PACKAGE='dev.kreijstal.aosppocket.loadertest'
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,required=True)
    parser.add_argument('--out',type=Path,default=ROOT/'out/exec-loader')
    parser.add_argument('--diagnostics',action='store_true')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    root='/data/user/0/'+PACKAGE+'/files/root'
    cc=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    def run(*argv):subprocess.run(list(map(str,argv)),cwd=ROOT,check=True)
    run(cc,*(['-DPOCKET_LOADER_DIAGNOSTICS'] if args.diagnostics else []),'-O2','-g','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic',
        '-nostdlib','-static','-Wl,-T,runtime/loader/loader.ld,-z,max-page-size=16384',
        ROOT/'runtime/loader/loader.c',ROOT/'runtime/loader/entry.S','-o',out/'pocket-loader')
    data=bytearray((out/'pocket-loader').read_bytes());marker=b'PCKELF01'+bytes(3072)
    assert data.count(marker)==1,'Missing or ambiguous loader instance config'
    offset=data.index(marker)+8
    for path in (root,root+'/system/bin/pocket-loader',root+'/system/bin/linker64'):
        value=path.encode()+b'\0';assert len(value)<=1024
        data[offset:offset+1024]=value.ljust(1024,b'\0');offset+=1024
    (out/'pocket-loader').write_bytes(data)
    assert b'INTERP' not in subprocess.check_output(['readelf','-lW',out/'pocket-loader'])
    symbols=subprocess.check_output(['readelf','-Ws',out/'pocket-loader'],text=True)
    assert not any(' UND ' in line and line.split()[-1]!='UND' for line in symbols.splitlines()),'Loader has undefined symbols'
    run(cc,'-O2','-Wall','-Wextra','-Werror',ROOT/'runtime/tests/exec-loader/probe.c','-ldl','-pthread',
        '-Wl,-z,max-page-size=16384','-o',out/'probe')
    shutil.copy2(out/'probe',out/'unprepared')
    assert reserve_interpreter(out/'probe')
    data=bytearray((out/'probe').read_bytes())
    phoff=struct.unpack_from('<Q',data,32)[0];stride,count=struct.unpack_from('<HH',data,54)
    for i in range(count):
        p=struct.unpack_from('<IIQQQQQQ',data,phoff+i*stride)
        if p[0]==3:
            name=(root+'/system/bin/pocket-loader').encode()+b'\0';assert len(name)<=p[5]
            data[p[2]:p[2]+p[5]]=name.ljust(p[5],b'\0');break
    else:raise AssertionError('Probe has no interpreter')
    (out/'probe').write_bytes(data)
    binaries={name:(out/name) for name in ('probe','pocket-loader','unprepared')}
    for name in ('sh','toybox'):
        binary=out/name;shutil.copy2(ROOT/'out/boot/platform/system/bin'/name,binary)
        data=bytearray(binary.read_bytes());assert struct.unpack_from('<H',data,18)[0]==183,'Expected ARM64 platform binary'
        assert reserve_interpreter(binary)
        data=bytearray(binary.read_bytes());phoff=struct.unpack_from('<Q',data,32)[0]
        stride,count=struct.unpack_from('<HH',data,54)
        for i in range(count):
            p=struct.unpack_from('<IIQQQQQQ',data,phoff+i*stride)
            if p[0]==3:
                name_bytes=(root+'/system/bin/pocket-loader').encode()+b'\0';assert len(name_bytes)<=p[5]
                data[p[2]:p[2]+p[5]]=name_bytes.ljust(p[5],b'\0');break
        else:raise AssertionError('Platform binary has no interpreter')
        binary.write_bytes(data);binaries[name]=binary
    toolchain=ROOT.parent/'helloworldandroid';android=toolchain/'vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    for name in ('classes','dex','tools'):(out/name).mkdir(exist_ok=True)
    run('javac','--release','8','-cp',android,'-d',out/'classes',ROOT/'runtime/tests/exec-loader/MainActivity.java')
    run('jar','cf',out/'classes.jar','-C',out/'classes','.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',out/'dex',out/'classes.jar')
    run('javac','--release','8','-cp',toolchain/'build/tools','-d',out/'tools',ROOT/'runtime/tools/PocketManifest.java')
    run('java','-cp',str(toolchain/'build/tools')+':'+str(out/'tools'),'PocketManifest',
        ROOT/'runtime/tests/exec-loader/AndroidManifest.xml',out/'AndroidManifest.xml','28','28','1','0.1')
    run('java','-cp',toolchain/'build/tools','com.example.build.ApkBuilder',out/'AndroidManifest.xml',out/'dex/classes.dex',out/'unsigned.apk')
    reference=reference_dir('arm64')
    dependencies={};queue=list(binaries.values())
    while queue:
        binary=queue.pop()
        elf=subprocess.check_output(['readelf','-dW',binary],text=True)
        for dependency in re.findall(r'Shared library: \[([^]]+)\]',elf):
            if dependency in dependencies:continue
            path=reference/'system/lib64'/dependency
            assert path.is_file(),'Missing pinned library: '+dependency
            assert struct.unpack_from('<H',path.read_bytes(),18)[0]==183,'Library ABI mismatch'
            dependencies[dependency]=path;queue.append(path)
    with zipfile.ZipFile(out/'unsigned.apk','a',zipfile.ZIP_DEFLATED) as apk:
        for name,path in sorted(dependencies.items()):apk.write(path,'assets/root/system/lib64/'+name)
        apk.write(reference/'system/bin/linker64','assets/root/system/bin/linker64')
        for name,path in binaries.items():apk.write(path,'assets/root/system/bin/'+name)
        config='dir.pocket = /system/bin\n[pocket]\nnamespace.default.isolated = false\nnamespace.default.search.paths = /system/lib64\nnamespace.default.permitted.paths = /system/lib64\n'
        apk.writestr('assets/root/system/etc/ld.config.txt',config)
        apk.writestr('assets/root/fixture.txt','loader-fixture\n')
        apk.writestr('assets/root/script.sh','#!/system/bin/sh\nexit 99\n')
    signer=['java','-cp',toolchain/'build/apksigner.jar','com.android.apksigner.ApkSignerTool']
    run(*signer,'sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android',
        '--out',out/'loader-test.apk',out/'unsigned.apk')
    run(*signer,'verify',out/'loader-test.apk')
    print('PASS: freestanding fixed-address interpreter and isolated loader-test.apk built')

if __name__=='__main__':main()
