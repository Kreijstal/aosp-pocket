#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Narrow source build of Android 9 init/zygote for the native ELF loader."""
from pathlib import Path
import re
import shutil
import zipfile
from target import ROOT, reference_dir

def build(ndk, output, run):
    out=ROOT/'out/api28';reference=reference_dir('arm64');libs=reference/'system/lib64'
    destination=output/'init-zygote';destination.mkdir(exist_ok=True)
    cc=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    common=[str(cc)+'++','-DPOCKET_GUEST_API=28','-DPOCKET_GUEST_ISA="arm64"',
        '-nostdinc++','-isystem',out/'libcxx/include','-std=c++17','-O2','-fno-rtti','-fno-exceptions',
        '-nostdlib++','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-writable-strings','-Wno-missing-field-initializers']
    includes=[out/'headers',out/'frameworks-native/libs/binder/include',ROOT/'runtime/api28']
    includes += [out/'system-core'/part/'include' for part in ('libutils','libcutils','liblog','libsystem','base')]
    common += ['-I'+str(path) for path in includes]
    link=[libs/'libc.so','-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lbinder','-lutils','-lc++','-llog','-lbase','-lcutils','-ldl',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    for name,source,patch in [('init',out/'system-core/init','init-rootless.patch'),('framework',out/'frameworks-base','zygote-rootless.patch')]:
        target=destination/name
        if target.exists():shutil.rmtree(target)
        shutil.copytree(source,target)
        import subprocess
        subprocess.run(['patch','--batch','-p1','-i',str(ROOT/'runtime/api28'/patch)],cwd=target,check=True,stdout=subprocess.DEVNULL)
    init=destination/'init';property_root=out/'system-core/property_service'
    serializer=property_root/'libpropertyinfoserializer';parser=property_root/'libpropertyinfoparser'
    sources=[init/(name+'.cpp') for name in ('action','action_manager','action_parser','parser','tokenizer','import_parser','service','rlimit_parser','descriptors','util')]
    sources += [serializer/name for name in ('property_info_serializer.cpp','trie_builder.cpp','trie_serializer.cpp')]
    sources += [parser/'property_info_parser.cpp']+[ROOT/'runtime/api28'/name for name in ('property_info.cpp','property_service.cpp','init_main.cpp')]
    run(*common,'-DPOCKET_INIT_INTEGRATION','-I'+str(init),'-I'+str(serializer/'include'),'-I'+str(parser/'include'),
        '-fPIE','-pie',*sources,*link,'-o',destination/'pocket-init-api28')
    run(*common,'-fPIE','-pie',ROOT/'runtime/api28/log_writer.cpp',*link,'-o',destination/'pocket-log-writer-api28')
    jni=destination/'framework/core/jni'
    zygote=common+['-I'+str(path) for path in (jni,jni/'include',out/'libnativehelper/include',out/'libnativehelper/include_jni',out/'libnativehelper/header_only_include',out/'art/openjdkjvmti/include')]
    # Keep the rootless platform profile explicit; no libc trampoline or
    # dynamically patched Bionic is linked into these source-built binaries.
    run(*zygote,'-fPIC','-shared',jni/'com_android_internal_os_Zygote.cpp',jni/'fd_utils.cpp',ROOT/'runtime/api28/zygote_bootstrap.cpp',
        '-landroid_runtime','-lnativehelper',*link,'-o',destination/'libpocket_zygote_api28.so')
    run(*zygote,'-fPIE','-pie',destination/'framework/cmds/app_process/app_main.cpp',out/'art/sigchainlib/sigchain.cc','-DART_TARGET_ANDROID',
        '-Wl,--export-dynamic','-Wl,--version-script='+str(out/'art/sigchainlib/version-script64.txt'),'-landroid_runtime','-lwilhelm',
        *link,'-o',destination/'pocket-app-process-api28')
    run(cc,'-O2','-Wall','-Wextra','-Werror',ROOT/'runtime/tests/exec-loader/init_probe.c','-ldl','-o',destination/'init-probe')
    classes=destination/'classes';dex=destination/'dex';classes.mkdir(exist_ok=True);dex.mkdir(exist_ok=True)
    android=ROOT.parent/'helloworldandroid/vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    run('javac','--release','8','-cp',android,'-d',classes,ROOT/'runtime/api28/ZygoteBootstrap.java',ROOT/'runtime/tests/exec-loader/LoaderChild.java')
    run('jar','cf',destination/'classes.jar','-C',classes,'.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',dex,destination/'classes.jar')
    jar=destination/'pocket-zygote.jar'
    with zipfile.ZipFile(jar,'w',zipfile.ZIP_DEFLATED) as archive:archive.write(dex/'classes.dex','classes.dex')
    environment=(reference/'init.environ.rc').read_text()
    bootclasspath=re.search(r'export BOOTCLASSPATH (\S+)',environment)[1]
    config=destination/'init.native.rc'
    config.write_text('''# Native loader bring-up: AOSP init and lazy AOSP zygote; no system_server.
on boot
    setprop dalvik.vm.heapstartsize 16m
    setprop dalvik.vm.heapsize 192m
    setprop dalvik.vm.jdwp-provider none
    setprop ro.dalvik.vm.native.bridge 0
    class_start native

service native-log /system/bin/pocket-log-writer-api28
    class native
    user 0
    group 0
    socket logdw dgram 0600 0 0

service native-zygote /system/bin/pocket-app-process-api28 -Xzygote -Xint -Xnorelocate -Xnoimage-dex2oat -Ximage:/system/framework/boot.art -Xplugin:libopenjdkjvmti.so -agentpath:/system/lib64/libpocket_zygote_api28.so -Xms16m -Xmx192m /system/bin --zygote --abi-list=arm64-v8a --socket-name=zygote --enable-lazy-preload
    class native
    user 0
    group 0
    socket zygote stream 0600 0 0
    setenv BOOTCLASSPATH '''+bootclasspath+'\n    setenv POCKET_GUEST_CONSOLE /logs/init.log\n')
    data={'system/framework/pocket-zygote.jar':jar,'system/etc/init/init.native.rc':config,'system/build.prop':reference/'system/build.prop'}
    for name in bootclasspath.split(':'):
        data[name.lstrip('/')]=reference/name.lstrip('/')
    for name in ('event-log-tags','public.libraries.txt','preloaded-classes'):
        data['system/etc/'+name]=reference/'system/etc'/name
    for path in (reference/'system/framework/arm64').glob('boot*'):data[str(path.relative_to(reference))]=path
    for path in (reference/'system/usr').rglob('*'):
        if path.is_file():data[str(path.relative_to(reference))]=path
    binaries={name:destination/name for name in ('pocket-init-api28','pocket-app-process-api28','pocket-log-writer-api28','init-probe')}
    libraries={'libpocket_zygote_api28.so':destination/'libpocket_zygote_api28.so'}
    for name in ('libart.so','libjavacore.so','libopenjdk.so','libopenjdkjvm.so','libopenjdkjvmti.so'):
        libraries[name]=libs/name
    for line in (reference/'system/etc/public.libraries.txt').read_text().splitlines():
        parts=line.split('#',1)[0].split()
        if parts and '32' not in parts[1:] and 'nopreload' not in parts[1:]:libraries[parts[0]]=libs/parts[0]
    return binaries,libraries,data
