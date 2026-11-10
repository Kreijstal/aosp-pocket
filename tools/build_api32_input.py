#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the Android 12L input reader with real rootless host wake protection."""
import argparse
import hashlib
from pathlib import Path
import re
import shlex
import shutil
import subprocess
from prepare_api32 import ROOT, CACHE, prepare, prepare_sources, prepare_aidl


def build_input(ndk,jobs=8):
    prepare_sources(['frameworks-native','libcxx','bionic','libbase','system-core',
                     'system-logging','libhidl','libhwbinder','libfmq','proto-logging','boringssl','fmtlib'])
    reference=prepare();out=CACHE/'input';out.mkdir(exist_ok=True)
    source=out/'reader-source';patch=ROOT/'runtime/api32/input-rootless.patch'
    stamp=hashlib.sha256(patch.read_bytes()).hexdigest();marker=source/'.pocket-input-source'
    if not marker.is_file() or marker.read_text()!=stamp:
        if source.exists():
            if not marker.is_file():raise ValueError('Unowned input reader source')
            shutil.rmtree(source)
        shutil.copytree(CACHE/'frameworks-native/services/inputflinger/reader',source)
        subprocess.run(['patch','--batch','--fuzz=0','-p1','-i',str(patch)],cwd=source,check=True)
        marker.write_text(stamp)
    generated=out/'generated';(generated/'sys').mkdir(parents=True,exist_ok=True)
    shutil.copy2(CACHE/'bionic/libc/include/sys/limits.h',generated/'sys/limits.h')
    constants=CACHE/'frameworks-native/libs/input/android/os/IInputConstants.aidl'
    constant_stamp=hashlib.sha256(constants.read_bytes()).hexdigest()
    saved=generated/'.input-constants-source'
    if not saved.is_file() or saved.read_text()!=constant_stamp:
        subprocess.run([str(prepare_aidl()),'--lang=cpp','--out='+str(generated/'src'),
            '--header_out='+str(generated),'-I'+str(CACHE/'frameworks-native/libs/input'),str(constants)],check=True)
        saved.write_text(constant_stamp)
    def write_generated(name,value):
        target=generated/name
        if not target.is_file() or target.read_text()!=value:target.write_text(value)
    # Declare the two generated platform APIs against their pinned schemas;
    # implementations remain the stock SDK 32 shared libraries, checked by ld.
    atom=(CACHE/'proto-logging/stats/atoms.proto').read_text()
    match=re.search(r'InputDeviceRegistered\s+inputdevice_registered\s*=\s*(\d+)',atom)
    if not match:raise ValueError('Missing pinned input registration atom')
    write_generated('statslog.h','#pragma once\n#include <stdint.h>\nnamespace android { namespace util {\n'+
        'constexpr int32_t INPUTDEVICE_REGISTERED='+match[1]+';\n'+
        'int stats_write(int32_t,const char*,int32_t,int32_t,int32_t,int32_t,const char*,int32_t);\n}}\n')
    prop=(CACHE/'frameworks-native/services/inputflinger/sysprop/InputFlingerProperties.sysprop').read_text()
    if not all(x in prop for x in ['android.sysprop.InputFlingerProperties','per_window_input_rotation','type: Boolean']):
        raise ValueError('Unexpected input property schema')
    write_generated('InputFlingerProperties.sysprop.h','#pragma once\n#include <optional>\n'+
        'namespace android { namespace sysprop { namespace InputFlingerProperties {\n'+
        'std::optional<bool> per_window_input_rotation();\n}}}\n')
    native=CACHE/'frameworks-native';framework=CACHE/'framework'
    if not (framework/'libpocket_hidl_api32.so').is_file() or not (framework/'generated/android/system/suspend/1.0/ISystemSuspend.h').is_file():
        from build_api32_framework import build_hardware
        build_hardware(ndk)
    includes=[generated,ROOT/'runtime/api32',ROOT/'runtime/direct',framework/'generated',native/'include',source/'include',source/'controller',
        source/'mapper',source/'mapper/accumulator',native/'services/inputflinger',native/'services/inputflinger/include',
        native/'services/batteryservice/include',native/'libs/input/include',native/'libs/ftl/include',
        native/'libs/ui/include',native/'libs/gui/include',native/'libs/binder/include',native/'libs/math/include',
        native/'libs/nativebase/include',native/'libs/nativewindow/include',native/'libs/arect/include',
        CACHE/'libbase/include',CACHE/'fmtlib/include',CACHE/'system-logging/liblog/include',CACHE/'libhidl/base/include',
        CACHE/'libhidl/transport/include',CACHE/'libhwbinder/include',CACHE/'libfmq/include',CACHE/'libfmq/base',
        CACHE/'bionic/libc/async_safe/include']
    includes += [CACHE/'system-core'/x/'include' for x in ['libutils','libcutils','libsystem']]
    # OpenSSL's SHA API used by the unchanged device-registration telemetry.
    includes.append(CACHE/'boringssl/src/include')
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    common=[str(llvm/'aarch64-linux-android32-clang++'),'-DPOCKET_GUEST_API=32',
        '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H','-nostdinc++','-isystem',str(CACHE/'libcxx/include'),
        '-std=c++17','-O2','-DNDEBUG','-fPIC','-fno-rtti','-fno-exceptions','-nostdlib++','-Wall','-Wextra','-Werror',
        '-Wno-unused-parameter','-Wno-sign-compare','-Wno-deprecated-declarations',
        '-Wno-deprecated-copy-with-user-provided-copy','-Wno-inconsistent-missing-override',
        '-Wno-range-loop-construct','-Wno-vla-cxx-extension','-Wno-unused-but-set-variable']+['-I'+str(p) for p in includes]
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    names=['base','crypto','cutils','input','log','statslog','ui','utils','InputFlingerProperties',
           'inputflinger_base','c++','hidlbase','android.system.suspend@1.0']
    link=[str(bionic/'libc.so'),'-L'+str(libs),'-L'+str(bionic),'-L'+str(framework),
        '-Wl,-rpath-link,'+str(libs),'-Wl,-rpath-link,'+str(bionic),'-Wl,-rpath-link,'+str(framework),
        '-Wl,--as-needed','-lpocket_hidl_api32']+['-l:'+name+'.so' if '@' in name else '-l'+name for name in names]+[
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    bp=(source/'Android.bp').read_text();block=bp.split('name: "libinputreader_sources"',1)[1].split('cc_defaults',1)[0]
    sources=[source/path for path in re.findall(r'"([^"\n]+\.cpp)"',block)]
    sources += [source/'InputReaderFactory.cpp',ROOT/'runtime/api32/input_wake.cpp']
    def path(value):return str(value).replace('$','$$').replace(' ','$ ').replace(':','$:')
    script=['builddir = '+path(out),'rule cxx','  command = '+shlex.join(common)+' -MMD -MF $out.d -c $in -o $out',
        '  description = CXX $in',
        '  depfile = $out.d','  deps = gcc','rule cxxfs',
        '  command = '+shlex.join(common+['-D_LIBCPP_BUILDING_LIBRARY','-Wno-unqualified-std-cast-call'])+' -MMD -MF $out.d -c $in -o $out',
        '  description = CXXFS $in',
        '  depfile = $out.d','  deps = gcc','rule link',
        '  command = '+shlex.join(common)+' $in '+shlex.join(link)+' $extra -o $out','  description = LINK $out']
    objects=[]
    for index,src in enumerate(sources):
        obj=out/f'reader-{index}.o';objects.append(obj);script.append(f'build {path(obj)}: cxx {path(src)}')
    for index,name in enumerate(['directory_iterator.cpp','operations.cpp']):
        obj=out/f'filesystem-{index}.o';objects.append(obj)
        script.append(f'build {path(obj)}: cxxfs {path(CACHE/"libcxx/src/filesystem"/name)}')
    target=out/'libinputreader.so'
    script += ['build '+path(target)+': link '+' '.join(map(path,objects)),
        '  extra = -shared -Wl,-soname,libinputreader.so']
    probe=out/'probe.o';script.append(f'build {path(probe)}: cxx {path(ROOT/"runtime/api32/input_probe.cpp")}')
    script += ['build '+path(out/'pocket-input-probe-api32')+': link '+path(probe)+' '+path(target),
        '  extra = -fPIE -pie']
    (out/'build.ninja').write_text('\n'.join(script)+'\n')
    subprocess.run(['ninja','-f',str(out/'build.ninja'),'-j',str(jobs)],check=True)
    print('API32_INPUT_BUILD_PASS',out)
    return out


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,required=True)
    parser.add_argument('--jobs',type=int,default=8)
    args=parser.parse_args()
    if args.jobs<1:parser.error('--jobs must be positive')
    build_input(args.ndk.resolve(),args.jobs)
