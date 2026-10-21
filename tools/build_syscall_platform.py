#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Rebuild only guest platform pieces affected by syscall identity translation."""
from pathlib import Path
import argparse
import subprocess
import shutil
from build_boot_zygote import build_zygote
from target import ROOT, clang, reference_dir
from build_boot import reserve_interpreter

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,required=True)
    parser.add_argument('--zygote-only',action='store_true')
    args=parser.parse_args()
    out=ROOT/'out/bionic-mode'
    out.mkdir(parents=True,exist_ok=True)
    if args.zygote_only:
        stage=out/'rootfs'
        (stage/'system/lib64').mkdir(parents=True,exist_ok=True)
        build_zygote(args.ndk,stage,None,zygote_only=True)
        shutil.copy2(stage/'system/lib64/libpocket_zygote_api28.so',out/'libpocket_zygote_api28.so')
        print('PASS: focused zygote console-path rebuild')
        return
    llvm=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    subprocess.run([clang(llvm,28),'-O2','-Wall','-Wextra','-Werror',ROOT/'runtime/direct/boot_probe.c',
        '-ldl','-Wl,-z,max-page-size=16384','-o',out/'pocket-boot-probe'],check=True)
    assert reserve_interpreter(out/'pocket-boot-probe')
    event_object=out/'boot_uevent.o'
    subprocess.run([clang(llvm,28),'-O2','-Wall','-Wextra','-Werror','-fPIC','-c',ROOT/'runtime/direct/boot_uevent.c',
        '-o',event_object],check=True)
    api=ROOT/'out/api28';libs=reference_dir()/'system/lib64'
    generated=ROOT/'out/boot/hidl-generated'
    config=api/'hardware-interfaces/configstore/1.1/default'
    includes=[api/'headers',api/'frameworks-native/libs/binder/include',generated,
        api/'libhidl/base/include',api/'libhidl/transport/include',api/'libhwbinder/include',config,
        api/'frameworks-av/media/libaudioclient/include',api/'frameworks-av/include',api/'system-media/audio/include']
    includes += [api/'system-core'/part/'include' for part in ('libutils','libcutils','liblog','libsystem','base')]
    command=[clang(llvm,28,cxx=True),'-DPOCKET_GUEST_API=28','-nostdinc++','-isystem',api/'libcxx/include',
        '-std=c++17','-O2','-fno-rtti','-fno-exceptions','-nostdlib++','-Wall','-Wextra','-Werror',
        '-Wno-unused-parameter','-Wno-unused-function','-Wno-writable-strings']
    command += ['-I'+str(path) for path in includes]
    command += ['-include',ROOT/'runtime/direct/hidl_codegen_compat.h','-fPIC','-shared']
    command += [ROOT/'runtime/direct'/name for name in ('boot_hidl.cpp','boot_binder.cpp','boot_health.cpp','boot_hardware.cpp','boot_audio.cpp')]
    command += [event_object,config/'SurfaceFlingerConfigs.cpp','-L'+str(libs),'-Wl,-rpath-link,'+str(libs)]
    command += ['-l:'+name for name in ('android.hardware.health@1.0.so','android.hardware.health@2.0.so','android.hardware.configstore@1.0.so','android.hardware.configstore@1.1.so')]
    command += ['-lhwbinder','-lhidlbase','-lhidltransport','-lbinder','-lutils','-lc++','-llog','-ldl',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',out/'libpocket_hidl_api28.so']
    subprocess.run(list(map(str,command)),check=True)
    print('PASS: focused data-bootstrap and platform-identity rebuild')

if __name__=='__main__':main()
