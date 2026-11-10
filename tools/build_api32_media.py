#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the API32 media preload and describe the stock media service roles."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE,prepare,prepare_sources

SERVICES=('pocket-mediaswcodec','pocket-mediaextractor','pocket-mediaserver')
CONFIG='''
service pocket-mediaswcodec /system/bin/mediaswcodec
    disabled
    user 1046
    group 1046 1006 1007 1031
    setenv LD_LIBRARY_PATH /apex/com.android.media.swcodec/lib64
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so:/system/lib64/libpocket_media_api32.so

service pocket-mediaextractor /system/bin/mediaextractor
    disabled
    user 1040
    group 1040 1007 1031
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so:/system/lib64/libpocket_media_api32.so

service pocket-mediaserver /system/bin/mediaserver
    disabled
    user 1013
    group 1013 1005 1006 1007 1031 3001 3002 3003 3007
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so
'''

def add_services(config):
    if any('\nservice '+name+' ' in '\n'+config for name in SERVICES):
        raise ValueError('Media services are already declared')
    return config+CONFIG

def binaries(reference):
    return {'system/bin/mediaextractor':reference/'system/bin/mediaextractor',
        'system/bin/mediaserver':reference/'system/bin/mediaserver',
        'system/bin/mediaswcodec':reference/'apex/com.android.media.swcodec/bin/mediaswcodec'}

def build(ndk,out=None):
    prepare_sources(['libcxx','frameworks-av','frameworks-native','system-core','libbase','system-logging'])
    reference=prepare();out=(out or CACHE/'media').resolve();out.mkdir(parents=True,exist_ok=True)
    compiler=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    policy=out/'media-policy.o'
    def run(*args):subprocess.run(list(map(str,args)),check=True)
    run(compiler.parent/'aarch64-linux-android32-clang','-O2','-Wall','-Wextra','-Werror','-fPIC','-c',ROOT/'runtime/api32/media_policy.c','-o',policy)
    includes=[ROOT/'runtime/api32',CACHE/'frameworks-av/include',CACHE/'frameworks-av/media/libmedia/include',
        CACHE/'frameworks-av/media/libstagefright/include',CACHE/'frameworks-av/media/libstagefright/foundation/include',
        CACHE/'frameworks-av/media/libstagefright/rtsp',CACHE/'frameworks-native/libs/binder/include',
        CACHE/'libbase/include',CACHE/'system-logging/liblog/include']
    includes += [CACHE/'system-core'/n/'include' for n in ['libutils','libcutils','libsystem']]
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    target=out/'libpocket_media_api32.so'
    run(compiler,'-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H','-nostdinc++','-isystem',CACHE/'libcxx/include',
        '-nostdlib++','-std=c++17','-O2','-fno-rtti','-fno-exceptions','-Wall','-Wextra','-Werror','-fPIC','-shared',
        *['-I'+str(n) for n in includes],ROOT/'runtime/api32/lazy_extractor.cpp',policy,bionic/'libc.so',
        '-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),'-Wl,-rpath-link,'+str(bionic),
        '-lc++','-lutils','-llog','-ldl','-Wl,--no-undefined','-Wl,-soname,libpocket_media_api32.so',
        '-Wl,-z,max-page-size=16384','-o',target)
    policy.unlink()
    (out/'build.json').write_text(json.dumps({'sdk':32,'firstDemandPlugins':True,'services':list(SERVICES),
        'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'runtimeVerified':False},indent=2)+'\n')
    print('API32_MEDIA_BUILD_PASS',target)
    return target

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ndk',type=Path,required=True);p.add_argument('--out',type=Path)
    a=p.parse_args();build(a.ndk.resolve(),a.out)
