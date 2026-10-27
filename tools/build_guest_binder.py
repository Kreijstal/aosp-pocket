#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Narrow ARM64 Binder bridge rebuild against a supplied Manager image."""
import argparse,subprocess,zipfile,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def build(base,out,ndk):
 out.mkdir(parents=True,exist_ok=True);libs=out/'lib64';libs.mkdir(exist_ok=True)
 with zipfile.ZipFile(base) as apk:
  prefix='assets/api28-rootfs/system/lib64/'
  for e in apk.infolist():
   if e.filename.startswith(prefix) and e.filename.endswith('.so.payload'):
    name=e.filename[len(prefix):-8]
    if '/' not in name:(libs/name).write_bytes(apk.read(e))
 assert struct.unpack_from('<H',(libs/'libc.so').read_bytes(),18)[0]==183
 api=ROOT/'out/api28';llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
 includes=[api/'headers',api/'frameworks-native/libs/binder/include',ROOT/'runtime/api28']
 includes += [api/'system-core'/x/'include' for x in ('libutils','libcutils','liblog','libsystem','base')]
 common=[llvm/'aarch64-linux-android28-clang++','-DPOCKET_GUEST_API=28','-nostdinc++','-isystem',api/'libcxx/include',
  '-std=c++17','-O2','-fno-rtti','-fno-exceptions','-nostdlib++','-Wall','-Wextra','-Werror',
  '-Wno-unused-parameter','-Wno-unused-function','-Wno-writable-strings']+['-I'+str(p) for p in includes]
 link=['-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lbinder','-lutils','-lc++','-llog','-ldl','-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
 def run(args):subprocess.run(list(map(str,args)),check=True)
 run(common+['-fPIC','-shared',ROOT/'runtime/api28/binder_bridge.cpp',ROOT/'runtime/api28/guest_credentials.cpp']+link+['-Wl,-soname,libpocket_binder_api28.so','-o',out/'libpocket_binder_api28.so'])
 event=out/'boot_uevent.o';run([llvm/'aarch64-linux-android28-clang','-fPIC','-O2','-c',ROOT/'runtime/direct/boot_uevent.c','-o',event])
 generated=ROOT/'out/boot/hidl-generated';config=api/'hardware-interfaces/configstore/1.1/default'
 hwinc=[generated,api/'libhidl/base/include',api/'libhidl/transport/include',api/'libhwbinder/include',config,
  api/'frameworks-av/media/libaudioclient/include',api/'frameworks-av/include',api/'system-media/audio/include']
 run(common+['-I'+str(p) for p in hwinc]+['-include',ROOT/'runtime/direct/hidl_codegen_compat.h','-fPIC','-shared']+
  [ROOT/'runtime/direct'/s for s in ('boot_hidl.cpp','boot_binder.cpp','boot_health.cpp','boot_hardware.cpp','boot_audio.cpp')]+[event,config/'SurfaceFlingerConfigs.cpp']+
  ['-l:android.hardware.health@1.0.so','-l:android.hardware.health@2.0.so','-l:android.hardware.configstore@1.0.so','-l:android.hardware.configstore@1.1.so','-lhwbinder','-lhidlbase','-lhidltransport']+link+['-o',out/'libpocket_hidl_api28.so'])
 print('PASS: Binder and HIDL bridge libraries rebuilt; stock Bionic untouched')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--base-apk',type=Path,required=True);p.add_argument('--out-dir',type=Path,required=True);p.add_argument('--ndk',type=Path,required=True);a=p.parse_args();build(a.base_apk,a.out_dir,a.ndk)
