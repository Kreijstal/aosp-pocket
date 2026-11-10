#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build libpocket_timecheck_api32.so, the 12L audioserver TimeCheck floor preload."""
import argparse,hashlib,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def build(ndk,out):
    out.mkdir(parents=True,exist_ok=True);lib=out/'libpocket_timecheck_api32.so'
    cc=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    subprocess.run([str(cc),'-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-Wl,-z,max-page-size=16384','-Wl,--build-id=none',
        str(ROOT/'runtime/api32/timecheck_preload.c'),'-llog','-ldl','-o',str(lib)],check=True)
    syms=subprocess.run(['readelf','-W','--dyn-syms',str(lib)],capture_output=True,text=True,check=True).stdout
    assert any(l.split()[-1]=='_ZN7android9TimeCheckC1EPKcj' and ' UND ' not in l for l in syms.splitlines() if l.strip()),'TimeCheck constructor not exported'
    print('API32_TIMECHECK_BUILD_PASS',lib,hashlib.sha256(lib.read_bytes()).hexdigest(),flush=True)
    return lib

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--out',type=Path,default=ROOT/'out/api32/timecheck')
    a=p.parse_args();build(a.ndk,a.out)
