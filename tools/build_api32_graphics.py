#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build software graphics from the pinned Android 12L platform sources."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from prepare_api32 import ROOT, CACHE, prepare, prepare_sources, prepare_hidl
from build_api32_renderer_policy import build_renderer_policy


def build_hal(ndk):
    prepare_sources(['libhardware','libcxx','system-core','system-logging','frameworks-native',
                     'hardware-interfaces','libhidl','libhwbinder','libfmq','libbase'])
    reference=prepare();out=CACHE/'graphics';out.mkdir(exist_ok=True)
    generated=out/'generated';generated.mkdir(exist_ok=True)
    packages=['android.hidl.base@1.0','android.hidl.manager@1.0']+[
        'android.hardware.graphics.'+name for name in ['common@1.0','common@1.1','common@1.2',
        'mapper@2.0','mapper@2.1','mapper@3.0','mapper@4.0','allocator@2.0','composer@2.1']]
    for package in packages:
        subprocess.run([str(prepare_hidl()),'-o',str(generated),'-Lc++-headers',
            '-randroid.hidl:'+str(CACHE/'libhidl/transport'),
            '-randroid.hardware:'+str(CACHE/'hardware-interfaces'),package],check=True)
    graphics=CACHE/'hardware-interfaces/graphics'
    includes=[generated,CACHE/'libhardware/include',CACHE/'libbase/include',CACHE/'libhidl/base/include',
        CACHE/'libhidl/transport/include',CACHE/'libhwbinder/include',CACHE/'libfmq/include',CACHE/'libfmq/base',
        CACHE/'system-logging/liblog/include',CACHE/'frameworks-native/libs/nativebase/include',
        CACHE/'frameworks-native/libs/nativewindow/include',CACHE/'frameworks-native/libs/arect/include']
    includes += [CACHE/'system-core'/name/'include' for name in ['libcutils','libutils','libsystem','libsync']]
    for component,version,kinds in [('mapper','2.0',['hal','passthrough']),('allocator','2.0',['hal','passthrough']),
        ('composer','2.1',['hal','passthrough','resources','command-buffer','hwc2on1adapter','hwc2onfbadapter'])]:
        includes += [graphics/component/version/'utils'/kind/'include' for kind in kinds]
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    common=[str(llvm/'aarch64-linux-android32-clang++'),'-DPOCKET_GUEST_API=32',
        '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H','-nostdinc++','-isystem',str(CACHE/'libcxx/include'),
        '-std=c++17','-O2','-fno-rtti','-fno-exceptions','-nostdlib++','-Wall','-Wextra','-Werror',
        '-Wno-unused-parameter','-Wno-sign-compare','-Wno-deprecated-declarations','-Wno-inconsistent-missing-override',
        '-Wno-deprecated-copy-with-user-provided-copy']
    common += ['-I'+str(p) for p in includes]
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    link=[str(bionic/'libc.so'),'-L'+str(libs),'-L'+str(bionic),'-Wl,-rpath-link,'+str(libs),
        '-Wl,-rpath-link,'+str(bionic),'-lhardware','-lhidlbase','-lfmq','-lutils','-lcutils','-lbase','-lsync','-llog','-lc++',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    link += ['-l:'+package+'.so' for package in packages if package.startswith('android.hardware.')]
    composer=graphics/'composer/2.1/utils'
    sources=[ROOT/'runtime/api32/graphics_hal.cpp',composer/'resources/ComposerResources.cpp',
        composer/'hwc2on1adapter/HWC2On1Adapter.cpp',composer/'hwc2on1adapter/MiniFence.cpp',
        composer/'hwc2onfbadapter/HWC2OnFbAdapter.cpp']
    subprocess.run(common+['-fPIC','-shared']+list(map(str,sources))+link+
        ['-o',str(out/'libpocket_graphics_api32.so')],check=True)
    subprocess.run(common+['-fPIE','-pie',str(ROOT/'runtime/api32/graphics_probe.cpp')]+link+
        ['-o',str(out/'pocket-graphics-probe-api32')],check=True)
    gralloc=out/'gralloc-source'
    if gralloc.exists():
        if not (gralloc/'.pocket-gralloc-source').is_file():raise ValueError('Unowned gralloc source')
        shutil.rmtree(gralloc)
    shutil.copytree(CACHE/'libhardware/modules/gralloc',gralloc)
    (gralloc/'.pocket-gralloc-source').write_text('32\n')
    subprocess.run(['patch','--batch','--fuzz=0','-p1','-i',str(ROOT/'runtime/api32/gralloc-rootless.patch')],cwd=gralloc,check=True)
    gralloc_link=[str(bionic/'libc.so'),'-L'+str(libs),'-L'+str(bionic),
        '-Wl,-rpath-link,'+str(libs),'-Wl,-rpath-link,'+str(bionic),
        '-Wl,--as-needed','-lcutils','-llog','-lc++','-lm',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    subprocess.run(common+['-Wno-unused-function','-Wno-missing-field-initializers','-fPIC','-shared']+
        [str(gralloc/name) for name in ['gralloc.cpp','mapper.cpp','framebuffer.cpp']]+gralloc_link+
        ['-o',str(out/'gralloc.pocket.so')],check=True)
    cc=[str(llvm/'aarch64-linux-android32-clang'),'-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-DPOCKET_GUEST_API=32','-Wno-deprecated-declarations']+['-I'+str(p) for p in includes]
    subprocess.run(cc+['-I'+str(gralloc),'-fPIC','-shared',
        str(ROOT/'runtime/direct/pocket_composer.c'),'-L'+str(libs),'-Wl,-rpath-link,'+str(libs),
        '-lhardware','-lcutils','-llog','-Wl,--no-undefined','-Wl,-z,max-page-size=16384',
        '-o',str(out/'hwcomposer.pocket.so')],check=True)
    subprocess.run([str(llvm/'aarch64-linux-android32-clang'),'-O2','-Wall','-Wextra','-Werror',
        str(ROOT/'runtime/direct/boot_egl.c'),'-L'+str(libs),'-Wl,-rpath-link,'+str(libs),
        '-lEGL','-lGLESv2','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',str(out/'pocket-egl-probe-api32')],check=True)
    build_renderer_policy(ndk)
    print('API32_GRAPHICS_HAL_BUILD_PASS',out)
    return out


def build_swiftshader(ndk, jobs):
    prepare_sources(['swiftshader','libhardware','system-core','system-logging','frameworks-native'])
    reference=prepare()
    out=CACHE/'graphics';out.mkdir(exist_ok=True)
    source=out/'swiftshader-source'
    patch=ROOT/'runtime/api32/swiftshader-platform.patch'
    stamp=hashlib.sha256(patch.read_bytes()).hexdigest()
    marker=source/'.pocket-platform-source'
    if not marker.is_file() or marker.read_text()!=stamp:
        if source.exists():
            if not marker.is_file():raise ValueError('Unowned SwiftShader build source')
            shutil.rmtree(source)
        shutil.copytree(CACHE/'swiftshader',source,ignore=shutil.ignore_patterns('.git'))
        subprocess.run(['patch','--batch','--fuzz=0','-p1','-i',str(patch)],cwd=source,check=True)
        marker.write_text(stamp)
    includes=[CACHE/'libhardware/include',CACHE/'system-core/libcutils/include',
        CACHE/'system-core/libsystem/include',CACHE/'system-core/libsync/include',
        CACHE/'system-logging/liblog/include',CACHE/'frameworks-native/libs/nativewindow/include',
        CACHE/'frameworks-native/libs/nativebase/include',CACHE/'frameworks-native/libs/arect/include']
    libraries=[reference/'system/lib64'/name for name in
        ['libhardware.so','libcutils.so','libnativewindow.so','libsync.so','liblog.so']]
    build=out/'swiftshader-build'
    subprocess.run(['cmake','-S',str(source),'-B',str(build),'-G','Ninja',
        '-DCMAKE_TOOLCHAIN_FILE='+str(ndk/'build/cmake/android.toolchain.cmake'),
        '-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-32','-DANDROID_STL=c++_static',
        '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
        '-DSWIFTSHADER_BUILD_VULKAN=OFF','-DSWIFTSHADER_BUILD_TESTS=OFF',
        '-DSWIFTSHADER_BUILD_PVR=OFF','-DSWIFTSHADER_BUILD_BENCHMARKS=OFF',
        '-DSWIFTSHADER_WARNINGS_AS_ERRORS=OFF',
        '-DPOCKET_PLATFORM_INCLUDES='+';'.join(map(str,includes)),
        '-DPOCKET_PLATFORM_LIBRARIES='+';'.join(map(str,libraries)),
        '-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384 -Wl,--no-undefined '+
            '-Wl,-rpath-link,'+str(reference/'system/lib64')],check=True)
    subprocess.run(['cmake','--build',str(build),'--target','libEGL','libGLESv2','libGLES_CM','-j',str(jobs)],check=True)
    files={}
    for name in ['libEGL_swiftshader.so','libGLESv2_swiftshader.so','libGLESv1_CM_swiftshader.so']:
        path=build/name
        if not path.is_file():raise ValueError('Missing software graphics library '+name)
        shutil.copy2(path,out/name)
        subprocess.run([str(ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'),
            '--strip-unneeded',str(out/name)],check=True)
        files[name]=hashlib.file_digest((out/name).open('rb'),'sha256').hexdigest()
    (out/'swiftshader-build.json').write_text(json.dumps({'sdk':32,'files':files,
        'sourcePatchSha256':stamp,'runtimeValidated':False},indent=2)+'\n')
    print('API32_SWIFTSHADER_BUILD_PASS',out)
    return out


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,required=True)
    parser.add_argument('--jobs',type=int,default=12)
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--hal-only',action='store_true')
    mode.add_argument('--swiftshader-only',action='store_true')
    args=parser.parse_args()
    if args.jobs<1:parser.error('--jobs must be positive')
    if not args.hal_only:build_swiftshader(args.ndk.resolve(),args.jobs)
    if not args.swiftshader_only:build_hal(args.ndk.resolve())
