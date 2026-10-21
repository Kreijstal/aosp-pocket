#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Rebuild only netd against a supplied Manager's bundled guest libraries."""
import argparse
from pathlib import Path
import struct
import subprocess
import zipfile
from boot_storage_protocol import generate
from build_boot import reserve_interpreter

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base-apk',type=Path,required=True)
parser.add_argument('--out-dir',type=Path,required=True)
parser.add_argument('--ndk',type=Path,required=True)
args=parser.parse_args()
args.out_dir.mkdir(parents=True,exist_ok=True)
libs=args.out_dir/'lib64';libs.mkdir(exist_ok=True)
with zipfile.ZipFile(args.base_apk) as apk:
    prefix='assets/api28-rootfs/system/lib64/'
    for entry in apk.infolist():
        if entry.filename.startswith(prefix) and entry.filename.endswith('.so.payload'):
            name=entry.filename[len(prefix):-8]
            if '/' not in name:(libs/name).write_bytes(apk.read(entry))
machine=struct.unpack_from('<H',(libs/'libc.so').read_bytes(),18)[0]
triple={183:'aarch64-linux-android',62:'x86_64-linux-android'}[machine]
api=ROOT/'out/api28'
includes=[api/'headers',api/'frameworks-native/libs/binder/include']
includes += [api/'system-core'/part/'include' for part in ('libutils','libcutils','liblog','libsystem','base')]
generate(args.out_dir/'boot_storage_protocol.h')
compiler=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'/(triple+'28-clang++')
binary=args.out_dir/'pocket-netd-offline'
command=[compiler,'-nostdinc++','-isystem',api/'libcxx/include','-std=c++17','-O2',
         '-fno-rtti','-fno-exceptions','-nostdlib++','-Wall','-Wextra','-Werror',
         '-Wno-unused-parameter','-Wno-unused-function','-Wno-writable-strings']
command += ['-I'+str(p) for p in includes+[args.out_dir]]
command += ['-fPIE','-pie',ROOT/'runtime/direct/boot_network.cpp','-L'+str(libs),
            '-Wl,-rpath-link,'+str(libs),'-lbinder','-lutils','-lc++','-llog',
            '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',binary]
subprocess.run(list(map(str,command)),check=True)
assert reserve_interpreter(binary)
assert struct.unpack_from('<H',binary.read_bytes(),18)[0]==machine
print('PASS:',triple,'netd rebuilt with private interpreter reservation')
