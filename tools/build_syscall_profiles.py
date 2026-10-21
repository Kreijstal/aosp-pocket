#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build an optional Zygisk translation module, fixture, and optional core migration."""
import argparse
from pathlib import Path
import re
import subprocess
import zipfile
from build_boot_zygote import build_zygote

ROOT=Path(__file__).resolve().parents[1]
def run(*args):subprocess.run(list(map(str,args)),cwd=ROOT,check=True)
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,default=Path.home()/'.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c')
    parser.add_argument('--out',type=Path,default=ROOT/'out/syscall-app')
    parser.add_argument('--base-apk',type=Path)
    parser.add_argument('--version',default='0.30')
    parser.add_argument('--test-only',action='store_true',help='Rebuild only the fixture APK and its control helper')
    args=parser.parse_args()
    if args.test_only and args.base_apk:parser.error('--test-only cannot package a Manager update')
    if not re.fullmatch(r'0\.[1-9][0-9]*',args.version):parser.error('Expected a numeric version such as 0.29')
    out=args.out.resolve();stage=out/'rootfs';(stage/'system/lib64').mkdir(parents=True,exist_ok=True)
    if args.base_apk:build_zygote(args.ndk,stage,None,zygote_only=True)
    compiler=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    if not args.test_only:
        module=ROOT/'runtime/modules/syscall-profiles'
        for name in ('syscall_profile.c','syscall_gate.S'):
            run(compiler,'-O2','-fPIC','-fvisibility=hidden','-Wall','-Wextra','-Werror','-c',module/name,'-o',out/(name+'.o'))
        run(str(compiler)+'++','-std=c++17','-O2','-shared','-fPIC','-fvisibility=hidden',
            '-fno-exceptions','-fno-rtti','-fno-threadsafe-statics','-nostdlib++','-Wall','-Wextra','-Werror',
            module/'module.cpp',out/'syscall_profile.c.o',out/'syscall_gate.S.o','-ldl',
            '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',out/'arm64-v8a.so')
        with zipfile.ZipFile(out/'pocket-syscall-profiles-0.1.zip','w',zipfile.ZIP_DEFLATED) as archive:
            for name in ('module.prop','customize.sh','skip_mount','NOTICE','sdk/zygisk.hpp'):
                archive.write(module/name,name)
            archive.write(out/'arm64-v8a.so','zygisk/arm64-v8a.so')
    toolchain=ROOT.parent/'helloworldandroid'
    android=toolchain/'vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    source=ROOT/'runtime/tests/syscall-app';classes=out/'classes';dex=out/'dex'
    classes.mkdir(exist_ok=True);dex.mkdir(exist_ok=True)
    run('javac','--release','8','-cp',android,'-d',classes,*sorted(source.glob('*.java')))
    run('jar','cf',out/'classes.jar','-C',classes,'.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',dex,out/'classes.jar')
    run(compiler,'-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',source/'fixture.c','-o',out/'libsyscall_fixture.so')
    run(compiler,'-O2','-Wall','-Wextra','-Werror',ROOT/'runtime/tests/key-attestation/launch.c','-o',out/'control')
    run('make','-f','runtime/Makefile','out/runtime/tools/PocketManifest.class')
    toolcp=str(toolchain/'build/tools')+':'+str(ROOT/'out/runtime/tools')
    run('java','-cp',toolcp,'PocketManifest',source/'AndroidManifest.xml',out/'AndroidManifest.xml','28','28','1','0.1')
    # The test depends on real separate Android processes. Catch encoders
    # silently omitting android:process before running an invalid comparison.
    import struct
    xml=(out/'AndroidManifest.xml').read_bytes();offset=8;ids=[];processes=0
    while offset<len(xml):
        kind,header,size=struct.unpack_from('<HHI',xml,offset)
        if kind==0x180:ids=list(struct.unpack_from('<'+'I'*((size-header)//4),xml,offset+header))
        elif kind==0x102:
            start,stride,count=struct.unpack_from('<HHH',xml,offset+24)
            for i in range(count):
                at=offset+16+start+i*stride;name=struct.unpack_from('<I',xml,at+4)[0]
                if name<len(ids) and ids[name]==0x01010011:
                    assert xml[at+15]==3,'android:process must be a string'
                    processes+=1
        offset+=size
    assert processes==2,'Fixture must declare two separately named processes'
    run('java','-cp',toolchain/'build/tools','com.example.build.ApkBuilder',out/'AndroidManifest.xml',dex/'classes.dex',out/'unsigned.apk')
    with zipfile.ZipFile(out/'unsigned.apk','a',zipfile.ZIP_DEFLATED) as apk:apk.write(out/'libsyscall_fixture.so','lib/arm64-v8a/libsyscall_fixture.so')
    signer=['java','-cp',toolchain/'build/apksigner.jar','com.android.apksigner.ApkSignerTool']
    run(*signer,'sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android','--out',out/'probe.apk',out/'unsigned.apk')
    run(*signer,'verify',out/'probe.apk')
    if args.base_apk:
        run('python3',ROOT/'tools/build_manager_update.py','--base-apk',args.base_apk.resolve(),
            '--out',ROOT/('out/manager-'+args.version),'--version-name',args.version,'--version-code',args.version.split('.')[-1],
            '--guest-file','system/lib64/libpocket_zygote_api28.so='+str(stage/'system/lib64/libpocket_zygote_api28.so'),
            '--remove-guest-file','system/lib64/libpocket_syscall_profile.so')
        with zipfile.ZipFile(ROOT/('out/manager-'+args.version)/('aosp-pocket-manager-'+args.version+'.apk')) as core:
            assert not any('syscall_profile' in name or 'syscall-profiles' in name for name in core.namelist()),'Optional adapter bundled in core'
            assert b'pocketApplySyscallProfile' not in core.read('assets/api28-rootfs/system/lib64/libpocket_zygote_api28.so.payload')
    print('PASS: real guest test APK built' if args.test_only else 'PASS: optional Zygisk module and fixture built; translator remains outside core APK')
if __name__=='__main__':main()
