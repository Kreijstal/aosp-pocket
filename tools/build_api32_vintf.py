#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build a bounded kernel-level conversion for the stock 12L VINTF parser."""
import argparse
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE,prepare,prepare_sources


def build_vintf(ndk):
    prepare_sources(['libvintf','libcxx','system-core','libbase','system-logging'])
    reference=prepare();out=CACHE/'vintf';out.mkdir(parents=True,exist_ok=True)
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    includes=[CACHE/'libvintf/include',CACHE/'libbase/include',CACHE/'system-logging/liblog/include',
        *[CACHE/'system-core'/name/'include' for name in ['libutils','libcutils','libsystem']]]
    common=[str(llvm/'aarch64-linux-android32-clang++'),'-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H',
        '-nostdinc++','-isystem',str(CACHE/'libcxx/include'),'-nostdlib++','-std=c++17','-O2',
        '-fno-rtti','-fno-exceptions','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
        *['-I'+str(p) for p in includes]]
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    link=[str(bionic/'libc.so'),'-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),
        '-Wl,-rpath-link,'+str(bionic),'-lvintf','-lutils','-lbase','-lc++','-llog',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    subprocess.run(common+['-fPIC','-shared',str(ROOT/'runtime/api32/vintf_kernel.cpp')]+link+
        ['-Wl,-soname,libpocket_vintf_api32.so','-o',str(out/'libpocket_vintf_api32.so')],check=True)
    subprocess.run(common+['-fPIE','-pie',str(ROOT/'runtime/api32/vintf_probe.cpp'),
        '-L'+str(out),'-lpocket_vintf_api32']+link+['-o',str(out/'pocket-vintf-probe-api32')],check=True)
    print('API32_VINTF_BUILD_PASS',out)
    return out


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,required=True)
    build_vintf(parser.parse_args().ndk.resolve())
