#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Narrow source build of Android 9 init/zygote for the native ELF loader."""
from pathlib import Path
import re
import shutil
import zipfile
from target import ROOT, reference_dir
from boot_class_manifest import write_manifest

def build(ndk, output, run, framework=False):
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
    if framework:
        subprocess.run(['patch','--batch','-p1','-i',str(ROOT/'runtime/direct/zygote-system-server.patch')],cwd=destination/'framework',check=True)
        source=jni/'com_android_internal_os_Zygote.cpp';text=source.read_text()
        text=text.replace('namespace {', '#include "guest_api.h"\nstatic int platformGuestGroups(size_t n,const gid_t* groups) {\n'
            '  const char* backend=getenv("POCKET_SYSCALL_BACKEND");\n'
            '  return backend && !strcmp(backend,"native") ? pocket_native_setgroups(n,groups) : setgroups(n,groups);\n}\nnamespace {',1)
        text=text.replace('if (setgroups(gids.size(),','if (platformGuestGroups(gids.size(),')
        # Cooperative Zygisk providers install after the zygote JNI table, as in
        # the direct backend (build_boot_zygote).
        registration='  return RegisterMethodsOrDie(env, "com/android/internal/os/Zygote", gMethods, NELEM(gMethods));'
        if registration not in text: raise RuntimeError('Unexpected zygote JNI registration')
        text=text.replace('namespace {','extern "C" int pocketInstallZygisk(JNIEnv*, const JNINativeMethod*, size_t);\nnamespace {',1)
        text=text.replace(registration,
            '  int result = RegisterMethodsOrDie(env, "com/android/internal/os/Zygote", gMethods, NELEM(gMethods));\n'
            '  if (result == 0 && pocketInstallZygisk(env, gMethods, NELEM(gMethods)) != 0)\n'
            '    fprintf(stderr, "POCKET_ZYGISK_UNAVAILABLE continuing without module hooks\\n");\n'
            '  return result;',1)
        server='static jint com_android_internal_os_Zygote_nativeForkSystemServer('
        start=text.index(server);end=text.index('\nstatic ',start+len(server))
        block=text[start:end]
        if block.count('  return pid;')!=1:raise RuntimeError('Unexpected system-server fork body')
        text=text[:start]+block.replace('  return pid;','  if (pid == 0) pocketSystemServerCompatibility(env);\n  return pid;',1)+text[end:]
        text=text.replace('namespace {','extern "C" void pocketSystemServerCompatibility(JNIEnv*);\nnamespace {',1)
        source.write_text(text)
        from build_boot_zygote import art_bridge_header
        art_bridge_header(libs/'libart.so',destination/'pocket_art_bridge.h')
    zygote=common+['-I'+str(ROOT/'runtime/loader')]+['-I'+str(path) for path in (jni,jni/'include',out/'libnativehelper/include',out/'libnativehelper/include_jni',out/'libnativehelper/header_only_include',out/'art/openjdkjvmti/include')]
    # Keep the rootless platform profile explicit; no libc trampoline or
    # dynamically patched Bionic is linked into these source-built binaries.
    zygisk=['-DPOCKET_ZYGISK','-I'+str(destination)] if framework else []
    run(*zygote,*zygisk,'-fPIC','-shared',jni/'com_android_internal_os_Zygote.cpp',jni/'fd_utils.cpp',ROOT/'runtime/api28/zygote_bootstrap.cpp',
        '-landroid_runtime','-lnativehelper',*link,'-o',destination/'libpocket_zygote_api28.so')
    run(*zygote,'-fPIE','-pie',destination/'framework/cmds/app_process/app_main.cpp',out/'art/sigchainlib/sigchain.cc','-DART_TARGET_ANDROID',
        '-Wl,--export-dynamic','-Wl,--version-script='+str(out/'art/sigchainlib/version-script64.txt'),'-landroid_runtime','-lwilhelm',
        ROOT/'runtime/api28/boot_classes.cpp',*link,'-o',destination/'pocket-app-process-api28')
    run(cc,'-O2','-Wall','-Wextra','-Werror',ROOT/'runtime/tests/exec-loader/init_probe.c','-ldl','-o',destination/'init-probe')
    classes=destination/'classes';dex=destination/'dex';classes.mkdir(exist_ok=True);dex.mkdir(exist_ok=True)
    android=ROOT.parent/'helloworldandroid/vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    # Manager and isolated fixture use the same bootstrap, with different entry points.
    java=[ROOT/'runtime/api28/ZygoteBootstrap.java',ROOT/'runtime/tests/exec-loader/LoaderChild.java']
    if framework:
        java += [ROOT/name for name in ('runtime/api28/MicrogSupport.java','runtime/api28/ZygoteChild.java','runtime/direct/FrameworkProbe.java','runtime/direct/GuestControl.java',
            'runtime/direct/HostDevices.java','runtime/direct/HostNetwork.java','runtime/app/src/dev/kreijstal/aosppocket/bridge/HostWire.java',
            'runtime/app/src/dev/kreijstal/aosppocket/bridge/DisplayGeometry.java')]
    run('javac','--release','8','-cp',android,'-d',classes,*java)
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

service native-zygote /system/bin/pocket-app-process-api28 -Xzygote -Xint -Xnorelocate -Xnoimage-dex2oat -Ximage:/system/framework/boot.art -Xms16m -Xmx192m /system/bin --zygote --abi-list=arm64-v8a --socket-name=zygote --enable-lazy-preload
    class native
    user 0
    group 0
    socket zygote stream 0600 0 0
    setenv BOOTCLASSPATH '''+bootclasspath+'\n    setenv POCKET_GUEST_CONSOLE /logs/init.log\n')
    data={'system/framework/pocket-zygote.jar':jar,'system/etc/init/init.native.rc':config,'system/build.prop':reference/'system/build.prop'}
    data['system/framework/pocket-zygote.classes']=write_manifest(jar,destination/'pocket-zygote.classes')
    if framework:
        data['system/etc/pocket-microg/google-cert.hex']=ROOT/'runtime/api28/microg-google-cert.hex'
        data['system/etc/permissions/privapp-permissions-pocket-microg.xml']=ROOT/'runtime/api28/microg/privapp-permissions-pocket-microg.xml'
        data['system/etc/sysconfig/pocket-microg.xml']=ROOT/'runtime/api28/microg/pocket-microg.xml'
    for name in bootclasspath.split(':'):
        data[name.lstrip('/')]=reference/name.lstrip('/')
    for name in ('event-log-tags','public.libraries.txt','preloaded-classes'):
        data['system/etc/'+name]=reference/'system/etc'/name
    for path in (reference/'system/framework/arm64').glob('boot*'):data[str(path.relative_to(reference))]=path
    for path in (reference/'system/usr').rglob('*'):
        if path.is_file():data[str(path.relative_to(reference))]=path
    binaries={name:destination/name for name in ('pocket-init-api28','pocket-app-process-api28','pocket-log-writer-api28','init-probe')}
    libraries={'libpocket_zygote_api28.so':destination/'libpocket_zygote_api28.so'}
    for name in ('libart.so','libjavacore.so','libopenjdk.so','libopenjdkjvm.so'):
        libraries[name]=libs/name
    for line in (reference/'system/etc/public.libraries.txt').read_text().splitlines():
        parts=line.split('#',1)[0].split()
        if parts and '32' not in parts[1:] and 'nopreload' not in parts[1:]:libraries[parts[0]]=libs/parts[0]
    return binaries,libraries,data
