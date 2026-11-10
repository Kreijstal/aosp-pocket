#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the combined UID-1000 software Keymaster/Gatekeeper HAL process."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE,prepare,prepare_sources

def build_crypto(ndk,reuse_generated=False,frontend=False,out=None):
    if not reuse_generated:
        from build_api32_keystore import build_keystore
        from build_api32_gatekeeper import build_gatekeeper
        build_keystore(ndk);build_gatekeeper(ndk)
    prepare_sources(['scrypt','boringssl'])
    reference=prepare();out=(out or CACHE/'crypto').resolve();out.mkdir(parents=True,exist_ok=True)
    soft=CACHE/'hardware-interfaces/gatekeeper/1.0/software';scrypt=CACHE/'scrypt'
    generated=[CACHE/'keystore/generated',CACHE/'gatekeeper/generated']
    required=[generated[0]/'android/hardware/keymaster/4.1/IKeymasterDevice.h',
        generated[1]/'android/hardware/gatekeeper/1.0/IGatekeeper.h',soft/'SoftGateKeeperDevice.cpp']
    if not all(p.is_file() for p in required):raise ValueError('Build the matching HAL generators first')
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    includes=[*generated,soft,CACHE/'boringssl/src/include',scrypt/'lib/crypto',
        CACHE/'system-keymaster/ng/include',CACHE/'system-keymaster/include',
        CACHE/'system-gatekeeper/include',CACHE/'libhardware/include',
        CACHE/'libbase/include',CACHE/'system-logging/liblog/include',
        CACHE/'libhidl/base/include',CACHE/'libhidl/transport/include',CACHE/'libhwbinder/include',
        CACHE/'libfmq/include',CACHE/'libfmq/base']
    includes += [CACHE/'system-core'/part/'include' for part in ['libutils','libcutils','libsystem']]
    def run(*args):subprocess.run(list(map(str,args)),check=True)
    scrypt_source=scrypt/'lib/crypto/crypto_scrypt-neon.c';scrypt_object=out/'scrypt.o'
    run(llvm/'aarch64-linux-android32-clang','-O2','-fPIE','-DHAVE_CONFIG_H','-DUSE_OPENSSL_PBKDF2',
        '-Wall','-Wextra','-Werror','-Wno-unused-variable',
        *['-I'+str(p) for p in [scrypt,scrypt/'lib/crypto',scrypt/'lib/util',CACHE/'boringssl/src/include']],
        '-c',scrypt_source,'-o',scrypt_object)
    common=[llvm/'aarch64-linux-android32-clang++','-std=c++17','-O2','-fno-rtti','-fno-exceptions',
        '-nostdinc++','-nostdlib++','-isystem',CACHE/'libcxx/include',
        '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H','-Wall','-Wextra','-Werror',
        '-Wno-unused-parameter','-Wno-inconsistent-missing-override','-Wno-deprecated-declarations',
        *['-I'+str(p) for p in includes]]
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    sources=[ROOT/'runtime/api32/crypto_main.cpp',soft/'SoftGateKeeperDevice.cpp']
    frontend_objects=[];frontend_libraries=[];frontend_info=None
    if frontend:
        from build_api32_gatekeeper_frontend import build_object
        obj,frontend_info=build_object(common,out)
        frontend_objects.append(obj)
        frontend_libraries=['-lbinder','-lbinder_ndk','-l:libgatekeeper_aidl.so',
            '-l:android.hardware.security.keymint-V1-ndk_platform.so',
            '-l:android.security.authorization-ndk_platform.so']
        common.append('-DPOCKET_CRYPTO_FRONTEND')
    run(*common,'-fPIE','-pie',*sources,*frontend_objects,scrypt_object,reference/'vendor/lib64/libkeymaster41.so',
        bionic/'libc.so','-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),
        '-Wl,-rpath-link,'+str(bionic),'-Wl,-rpath-link,'+str(reference/'vendor/lib64'),
        '-lhidlbase','-l:android.hardware.keymaster@4.0.so','-l:android.hardware.keymaster@4.1.so',
        '-l:android.hardware.gatekeeper@1.0.so',*frontend_libraries,'-lgatekeeper','-lcrypto','-lbase','-lutils','-llog','-lc++',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',out/'pocket-crypto-api32')
    run(*common,'-fPIE','-pie',ROOT/'runtime/api32/scrypt_probe.cpp',scrypt_object,
        bionic/'libc.so','-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),
        '-Wl,-rpath-link,'+str(bionic),'-lcrypto','-lc++',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',out/'pocket-scrypt-probe-api32')
    (out/'build.json').write_text(json.dumps({'sdk':32,'serviceUid':1000,'serviceGid':1000,
        'components':3 if frontend else 2,'processesSaved':2 if frontend else 1,'runtimeVerified':False,
        'frontend':frontend_info,
        'keymasterSupplementalGroup1026Removed':True,
        'sourceSha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in [*sources,ROOT/'tools/build_api32_crypto.py',
                *([ROOT/'tools/build_api32_gatekeeper_frontend.py',ROOT/'runtime/api32/gatekeeper-frontend.patch'] if frontend else []),
                soft/'SoftGateKeeper.h',soft/'SoftGateKeeperDevice.h',scrypt_source,
                scrypt/'lib/crypto/crypto_scrypt-neon-salsa208.h',scrypt/'NOTICE']}},indent=2)+'\n')
    print('API32_CRYPTO_BUILD_PASS',out)
    return out

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,required=True)
    parser.add_argument('--reuse-generated',action='store_true')
    parser.add_argument('--frontend',action='store_true',help='Include the classic frontend; requires matching dual-Binder startup without standalone gatekeeperd')
    parser.add_argument('--out',type=Path);args=parser.parse_args()
    build_crypto(args.ndk.resolve(),args.reuse_generated,args.frontend,args.out)
