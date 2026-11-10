#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the SDK32 software-compositor worker scheduling adapter."""
import argparse
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE

def build_renderer_policy(ndk):
    out=CACHE/'graphics';out.mkdir(parents=True,exist_ok=True)
    compiler=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang'
    subprocess.run(list(map(str,[compiler,'-std=c11','-D_GNU_SOURCE','-O2','-Wall','-Wextra','-Werror',
        '-fPIC','-shared',ROOT/'runtime/api32/renderer_workers.c','-ldl','-pthread',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384',
        '-Wl,-soname,libpocket_renderer_workers_api32.so','-o',out/'libpocket_renderer_workers_api32.so'])),check=True)
    print('API32_RENDERER_POLICY_BUILD_PASS',out/'libpocket_renderer_workers_api32.so')
    return out

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,required=True)
    build_renderer_policy(parser.parse_args().ndk.resolve())
