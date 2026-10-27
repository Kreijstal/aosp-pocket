#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build version-matched native services for the Android 10 guest port."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
from prepare_api29 import ROOT, prepare, prepare_sources


def build_properties(ndk, sdk=29):
    if sdk==29:prepare_sources(['bionic','libcxx','system-core'])
    out=ROOT/('out/api'+str(sdk));reference=prepare() if sdk==29 else ROOT/'out/api28/reference'
    target=ROOT/'out/api29'/('native' if sdk==29 else 'api28-regression');target.mkdir(parents=True,exist_ok=True)
    cc=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'/('aarch64-linux-android'+str(sdk)+'-clang++')
    headers=target/'headers/sys';headers.mkdir(parents=True,exist_ok=True)
    shutil.copy2(out/'bionic/libc/include/sys/_system_properties.h',headers/'_system_properties.h')
    common=[str(cc),'-DPOCKET_GUEST_API='+str(sdk),'-nostdinc++','-isystem',str(out/'libcxx/include'),'-std=c++17','-O2',
            '-fno-rtti','-fno-exceptions','-nostdlib++','-Wall','-Wextra','-Werror','-fPIE','-pie',
            '-I'+str(target/'headers'),'-I'+str(ROOT/'runtime/api28')]
    if sdk==29:
        # This libc++ revision unconditionally defines the old locale
        # fallbacks. Modern NDK Bionic headers already provide them; use
        # those declarations without editing the pinned source checkout.
        common+=['-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H']
    core=out/'system-core';property_root=core/'property_service';serializer=property_root/'libpropertyinfoserializer';parser=property_root/'libpropertyinfoparser'
    common+=['-I'+str(core/part/'include') for part in ['base','liblog','libutils','libcutils','libsystem']]
    common+=['-I'+str(serializer/'include'),'-I'+str(parser/'include')]
    libs=reference/'system/lib64'
    link=[str(libs/'libc.so'),'-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lbase','-lc++','-llog','-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    sources=[serializer/n for n in ['property_info_serializer.cpp','trie_builder.cpp','trie_serializer.cpp']]
    sources+=[parser/'property_info_parser.cpp',ROOT/'runtime/api28/property_info.cpp',ROOT/'runtime/api28/property_service.cpp']
    for source_names,name in [(sources,'pocket-property-service-api'+str(sdk)),([ROOT/'runtime/api28/property_probe.cpp'],'pocket-property-probe-api'+str(sdk))]:
        subprocess.run(common+list(map(str,source_names))+link+['-o',str(target/name)],check=True)
    report={'sdk':sdk,'component':'properties','nativeBuild':'PASS','fullGuestBootVerified':False}
    (target/'properties-build.json').write_text(json.dumps(report,indent=2)+'\n')
    print('API'+str(sdk)+'_PROPERTIES_BUILD_PASS',target)
    return target


def build_binder(ndk, sdk=29):
    if sdk==29:prepare_sources(['libcxx','frameworks-native','system-core'])
    out=ROOT/('out/api'+str(sdk))
    reference=prepare() if sdk==29 else ROOT/'out/api28/reference'
    target=ROOT/'out/api29'/('native' if sdk==29 else 'api28-regression')
    target.mkdir(parents=True,exist_ok=True)
    cc=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'/('aarch64-linux-android'+str(sdk)+'-clang++')
    common=[str(cc),'-DPOCKET_GUEST_API='+str(sdk),'-nostdinc++','-isystem',str(out/'libcxx/include'),
            '-std=c++17','-O2','-fno-rtti','-fno-exceptions','-nostdlib++','-Wall','-Wextra','-Werror',
            '-I'+str(ROOT/'runtime/api28'),'-I'+str(out/'frameworks-native/libs/binder/include')]
    if sdk==29:common+=['-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H']
    common+=['-I'+str(out/'system-core'/part/'include') for part in ['base','liblog','libutils','libcutils','libsystem']]
    libs=reference/'system/lib64'
    link=[str(libs/'libc.so'),'-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lbinder','-lutils','-lc++','-llog','-ldl',
          '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    bridge='pocket_binder_api'+str(sdk)
    sources=[ROOT/'runtime/api28'/name for name in ['binder_bridge.cpp','guest_credentials.cpp']]
    sources.append(ROOT/'runtime/direct/boot_binder.cpp')
    subprocess.run(common+['-fPIC','-shared']+list(map(str,sources))+link+
                   ['-Wl,-soname,lib'+bridge+'.so','-o',str(target/('lib'+bridge+'.so'))],check=True)
    subprocess.run(common+['-fPIE','-pie',str(ROOT/'runtime/api28/service_probe.cpp'),'-L'+str(target),'-l'+bridge]+link+
                   ['-o',str(target/('pocket-binder-probe-api'+str(sdk)))],check=True)
    print('API'+str(sdk)+'_BINDER_BUILD_PASS',target)
    return target


def build_init(ndk,sdk=29):
    target=build_properties(ndk,sdk)
    out=ROOT/('out/api'+str(sdk))
    reference=prepare() if sdk==29 else ROOT/'out/api28/reference'
    libs=reference/'system/lib64'
    source=target/'init'
    if source.exists():shutil.rmtree(source)
    shutil.copytree(out/'system-core/init',source,ignore=shutil.ignore_patterns('.clang-format'))
    subprocess.run(['patch','--batch','--fuzz=0','-p1','-i',str(ROOT/('runtime/api'+str(sdk))/'init-rootless.patch')],cwd=source,check=True)
    cc=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'/('aarch64-linux-android'+str(sdk)+'-clang++')
    common=[str(cc),'-DPOCKET_GUEST_API='+str(sdk),'-nostdinc++','-isystem',str(out/'libcxx/include'),
            '-std=c++17','-O2','-fno-rtti','-fno-exceptions',
            '-nostdlib++','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-missing-field-initializers',
            '-I'+str(target/'headers'),'-I'+str(ROOT/'runtime/api28'),'-I'+str(source)]
    if sdk==29:common+=['-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H']
    common+=['-I'+str(out/'system-core'/part/'include') for part in ['base','liblog','libutils','libcutils','libsystem']]
    property_root=out/'system-core/property_service';serializer=property_root/'libpropertyinfoserializer';parser=property_root/'libpropertyinfoparser'
    common+=['-I'+str(serializer/'include'),'-I'+str(parser/'include')]
    link=[str(libs/'libc.so'),'-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lbase','-lcutils','-lc++','-llog',
          '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    sources=[source/(name+'.cpp') for name in ['action','action_manager','action_parser','parser','tokenizer','import_parser','service','rlimit_parser','descriptors','util']]
    sources += [serializer/name for name in ['property_info_serializer.cpp','trie_builder.cpp','trie_serializer.cpp']]
    sources += [parser/'property_info_parser.cpp']+[ROOT/'runtime/api28'/name for name in ['property_info.cpp','property_service.cpp','init_main.cpp']]
    subprocess.run(common+['-fPIE','-pie','-DPOCKET_INIT_INTEGRATION']+list(map(str,sources))+link+
                   ['-o',str(target/('pocket-init-api'+str(sdk)))],check=True)
    subprocess.run(common+['-fPIE','-pie',str(ROOT/'runtime/api28/init_test.cpp')]+link+
                   ['-o',str(target/('pocket-init-test-api'+str(sdk)))],check=True)
    print('API'+str(sdk)+'_INIT_BUILD_PASS',target)
    return target


def build_zygote(ndk):
    prepare_sources(['bionic','libcxx','system-core','frameworks-native','frameworks-base','libnativehelper','art'])
    target=ROOT/'out/api29/native';target.mkdir(parents=True,exist_ok=True)
    out=ROOT/'out/api29';reference=prepare();libs=reference/'system/lib64'
    source=target/'framework'
    if source.exists():shutil.rmtree(source)
    shutil.copytree(out/'frameworks-base',source,symlinks=True,ignore=shutil.ignore_patterns('.git','.clang-format'))
    subprocess.run(['patch','--batch','--fuzz=0','-p1','-i',str(ROOT/'runtime/api29/zygote-rootless.patch')],cwd=source,check=True)
    cc=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++'
    jni=source/'core/jni'
    includes=[ROOT/'runtime/api28',ROOT/'runtime/loader',jni,jni/'include',out/'bionic/libc/private',out/'bionic/libc/async_safe/include',
              out/'frameworks-native/libs/binder/include',out/'libnativehelper/include',
              out/'libnativehelper/include_jni',out/'libnativehelper/header_only_include']
    includes += [out/'system-core'/part/'include' for part in ['base','liblog','libutils','libcutils','libsystem']]
    common=[str(cc),'-DPOCKET_GUEST_API=29','-DPOCKET_GUEST_ISA="arm64"','-nostdinc++','-isystem',str(out/'libcxx/include'),
            '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H','-std=c++17','-O2','-fno-rtti','-fno-exceptions',
            '-nostdlib++','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-writable-strings']
    common += ['-I'+str(path) for path in includes]
    apex=reference/'apex/com.android.runtime/lib64'
    link=[str(libs/'libc.so'),'-L'+str(libs),'-L'+str(apex),'-Wl,-rpath-link,'+str(libs),'-Wl,-rpath-link,'+str(apex),'-landroid_runtime','-lbinder','-lutils','-lcutils','-lbase','-lc++','-llog','-ldl',
          '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    subprocess.run(common+['-fPIC','-shared',str(jni/'com_android_internal_os_Zygote.cpp'),str(jni/'fd_utils.cpp'),
                          str(ROOT/'runtime/api29/zygote_bootstrap.cpp'),'-lnativehelper']+link+
                   ['-o',str(target/'libpocket_zygote_api29.so')],check=True)
    subprocess.run(common+['-fPIE','-pie',str(source/'cmds/app_process/app_main.cpp'),str(out/'art/sigchainlib/sigchain.cc'),
                          str(ROOT/'runtime/api28/boot_classes.cpp'),'-DART_TARGET_ANDROID','-Wl,--export-dynamic',
                          '-Wl,--version-script='+str(out/'art/sigchainlib/version-script64.txt'),'-lwilhelm']+link+
                   ['-o',str(target/'pocket-app-process-api29')],check=True)
    subprocess.run(common+['-fPIE','-pie',str(ROOT/'runtime/api29/zygote_client.cpp')]+link+
                   ['-o',str(target/'pocket-zygote-client-api29')],check=True)
    subprocess.run(common+['-fPIE','-pie',str(ROOT/'runtime/api28/log_writer.cpp')]+link+
                   ['-o',str(target/'pocket-log-writer-api29')],check=True)
    print('API29_ZYGOTE_BUILD_PASS',target)
    return target


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,required=True)
    parser.add_argument('--sdk',type=int,choices=[28,29],default=29,help='API 28 checks the shared source against the existing ABI')
    parser.add_argument('--binder',action='store_true')
    parser.add_argument('--init',action='store_true')
    parser.add_argument('--zygote',action='store_true')
    args=parser.parse_args()
    if args.zygote:
        if args.sdk!=29:parser.error('--zygote requires API 29')
        build_zygote(args.ndk.resolve())
    elif args.init:build_init(args.ndk.resolve(),args.sdk)
    else:(build_binder if args.binder else build_properties)(args.ndk.resolve(),args.sdk)
