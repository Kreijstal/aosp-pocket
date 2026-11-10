#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compile the stock MediaMetricsService entry for the shared API32 core."""
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE,prepare,prepare_sources,prepare_aidl
from build_boot import reserve_interpreter


def build_object(ndk,out):
    prepare_sources(['frameworks-av','packages-statsd','system-media','libcxx',
                     'frameworks-native','system-core','libbase','system-logging'])
    generated=out/'metrics-generated';generated.mkdir(parents=True,exist_ok=True)
    definitions=CACHE/'frameworks-av/media/libmediametrics/aidl'
    permission=CACHE/'frameworks-native/libs/permission/aidl'
    binder=CACHE/'frameworks-native/libs/binder/aidl'
    source=definitions/'android/media/IMediaMetricsService.aidl'
    if 'oneway void submitBuffer(in byte[] buffer);' not in source.read_text():
        raise ValueError('Unexpected pinned MediaMetrics interface')
    subprocess.run(list(map(str,[prepare_aidl(),'--lang=cpp','--structured',
        '--out='+str(generated/'src'),'--header_out='+str(generated),
        '-I'+str(definitions),'-I'+str(permission),'-I'+str(binder),source,
        *sorted(permission.rglob('*.aidl')),*sorted((binder/'android/content/pm').glob('*.aidl'))])),check=True)
    includes=[generated,CACHE/'frameworks-av/services/mediametrics',
        CACHE/'frameworks-av/media/libmediametrics/include',CACHE/'frameworks-av/media/utils/include',
        CACHE/'packages-statsd/lib/libstatspull/include',CACHE/'packages-statsd/lib/libstatssocket/include',
        CACHE/'system-media/audio/include',
        CACHE/'system-media/audio_utils/include',CACHE/'frameworks-native/libs/binder/include',
        CACHE/'frameworks-native/libs/permission/include',
        CACHE/'libbase/include',CACHE/'system-logging/liblog/include']
    includes += [CACHE/'system-core'/name/'include' for name in ['libutils','libcutils','libsystem']]
    compiler=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    target=out/'metrics.o'
    subprocess.run(list(map(str,[compiler,'-DPOCKET_GUEST_API=32',
        '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H','-nostdinc++','-isystem',CACHE/'libcxx/include',
        '-std=c++17','-O2','-fno-rtti','-fno-exceptions','-fPIE','-Wall','-Wextra','-Werror',
        '-Wno-unused-parameter','-Wno-inconsistent-missing-override','-Wno-deprecated-declarations',
        *['-I'+str(p) for p in includes],'-c',ROOT/'runtime/api32/metrics_core.cpp','-o',target])),check=True)
    reference=prepare();libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    probe=out/'pocket-metrics-probe-api32'
    subprocess.run(list(map(str,[compiler,'-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H',
        '-nostdinc++','-isystem',CACHE/'libcxx/include','-nostdlib++','-std=c++17','-O2',
        '-fno-rtti','-fno-exceptions','-Wall','-Wextra','-Werror',
        *['-I'+str(p) for p in includes],ROOT/'runtime/tests/api32/metrics_core_probe.cpp',
        bionic/'libc.so','-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),'-Wl,-rpath-link,'+str(bionic),
        '-lmediametrics','-l:mediametricsservice-aidl-cpp.so','-lbinder','-lutils','-llog','-lc++',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',probe])),check=True)
    if not reserve_interpreter(probe):raise ValueError('Metrics probe needs a loader reservation')
    return target
