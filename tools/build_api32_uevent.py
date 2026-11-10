#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the private device-event provider for the stock Android 12L JNI."""
import argparse
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE,prepare_sources


def build_uevent(ndk):
    prepare_sources(['libhardware-legacy'])
    out=CACHE/'uevent';out.mkdir(parents=True,exist_ok=True)
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    common=['-O2','-Wall','-Wextra','-Werror','-I'+str(CACHE/'libhardware-legacy/include')]
    cc=[str(llvm/'aarch64-linux-android32-clang'),'-std=c11',*common]
    subprocess.run(cc+['-fPIC','-c',str(ROOT/'runtime/api32/uevent.c'),'-o',str(out/'uevent.o')],check=True)
    subprocess.run([str(llvm/'aarch64-linux-android32-clang++'),'-std=c++17','-nostdlib++',*common,
        '-fPIC','-shared',str(ROOT/'runtime/api32/uevent_jni.cpp'),str(out/'uevent.o'),
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-Wl,-soname,libpocket_uevent_api32.so',
        '-o',str(out/'libpocket_uevent_api32.so')],check=True)
    subprocess.run(cc+[str(ROOT/'runtime/api32/uevent_probe.c'),'-L'+str(out),'-lpocket_uevent_api32',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',str(out/'pocket-uevent-probe-api32')],check=True)
    print('API32_UEVENT_BUILD_PASS',out)
    return out


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,required=True)
    build_uevent(parser.parse_args().ndk.resolve())
