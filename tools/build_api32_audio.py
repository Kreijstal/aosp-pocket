#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the 12L audio passthrough gateway and Pocket host PCM HAL."""
import argparse
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE,prepare,prepare_sources,prepare_hidl


def build_audio(ndk):
    prepare_sources(['libcxx','system-core','system-logging','libbase','libhardware',
        'hardware-interfaces','libhidl','libhwbinder','libfmq','system-media','frameworks-native'])
    reference=prepare();out=CACHE/'audio';generated=out/'generated';generated.mkdir(parents=True,exist_ok=True)
    packages=['android.hidl.base@1.0','android.hidl.manager@1.0','android.hidl.memory@1.0','android.hidl.safe_union@1.0',
        'android.hardware.audio.common@7.0','android.hardware.audio@7.0','android.hardware.audio.effect@7.0']
    for package in packages:
        subprocess.run([str(prepare_hidl()),'-o',str(generated),'-Lc++-headers',
            '-randroid.hidl:'+str(CACHE/'libhidl/transport'),
            '-randroid.hardware:'+str(CACHE/'hardware-interfaces'),package],check=True)
    includes=[generated,CACHE/'libhardware/include',CACHE/'system-media/audio/include',CACHE/'libbase/include',
        CACHE/'system-logging/liblog/include',CACHE/'libhidl/base/include',CACHE/'libhidl/transport/include',
        CACHE/'libhwbinder/include',CACHE/'libfmq/include',CACHE/'libfmq/base',
        CACHE/'frameworks-native/libs/binder/include',CACHE/'frameworks-native/libs/nativebase/include']
    includes += [CACHE/'system-core'/name/'include' for name in ['libutils','libcutils','libsystem']]
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    common=[str(llvm/'aarch64-linux-android32-clang++'),'-DPOCKET_GUEST_API=32',
        '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H','-nostdinc++','-isystem',str(CACHE/'libcxx/include'),
        '-std=c++17','-O2','-fno-rtti','-fno-exceptions','-nostdlib++','-Wall','-Wextra','-Werror',
        '-Wno-unused-parameter','-Wno-inconsistent-missing-override','-Wno-deprecated-declarations']
    common += ['-I'+str(p) for p in includes]
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    link=[str(bionic/'libc.so'),'-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),
        '-Wl,-rpath-link,'+str(bionic),'-lhidlbase','-lhardware','-lbinder','-lutils','-lcutils','-lbase','-llog','-lc++',
        *['-l:'+p+'.so' for p in packages if p.startswith('android.hardware.')],
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    subprocess.run(common+['-fPIC','-shared',str(ROOT/'runtime/api32/audio_hal.cpp')]+link+
        ['-o',str(out/'libpocket_audio_api32.so')],check=True)
    subprocess.run(common+['-fPIE','-pie',str(ROOT/'runtime/api32/audio_probe.cpp')]+link+
        ['-o',str(out/'pocket-audio-probe-api32')],check=True)
    cc=[str(llvm/'aarch64-linux-android32-clang'),'-std=c11','-O2','-DPOCKET_GUEST_API=32',
        '-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',*['-I'+str(p) for p in includes]]
    subprocess.run(cc+['-fPIC','-shared',str(ROOT/'runtime/direct/audio_hal_pocket.c'),
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-Wl,-soname,audio.primary.pocket.so',
        '-o',str(out/'audio.primary.pocket.so')],check=True)
    print('API32_AUDIO_BUILD_PASS',out)
    return out


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,required=True)
    build_audio(parser.parse_args().ndk.resolve())
