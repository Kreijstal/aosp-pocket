#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build clients and guest authorization for the stock 12L software keystore."""
import argparse
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE,prepare,prepare_sources,prepare_aidl,prepare_hidl


def build_keystore(ndk):
    prepare_sources(['libcxx','system-core','libbase','system-logging','frameworks-native',
                     'hardware-interfaces','system-hardware-interfaces','system-security','system-keymaster',
                     'libhidl','libhwbinder','libfmq','libhardware'])
    reference=prepare();out=CACHE/'keystore';generated=out/'generated';generated.mkdir(parents=True,exist_ok=True)
    stable=[CACHE/'system-hardware-interfaces/keystore2/aidl/aidl_api/android.system.keystore2/1',
            CACHE/'hardware-interfaces/security/keymint/aidl/aidl_api/android.hardware.security.keymint/1',
            CACHE/'hardware-interfaces/security/secureclock/aidl/aidl_api/android.hardware.security.secureclock/1']
    for definitions in stable:
        digest=(definitions/'.hash').read_text().splitlines()[-1]
        subprocess.run([str(prepare_aidl()),'--lang=cpp','--structured','--stability=vintf','--version=1','--hash='+digest,
            '--out='+str(generated/'src'),'--header_out='+str(generated),*['-I'+str(p) for p in stable],
            *map(str,sorted(definitions.rglob('*.aidl')))],check=True)
    maintenance=CACHE/'system-security/keystore2/aidl'
    subprocess.run([str(prepare_aidl()),'--lang=cpp','--structured','--out='+str(generated/'src'),
        '--header_out='+str(generated),'-I'+str(maintenance),*['-I'+str(p) for p in stable],
        *map(str,sorted((maintenance/'android/security/maintenance').glob('*.aidl')))],check=True)
    for package in ['android.hidl.base@1.0','android.hidl.manager@1.0',
                    'android.hardware.keymaster@3.0','android.hardware.keymaster@4.0','android.hardware.keymaster@4.1']:
        subprocess.run([str(prepare_hidl()),'-o',str(generated),'-Lc++-headers',
            '-randroid.hidl:'+str(CACHE/'libhidl/transport'),
            '-randroid.hardware:'+str(CACHE/'hardware-interfaces'),package],check=True)
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    includes=[generated,CACHE/'frameworks-native/libs/binder/include',CACHE/'libbase/include',
        CACHE/'system-logging/liblog/include',CACHE/'libhidl/base/include',CACHE/'libhidl/transport/include',
        CACHE/'libhwbinder/include',CACHE/'libfmq/include',CACHE/'libfmq/base']
    includes += [CACHE/'system-core'/name/'include' for name in ['libutils','libcutils','libsystem']]
    common=[str(llvm/'aarch64-linux-android32-clang++'),'-DPOCKET_GUEST_API=32',
        '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H','-nostdinc++','-isystem',str(CACHE/'libcxx/include'),
        '-std=c++17','-O2','-fno-rtti','-fno-exceptions','-nostdlib++','-Wall','-Wextra','-Werror',
        '-Wno-unused-parameter','-Wno-inconsistent-missing-override',*['-I'+str(p) for p in includes]]
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    link=[str(bionic/'libc.so'),'-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),
        '-Wl,-rpath-link,'+str(bionic),'-lbinder','-lutils','-lc++','-llog',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    subprocess.run(common+['-fPIC','-shared',str(ROOT/'runtime/api32/keystore_policy.cpp')]+link+
        ['-Wl,-soname,libpocket_keystore_api32.so','-o',str(out/'libpocket_keystore_api32.so')],check=True)
    subprocess.run(common+['-fPIE','-pie',str(ROOT/'runtime/api32/keystore_probe.cpp'),
        *map(str,(generated/'src/android/security/maintenance').glob('*.cpp'))]+link+
        ['-lhidlbase','-l:android.hardware.keymaster@4.0.so','-l:android.hardware.keymaster@4.1.so',
         '-l:android.system.keystore2-V1-cpp.so','-l:android.hardware.security.keymint-V1-cpp.so',
         '-o',str(out/'pocket-keystore-probe-api32')],check=True)
    subprocess.run(common+['-I'+str(CACHE/'system-keymaster/ng/include'),'-I'+str(CACHE/'system-keymaster/include'),
        '-I'+str(CACHE/'libhardware/include'),'-fPIE','-pie',str(ROOT/'runtime/api32/keymaster_main.cpp'),
        str(reference/'vendor/lib64/libkeymaster41.so')]+link+
        ['-Wl,-rpath-link,'+str(reference/'vendor/lib64'),'-lhidlbase',
         '-l:android.hardware.keymaster@4.0.so','-l:android.hardware.keymaster@4.1.so',
         '-o',str(out/'pocket-keymaster-api32')],check=True)
    print('API32_KEYSTORE_BUILD_PASS',out)
    return out


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,required=True)
    build_keystore(parser.parse_args().ndk.resolve())
