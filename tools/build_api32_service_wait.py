#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the bounded normal-runtime API32 Binder readiness client."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from prepare_api32 import ROOT, CACHE, prepare
from build_boot import reserve_interpreter


def build_service_wait(ndk, out):
    out=out.resolve();out.mkdir(parents=True,exist_ok=True)
    reference=prepare()
    compiler=ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    source=ROOT/'runtime/api32/service_wait_main.cpp'
    includes=[CACHE/'frameworks-native/libs/binder/include',CACHE/'libbase/include',
              CACHE/'system-logging/liblog/include']
    includes += [CACHE/'system-core'/name/'include' for name in ['libutils','libcutils','libsystem']]
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    binary=out/'pocket-runtime-service-wait-api32'
    command=[compiler,'-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H','-nostdinc++',
             '-isystem',CACHE/'libcxx/include','-nostdlib++','-std=c++17','-O2',
             '-fno-rtti','-fno-exceptions','-Wall','-Wextra','-Werror','-fPIE','-pie',
             *['-I'+str(p) for p in includes],source,bionic/'libc.so',
             '-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),
             '-Wl,-rpath-link,'+str(bionic),'-lbinder','-lutils','-llog','-lc++',
             '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',binary]
    subprocess.run(list(map(str,command)),check=True)
    if not reserve_interpreter(binary):raise ValueError('Service readiness client needs a loader interpreter reservation')
    pins={p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
          for p in [source,ROOT/'runtime/api32/service_readiness.h',Path(__file__).resolve()]}
    receipt={'sdk':32,'component':'normal-service-readiness','timeoutMs':60000,
             'overallHostBootTimeoutMsUnchanged':240000,'requiresPublishedAndPingableBinder':True,
             'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'bytes':binary.stat().st_size,
             'sources':pins,'command':list(map(str,command)),'runtimeVerified':False}
    (out/'build.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print('API32_SERVICE_WAIT_BUILD_PASS',receipt['sha256'])
    return binary


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();build_service_wait(args.ndk,args.out)
