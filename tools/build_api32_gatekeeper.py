#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build a startup-only adapter and typed clients for SDK 32 software Gatekeeper."""
import argparse
from pathlib import Path
import subprocess
from prepare_api32 import ROOT, CACHE, prepare, prepare_sources, prepare_aidl, prepare_hidl


def build_gatekeeper(ndk):
    prepare_sources(['libcxx','system-core','frameworks-native','libbase','system-logging',
                     'system-gatekeeper','hardware-interfaces','libhardware','libhidl','libhwbinder','libfmq'])
    reference=prepare();out=CACHE/'gatekeeper';generated=out/'generated';generated.mkdir(parents=True,exist_ok=True)
    definitions=CACHE/'system-core/gatekeeperd/binder'
    subprocess.run([str(prepare_aidl()),'--lang=cpp','--out='+str(generated/'src'),'--header_out='+str(generated),
        '-I'+str(definitions),str(definitions/'android/service/gatekeeper/IGateKeeperService.aidl')],check=True)
    for package in ['android.hidl.base@1.0','android.hidl.manager@1.0','android.hardware.gatekeeper@1.0']:
        subprocess.run([str(prepare_hidl()),'-o',str(generated),'-Lc++-headers',
            '-randroid.hidl:'+str(CACHE/'libhidl/transport'),'-randroid.hardware:'+str(CACHE/'hardware-interfaces'),package],check=True)
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    subprocess.run([str(llvm/'aarch64-linux-android32-clang++'),'-std=c++17','-O2','-fno-exceptions','-fno-rtti','-nostdlib++','-Wall','-Wextra','-Werror',
        '-fPIC','-shared',str(ROOT/'runtime/api32/gatekeeper_pool.cpp'),'-ldl','-Wl,--no-undefined','-Wl,-z,max-page-size=16384',
        '-Wl,-soname,libpocket_gatekeeper_pool_api32.so','-o',str(out/'libpocket_gatekeeper_pool_api32.so')],check=True)
    includes=[generated,CACHE/'system-core/gatekeeperd/include',CACHE/'system-gatekeeper/include',CACHE/'libhardware/include',
        CACHE/'frameworks-native/libs/binder/include',CACHE/'libbase/include',CACHE/'system-logging/liblog/include',
        CACHE/'libhidl/base/include',CACHE/'libhidl/transport/include',CACHE/'libhwbinder/include',CACHE/'libfmq/include',CACHE/'libfmq/base']
    includes += [CACHE/'system-core'/p/'include' for p in ['libutils','libcutils','libsystem']]
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    command=[str(llvm/'aarch64-linux-android32-clang++'),'-std=c++17','-O2','-fno-rtti','-fno-exceptions',
        '-nostdinc++','-nostdlib++','-isystem',str(CACHE/'libcxx/include'),'-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H',
        '-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-inconsistent-missing-override',
        *['-I'+str(p) for p in includes],'-fPIE','-pie',str(ROOT/'runtime/api32/gatekeeper_probe.cpp'),
        str(bionic/'libc.so'),'-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),'-Wl,-rpath-link,'+str(bionic),
        '-l:libgatekeeper_aidl.so','-l:android.hardware.gatekeeper@1.0.so','-lhidlbase','-lbinder','-lutils','-lc++','-llog',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',str(out/'pocket-gatekeeper-probe-api32')]
    subprocess.run(command,check=True)
    print('API32_GATEKEEPER_BUILD_PASS',out)
    return out


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,required=True)
    build_gatekeeper(parser.parse_args().ndk.resolve())
