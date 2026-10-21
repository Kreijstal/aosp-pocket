#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build direct-profile zygote hooks separately from the PRoot regression."""
from pathlib import Path
import shutil
import subprocess
import struct
import re
from target import ROOT, clang, info, reference_dir
from boot_storage_protocol import generate as storage_protocol


def art_bridge_header(library, output):
    """Pin the existing ART native-bridge callbacks; never infer ArtMethod layout."""
    data=library.read_bytes()
    phoff=struct.unpack_from('<Q',data,32)[0]
    stride,count=struct.unpack_from('<HH',data,54)
    segments=[struct.unpack_from('<IIQQQQQQ',data,phoff+i*stride) for i in range(count)]
    mapped_base=min(p[3] & ~4095 for p in segments if p[0]==1)
    symbols=subprocess.check_output(['readelf','--wide','-Ws',str(library)],text=True)
    names=['_ZN3artL15GetMethodShorty','_ZN3artL20GetNativeMethodCount','_ZN3artL16GetNativeMethods']
    lines=['// Generated from the pinned guest ART; verified before calling.',
           'struct PocketArtCallback {uintptr_t offset; unsigned char bytes[32];};',
           'static const PocketArtCallback pocket_art_callbacks[] = {']
    for name in names:
        matches=re.findall(r'^\s*\d+:\s*([0-9a-f]+)\s+\d+\s+FUNC\s+LOCAL\s+DEFAULT\s+\d+\s+('+name+r'\S*)$',symbols,re.M)
        if len(matches)!=1: raise RuntimeError('Pinned ART callback missing or ambiguous: '+name)
        address=int(matches[0][0],16)
        segment=next(p for p in segments if p[0]==1 and p[3]<=address and address+32<=p[3]+p[5])
        start=segment[2]+address-segment[3]
        lines.append('{0x%x,{%s}},'%(address-mapped_base,','.join(str(c) for c in data[start:start+32])))
    lines.append('};')
    output.write_text('\n'.join(lines)+'\n')


def build_zygote(ndk, stage, hidl_gen, zygote_only=False):
    out=ROOT/'out/api28'
    source=ROOT/'out/boot/frameworks-base'
    if source.exists(): shutil.rmtree(source)
    shutil.copytree(out/'rootless-frameworks-base',source)
    subprocess.run(['patch','--batch','-p1','-i',str(ROOT/'runtime/direct/zygote-system-server.patch')],cwd=source,check=True)
    libs=reference_dir()/'system/lib64'
    header=ROOT/'out/boot/pocket_art_bridge.h'
    art_bridge_header(libs/'libart.so',header)
    includes=[out/'headers',out/'frameworks-native/libs/binder/include']
    includes += [out/'system-core'/part/'include' for part in ('libutils','libcutils','liblog','libsystem','base')]
    jni=source/'core/jni'
    zygote_source=jni/'com_android_internal_os_Zygote.cpp'
    text=zygote_source.read_text()
    registration='  return RegisterMethodsOrDie(env, "com/android/internal/os/Zygote", gMethods, NELEM(gMethods));'
    if registration not in text: raise RuntimeError('Unexpected zygote JNI registration')
    # Declare after includes, before the namespace, where JNI types are known.
    text=text.replace('namespace {', 'extern "C" int pocketInstallZygisk(JNIEnv*, const JNINativeMethod*, size_t);\n\nnamespace {',1)
    text=text.replace(registration,
        '  int result = RegisterMethodsOrDie(env, "com/android/internal/os/Zygote", gMethods, NELEM(gMethods));\n'
        '  if (result == 0 && pocketInstallZygisk(env, gMethods, NELEM(gMethods)) != 0)\n'
        '    fprintf(stderr, "POCKET_ZYGISK_UNAVAILABLE continuing without module hooks\\n");\n'
        '  return result;',1)
    zygote_source.write_text(text)
    includes += [jni,jni/'include',out/'libnativehelper/include',out/'libnativehelper/include_jni',out/'libnativehelper/header_only_include',out/'art/openjdkjvmti/include']
    command=[str(clang(ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin',28,cxx=True)),
             '-DPOCKET_GUEST_API=28','-DPOCKET_ZYGISK','-DPOCKET_GUEST_ISA="'+info()['framework']+'"',
             '-nostdinc++','-isystem',out/'libcxx/include',
             '-std=c++17','-O2','-fno-rtti','-fno-exceptions','-nostdlib++',
             '-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-writable-strings']
    command += ['-I'+str(path) for path in includes]+['-I'+str(header.parent)]
    command += ['-fPIC','-shared',jni/'com_android_internal_os_Zygote.cpp',jni/'fd_utils.cpp',ROOT/'runtime/api28/zygote_bootstrap.cpp',
                libs/'libc.so','-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-landroid_runtime','-lnativehelper','-lcutils','-lbase','-lbinder','-lutils','-lc++','-llog','-ldl',
                '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',stage/'system/lib64/libpocket_zygote_api28.so']
    subprocess.run(command,check=True)
    if zygote_only: return
    generated=ROOT/'out/boot/hidl-generated'
    generated.mkdir(parents=True,exist_ok=True)
    subprocess.run([hidl_gen,'-o',generated,'-Lc++-headers','-randroid.hidl:'+str(out/'libhidl/transport'),'android.hidl.base@1.0'],check=True)
    subprocess.run([hidl_gen,'-o',generated,'-Lc++-headers','-randroid.hidl:'+str(out/'libhidl/transport'),'android.hidl.manager@1.0'],check=True)
    subprocess.run([hidl_gen,'-o',generated,'-Lc++-headers','-randroid.hidl:'+str(out/'libhidl/transport'),'android.hidl.manager@1.1'],check=True)
    for version in ('1.0','1.1'):
        subprocess.run([hidl_gen,'-o',generated,'-Lc++-headers',
            '-randroid.hidl:'+str(out/'libhidl/transport'),'-randroid.hardware:'+str(out/'hardware-interfaces'),
            'android.hardware.configstore@'+version],check=True)
    for version in ('1.0','2.0'):
        subprocess.run([hidl_gen,'-o',generated,'-Lc++-headers',
            '-randroid.hidl:'+str(out/'libhidl/transport'),'-randroid.hardware:'+str(out/'hardware-interfaces'),
            'android.hardware.health@'+version],check=True)
    event_object=ROOT/'out/boot/boot_uevent.o'
    subprocess.run([clang(ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin',28),'-O2','-Wall','-Wextra','-Werror','-fPIC','-c',
        ROOT/'runtime/direct/boot_uevent.c','-o',event_object],check=True)
    config=out/'hardware-interfaces/configstore/1.1/default'
    prefix=command[:command.index('-fPIC')]
    # Only a forward declaration is needed by IAudioFlinger's createRecord
    # signature. No generated record client/stub is built or substituted.
    record=generated/'android/media/IAudioRecord.h';record.parent.mkdir(parents=True,exist_ok=True)
    record.write_text('namespace android { namespace media { class IAudioRecord; }}\n')
    audio_includes=['-I'+str(out/'frameworks-av/media/libaudioclient/include'),
                    '-I'+str(out/'frameworks-av/include'),'-I'+str(out/'system-media/audio/include')]
    subprocess.run(prefix+['-I'+str(generated),'-I'+str(out/'libhidl/base/include'),'-I'+str(out/'libhidl/transport/include'),'-I'+str(out/'libhwbinder/include'),
        '-include',ROOT/'runtime/direct/hidl_codegen_compat.h','-I'+str(config)]+audio_includes+[
        '-fPIC','-shared',ROOT/'runtime/direct/boot_hidl.cpp',ROOT/'runtime/direct/boot_binder.cpp',ROOT/'runtime/direct/boot_health.cpp',ROOT/'runtime/direct/boot_hardware.cpp',ROOT/'runtime/direct/boot_audio.cpp',event_object,config/'SurfaceFlingerConfigs.cpp','-L'+str(libs),'-Wl,-rpath-link,'+str(libs),
        '-l:android.hardware.health@1.0.so','-l:android.hardware.health@2.0.so','-l:android.hardware.configstore@1.0.so','-l:android.hardware.configstore@1.1.so','-lhwbinder','-lhidlbase','-lhidltransport','-lbinder','-lutils','-lc++','-llog','-ldl','-Wl,--no-undefined','-Wl,-z,max-page-size=16384',
        '-o',stage/'system/lib64/libpocket_hidl_api28.so'],check=True)
    # Independent process verifies actual Binder registrations from system_server.
    prefix=command[:command.index('-fPIC')]
    subprocess.run(prefix+['-fPIE','-pie',ROOT/'runtime/direct/boot_services.cpp',
        '-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lbinder','-lutils','-lc++','-llog',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',stage/'system/bin/pocket-boot-services'],check=True)
    storage_protocol(ROOT/'out/boot/boot_storage_protocol.h')
    subprocess.run(prefix+['-I'+str(ROOT/'out/boot'),'-fPIE','-pie',ROOT/'runtime/direct/boot_storage.cpp',
        '-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lbinder','-lutils','-lc++','-llog',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',stage/'system/bin/pocket-storage-services'],check=True)
    subprocess.run(prefix+['-I'+str(ROOT/'out/boot'),'-fPIE','-pie',ROOT/'runtime/direct/boot_network.cpp',
        '-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lbinder','-lutils','-lc++','-llog',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',stage/'system/bin/pocket-netd-offline'],check=True)
    subprocess.run(prefix+audio_includes+['-fPIE','-pie',ROOT/'runtime/direct/boot_audio_probe.cpp',
        '-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-laudioclient','-lsoundtrigger','-lbinder','-lutils','-lc++','-llog',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',stage/'system/bin/pocket-audio-verify'],check=True)
