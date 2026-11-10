#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the rootless Bluetooth callback policy and actual bionic timer probe."""
import argparse
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE,prepare

def build_timers(ndk):
    reference=prepare();out=CACHE/'timers';out.mkdir(parents=True,exist_ok=True)
    compiler=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang'
    common=[compiler,'-std=c11','-D_GNU_SOURCE','-O2','-Wall','-Wextra','-Werror']
    libc=reference/'apex/com.android.runtime/lib64/bionic/libc.so'
    link=[libc,'-ldl','-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    def run(*args):subprocess.run(list(map(str,args)),check=True)
    run(*common,'-fPIC','-shared',ROOT/'runtime/api32/service_timers.c',*link,
        '-Wl,-soname,libpocket_service_timers_api32.so','-o',out/'libpocket_service_timers_api32.so')
    run(*common,'-fPIE','-pie',ROOT/'runtime/api32/service_timer_probe.c',*link,
        '-o',out/'pocket-service-timer-probe-api32')
    print('API32_SERVICE_TIMERS_BUILD_PASS',out)
    return out

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',required=True,type=Path)
    build_timers(parser.parse_args().ndk.resolve())
