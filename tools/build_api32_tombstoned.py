#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the narrowly scoped 12L tombstoned transport and negative client."""
import argparse
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE,prepare,prepare_sources

def build_tombstoned(ndk):
    prepare_sources(['libcxx','system-core'])
    reference=prepare();out=CACHE/'tombstoned';out.mkdir(parents=True,exist_ok=True)
    compiler=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    common=[compiler,'-DPOCKET_GUEST_API=32','-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H',
        '-nostdinc++','-isystem',CACHE/'libcxx/include','-std=c++17','-O2','-fno-rtti','-fno-exceptions',
        '-nostdlib++','-Wall','-Wextra','-Werror','-I'+str(CACHE/'system-core/debuggerd'),
        '-I'+str(CACHE/'system-core/debuggerd/common/include')]
    link=[bionic/'libc.so','-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),
        '-Wl,-rpath-link,'+str(bionic),'-lc++','-ldl','-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    def run(*args):subprocess.run(list(map(str,args)),check=True)
    run(*common,'-fPIC','-shared',ROOT/'runtime/api32/tombstoned_scope.cpp',*link,'-o',out/'libpocket_tombstoned_api32.so')
    run(*common,'-fPIE','-pie',ROOT/'runtime/api32/tombstoned_probe.cpp',*link,'-o',out/'pocket-tombstoned-probe-api32')
    print('API32_TOMBSTONED_BUILD_PASS',out)
    return out

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ndk',required=True,type=Path)
    build_tombstoned(p.parse_args().ndk.resolve())
