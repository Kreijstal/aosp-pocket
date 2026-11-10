#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build private-socket identity translation and stock Perfetto session checks."""
import argparse
from pathlib import Path
import subprocess
from prepare_api32 import ROOT, CACHE, prepare, prepare_sources

def build_perfetto(ndk):
    prepare_sources(['libcxx'])
    reference=prepare();out=CACHE/'perfetto';out.mkdir(parents=True,exist_ok=True)
    compiler=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    common=[compiler,'-DPOCKET_GUEST_API=32','-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H',
        '-nostdinc++','-isystem',CACHE/'libcxx/include','-std=c++17','-O2','-fno-rtti','-fno-exceptions',
        '-nostdlib++','-Wall','-Wextra','-Werror']
    link=[bionic/'libc.so','-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),
        '-Wl,-rpath-link,'+str(bionic),'-lc++','-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    for source,target,flags in [('perfetto_scope.cpp','libpocket_perfetto_api32.so',['-fPIC','-shared']),
                                ('perfetto_probe.cpp','pocket-perfetto-probe-api32',['-fPIE','-pie'])]:
        subprocess.run(list(map(str,[*common,*flags,ROOT/'runtime/api32'/source,*link,'-o',out/target])),check=True)
    print('API32_PERFETTO_BUILD_PASS',out)
    return out

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',required=True,type=Path)
    build_perfetto(parser.parse_args().ndk.resolve())
