#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Narrow complete-Manager update for the encrypted VFS and Android vold flow."""
import argparse
from pathlib import Path
import shutil
import subprocess
import zipfile
from target import ROOT
from boot_storage_protocol import generate
from build_boot import reserve_interpreter
from validate_native_loader import validate


def build_parts(base_apk,ndk,out):
    out.mkdir(parents=True,exist_ok=True)
    def run(*argv):subprocess.run(list(map(str,argv)),cwd=ROOT,check=True)
    cc=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    run(cc,'-O2','-g','-Wall','-Wextra','-Werror','-mno-outline-atomics','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic',
        '-nostdlib','-static','-Wl,-T,runtime/loader/loader.ld,-z,max-page-size=16384',
        ROOT/'runtime/loader/loader.c',ROOT/'runtime/loader/entry.S','-o',out/'pocket-loader')
    validate(out/'pocket-loader')
    run(cc,'-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-pthread','-Wl,-z,max-page-size=16384',
        ROOT/'runtime/storage/native/ce_fuse.c','-o',out/'libpocket_ce.so')
    run(cc,'-O2','-Wall','-Wextra','-Werror','-Wl,-z,max-page-size=16384',ROOT/'runtime/direct/boot_probe.c','-ldl','-o',out/'pocket-boot-probe')
    api=ROOT/'out/api28';libs=out/'lib64';libs.mkdir(exist_ok=True);generate(out/'boot_storage_protocol.h')
    with zipfile.ZipFile(base_apk) as apk:
        for name in ('libbinder.so','libutils.so','libc++.so','liblog.so','libc.so','libm.so','libdl.so','libcutils.so','libbase.so','libbacktrace.so','libunwind.so','liblzma.so','libdexfile.so','libz.so','libprocinfo.so','libunwindstack.so','libartbase.so','libziparchive.so','libvndksupport.so','libnativehelper.so','libcrypto.so'):
            path='assets/api28-rootfs/system/lib64/'+name+'.payload'
            if path in apk.namelist():(libs/name).write_bytes(apk.read(path))
    inc=[out,api/'headers',api/'frameworks-native/libs/binder/include']+[api/'system-core'/p/'include' for p in ('libutils','libcutils','liblog','libsystem','base')]
    run(str(cc)+'++','-std=c++17','-nostdinc++','-isystem',api/'libcxx/include','-nostdlib++','-O2','-fno-rtti','-fno-exceptions',
        '-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-writable-strings',*['-I'+str(p) for p in inc],
        '-fPIE','-pie',ROOT/'runtime/direct/boot_storage.cpp','-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lbinder','-lutils','-lc++','-llog',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',out/'pocket-storage-services')
    for name in ('pocket-boot-probe','pocket-storage-services'):reserve_interpreter(out/name)
    android=ROOT.parent/'helloworldandroid/vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    for name in ('key-classes','key-dex'):
        shutil.rmtree(out/name,ignore_errors=True);(out/name).mkdir()
    run('javac','--release','8','-cp',android,'-d',out/'key-classes',ROOT/'runtime/tests/GuestCeKey.java')
    run('jar','cf',out/'key-classes.jar','-C',out/'key-classes','.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',out/'key-dex',out/'key-classes.jar')
    with zipfile.ZipFile(out/'pocket-ce-key.jar','w',zipfile.ZIP_DEFLATED) as jar:jar.write(out/'key-dex/classes.dex','classes.dex')
    return {'system/bin/'+name:out/name for name in ('pocket-loader','pocket-boot-probe','pocket-storage-services')}|{'system/framework/pocket-ce-key.jar':out/'pocket-ce-key.jar'},out/'libpocket_ce.so'


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ndk',type=Path,required=True);p.add_argument('--base-apk',type=Path,required=True);p.add_argument('--version',default='0.44');args=p.parse_args()
    out=ROOT/('out/manager-'+args.version);out.mkdir(parents=True,exist_ok=True)
    payload,library=build_parts(args.base_apk.resolve(),args.ndk.resolve(),out/'storage')
    toolchain=ROOT.parent/'helloworldandroid';android=toolchain/'vendor/android-platforms/android-36/android.jar';r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    def run(*argv):subprocess.run(list(map(str,argv)),cwd=ROOT,check=True)
    for name in ('host-classes','host-dex'):
        shutil.rmtree(out/name,ignore_errors=True);(out/name).mkdir()
    java=sorted((ROOT/'runtime/app/src').rglob('*.java'))+sorted((ROOT/'runtime/storage/src').rglob('*.java'))+[ROOT/'runtime/direct/BootBattery.java']
    run('javac','--release','8','-cp',android,'-d',out/'host-classes',*java)
    run('jar','cf',out/'host-classes.jar','-C',out/'host-classes','.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',out/'host-dex',out/'host-classes.jar')
    cmd=['python3',ROOT/'tools/build_manager_update.py','--base-apk',args.base_apk.resolve(),'--out',out,'--host-dex',out/'host-dex/classes.dex',
        '--version-code',args.version.split('.')[-1],'--version-name',args.version,'--host-library','libpocket_ce.so='+str(library)]
    for name,path in payload.items():cmd+=['--guest-file',name+'='+str(path)]
    run(*cmd);print('PASS: complete Manager encrypted-storage integration built')


if __name__=='__main__':main()
