#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the rootless adapter for the stock authenticated 12L ADB daemon."""
import argparse
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE

CONFIG='''
service pocket-adbd /system/bin/adbd
    disabled
    user 0
    group 0
    setenv LD_LIBRARY_PATH /apex/com.android.adbd/lib64
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_adbd_api32.so

on property:service.adb.root=*
    setprop ctl.restart pocket-adbd
'''

def build(ndk,out=None):
    out=(out or CACHE/'adbd').resolve();out.mkdir(parents=True,exist_ok=True)
    cc=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang'
    target=out/'libpocket_adbd_api32.so'
    subprocess.run(list(map(str,[cc,'-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',ROOT/'runtime/api32/adbd_policy.c',
        '-Wl,--no-undefined','-Wl,-soname,libpocket_adbd_api32.so','-Wl,-z,max-page-size=16384','-o',target])),check=True)
    print('API32_ADBD_BUILD_PASS',target)
    return target

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ndk',type=Path,required=True);a=p.parse_args();build(a.ndk.resolve())
