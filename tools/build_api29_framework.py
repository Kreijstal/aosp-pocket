#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build Android 10's version-matched framework service adapters."""
import argparse
import json
from pathlib import Path
import subprocess
from prepare_api29 import ROOT, prepare, prepare_sources
from boot_storage_protocol import generate as storage_protocol


def build_graphics(ndk):
    prepare_sources(['libhardware','system-core'])
    api=ROOT/'out/api29';libs=prepare()/'system/lib64';out=api/'framework'
    out.mkdir(parents=True,exist_ok=True)
    cc=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang'
    common=[cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
            '-I'+str(api/'libhardware/include'),'-I'+str(api/'system-core/libcutils/include'),
            '-I'+str(api/'system-core/libsystem/include')]
    link=['-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lhardware','-lcutils','-llog',
          '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    subprocess.run(list(map(str,common+['-fPIC','-shared',ROOT/'runtime/direct/pocket_composer.c']+link+
                            ['-o',out/'hwcomposer.pocket.so'])),check=True)
    subprocess.run(list(map(str,common+[ROOT/'runtime/api29/gralloc_probe.c']+link+
                            ['-o',out/'pocket-gralloc-probe-api29'])),check=True)
    subprocess.run(list(map(str,[cc,'-O2','-Wall','-Wextra','-Werror',ROOT/'runtime/direct/boot_egl.c',
        '-L'+str(libs),'-lEGL','-lGLESv2','-Wl,--no-undefined','-Wl,-z,max-page-size=16384',
        '-o',out/'pocket-egl-probe-api29'])),check=True)
    print('API29_GRAPHICS_BUILD_PASS',out)
    return out


def build_storage(ndk,sdk=29):
    if sdk==29:prepare_sources(['libcxx','system-core','frameworks-native','system-vold','system-netd','system-apex'])
    api=ROOT/('out/api'+str(sdk));reference=prepare() if sdk==29 else ROOT/'out/api28/reference'
    out=ROOT/'out/api29'/('framework' if sdk==29 else 'api28-framework-regression')
    out.mkdir(parents=True,exist_ok=True);storage_protocol(out/'boot_storage_protocol.h',sdk)
    libs=reference/'system/lib64';llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    includes=[out,ROOT/'runtime/api28',api/'frameworks-native/libs/binder/include']
    includes += [api/'system-core'/part/'include' for part in ['base','liblog','libutils','libcutils','libsystem']]
    cmd=[llvm/('aarch64-linux-android'+str(sdk)+'-clang++'),'-DPOCKET_GUEST_API='+str(sdk),
         '-nostdinc++','-isystem',api/'libcxx/include','-std=c++17','-O2','-fno-rtti','-fno-exceptions',
         '-nostdlib++','-Wall','-Wextra','-Werror']
    if sdk==29:cmd+=['-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H']
    cmd+=['-I'+str(path) for path in includes]
    cmd+=['-fPIE','-pie',ROOT/'runtime/direct/boot_storage.cpp','-L'+str(libs),'-Wl,-rpath-link,'+str(libs),
          '-lbinder','-lutils','-lc++','-llog','-Wl,--no-undefined','-Wl,-z,max-page-size=16384',
          '-o',out/('pocket-storage-services-api'+str(sdk))]
    subprocess.run(list(map(str,cmd)),check=True)
    if sdk==29:
        records=[]
        for path in sorted((reference/'apex').glob('*/apex_manifest.json')):
            raw=path.read_text();manifest=json.loads(raw)
            assert manifest['name']==path.parent.name
            records.append('{'+','.join([json.dumps(manifest['name']),json.dumps('/apex/'+manifest['name']),
                str(int(manifest['version'])),json.dumps(manifest.get('versionName','')),json.dumps(raw)])+'},')
        assert len(records)==6
        (out/'apex_modules.h').write_text('// Generated from the digest-verified flattened APEX manifests.\n'+
            'static const Module modules[]={\n'+'\n'.join(records)+'\n};\n')
        apex_cmd=cmd.copy();apex_cmd[apex_cmd.index(ROOT/'runtime/direct/boot_storage.cpp')]=ROOT/'runtime/api29/apex_service.cpp'
        apex_cmd[-1]=out/'pocket-apex-service-api29'
        subprocess.run(list(map(str,apex_cmd)),check=True)
    print('API'+str(sdk)+'_STORAGE_BUILD_PASS',out)
    return out


def build_hardware(ndk,hidl_gen,sdk=29):
    if sdk==29:
        prepare_sources(['libcxx','system-core','frameworks-native','libhidl','libhwbinder','hardware-interfaces','frameworks-av','system-media','system-hardware-interfaces'])
    api=ROOT/('out/api'+str(sdk));reference=prepare() if sdk==29 else ROOT/'out/api28/reference'
    out=ROOT/'out/api29'/('framework' if sdk==29 else 'api28-framework-regression')
    out.mkdir(parents=True,exist_ok=True);generated=out/'generated';generated.mkdir(exist_ok=True)
    def run(*cmd):subprocess.run(list(map(str,cmd)),check=True)
    packages=['android.hidl.base@1.0','android.hidl.manager@1.0','android.hidl.manager@1.1']
    if sdk==29:packages+=['android.hidl.manager@1.2','android.system.suspend@1.0']
    packages += ['android.hardware.'+p for p in ['configstore@1.0','configstore@1.1','health@1.0','health@2.0','vibrator@1.0','vibrator@1.1','vibrator@1.2']]
    for package in packages:
        run(hidl_gen,'-o',generated,'-Lc++-headers','-randroid.hidl:'+str(api/'libhidl/transport'),
            '-randroid.hardware:'+str(api/'hardware-interfaces'),
            *(['-randroid.system:'+str(api/'system-hardware-interfaces')] if sdk==29 else []),package)
    libs=reference/'system/lib64';config=api/'hardware-interfaces/configstore/1.1/default'
    includes=[generated,ROOT/'runtime/api28',api/'frameworks-native/libs/binder/include',
              api/'libhidl/base/include',api/'libhidl/transport/include',api/'libhwbinder/include',config,
              api/'frameworks-av/media/libaudioclient/include',api/'frameworks-av/include',api/'system-media/audio/include']
    includes += [api/'system-core'/part/'include' for part in ['base','liblog','libutils','libcutils','libsystem']]
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    common=[llvm/('aarch64-linux-android'+str(sdk)+'-clang++'),'-DPOCKET_GUEST_API='+str(sdk),
            '-nostdinc++','-isystem',api/'libcxx/include','-std=c++17','-O2','-fno-rtti','-fno-exceptions',
            '-nostdlib++','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-writable-strings']
    if sdk==29:common+=['-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H']
    else:common+=['-include',ROOT/'runtime/direct/hidl_codegen_compat.h']
    common+=['-I'+str(path) for path in includes]
    if sdk==29:common+=['-I'+str(api/'system-core/libvndksupport/include')]
    # The audio header refers to the generated record interface by pointer only.
    record=generated/'android/media/IAudioRecord.h';record.parent.mkdir(parents=True,exist_ok=True)
    record.write_text('namespace android { namespace media { class IAudioRecord; }}\n')
    event=out/'boot_uevent.o'
    run(llvm/('aarch64-linux-android'+str(sdk)+'-clang'),'-O2','-Wall','-Wextra','-Werror','-fPIC','-c',ROOT/'runtime/direct/boot_uevent.c','-o',event)
    link=['-L'+str(libs),'-Wl,-rpath-link,'+str(libs),'-lbinder','-lutils','-lc++','-llog','-ldl','-lcutils',
          '-lhwbinder','-lhidlbase','-lhidltransport','-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    if sdk==29:link+=['-lvndksupport']
    sources=[ROOT/'runtime/direct'/name for name in ['boot_hidl.cpp','boot_binder.cpp','boot_health.cpp','boot_hardware.cpp','boot_audio.cpp','boot_vibrator.cpp']]
    if sdk==29:sources+=[ROOT/'runtime/api29/power.cpp']
    hal_libraries=['-l:'+package+'.so' for package in packages if package.startswith(('android.hardware.','android.system.'))]
    run(*common,'-fPIC','-shared',*sources,event,config/'SurfaceFlingerConfigs.cpp',*hal_libraries,*link,
        '-o',out/('libpocket_hidl_api'+str(sdk)+'.so'))
    run(llvm/('aarch64-linux-android'+str(sdk)+'-clang'),'-O2','-Wall','-Wextra','-Werror',
        ROOT/'runtime/direct/boot_hw_main.c','-ldl','-o',out/('pocket-hw-services-api'+str(sdk)))
    if sdk==29:
        run(*common,'-fPIE','-pie',ROOT/'runtime/api29/hardware_probe.cpp','-L'+str(out),'-lpocket_hidl_api29',*hal_libraries,*link,
            '-o',out/'pocket-hardware-probe-api29')
        run(*common,'-fPIE','-pie',ROOT/'runtime/api29/framework_probe.cpp',*link,
            '-o',out/'pocket-framework-probe-api29')
        run(llvm/'aarch64-linux-android29-clang','-O2','-Wall','-Wextra','-Werror',
            ROOT/'runtime/direct/boot_probe.c','-ldl','-o',out/'pocket-boot-probe-api29')
        run(*common,'-fPIE','-pie',ROOT/'runtime/api29/suspend_control.cpp',*link,
            '-o',out/'pocket-suspend-control-api29')
    print('API'+str(sdk)+'_HARDWARE_BUILD_PASS',out)
    return out


def build_network(ndk,sdk=29):
    if sdk==29:
        prepare_sources(['libcxx','system-core','frameworks-native','system-vold','system-netd','system-apex'])
    api=ROOT/('out/api'+str(sdk));reference=prepare() if sdk==29 else ROOT/'out/api28/reference'
    out=ROOT/'out/api29'/('framework' if sdk==29 else 'api28-framework-regression')
    out.mkdir(parents=True,exist_ok=True);storage_protocol(out/'boot_storage_protocol.h',sdk)
    libs=reference/'system/lib64';llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    includes=[out,api/'frameworks-native/libs/binder/include']
    includes += [api/'system-core'/part/'include' for part in ['base','liblog','libutils','libcutils','libsystem']]
    cmd=[llvm/('aarch64-linux-android'+str(sdk)+'-clang++'),'-DPOCKET_GUEST_API='+str(sdk),
         '-nostdinc++','-isystem',api/'libcxx/include','-std=c++17','-O2','-fno-rtti','-fno-exceptions',
         '-nostdlib++','-Wall','-Wextra','-Werror','-Wno-misleading-indentation']
    if sdk==29:cmd+=['-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H']
    cmd+=['-I'+str(path) for path in includes]
    cmd+=['-fPIE','-pie',ROOT/'runtime/direct/boot_network.cpp','-L'+str(libs),'-Wl,-rpath-link,'+str(libs),
          '-lbinder','-lutils','-lc++','-llog','-Wl,--no-undefined','-Wl,-z,max-page-size=16384',
          '-o',out/('pocket-network-services-api'+str(sdk))]
    subprocess.run(list(map(str,cmd)),check=True)
    print('API'+str(sdk)+'_NETWORK_BUILD_PASS',out)
    return out


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--hidl-gen',type=Path,default=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/build-tools/linux-x86/bin/hidl-gen')
    p.add_argument('--sdk',type=int,choices=[28,29],default=29)
    p.add_argument('--storage',action='store_true')
    p.add_argument('--graphics',action='store_true')
    p.add_argument('--network',action='store_true')
    args=p.parse_args()
    if args.graphics:
        if args.sdk!=29:p.error('--graphics requires SDK 29')
        build_graphics(args.ndk.resolve())
    elif args.storage:build_storage(args.ndk.resolve(),args.sdk)
    elif args.network:build_network(args.ndk.resolve(),args.sdk)
    else:build_hardware(args.ndk.resolve(),args.hidl_gen.resolve(),args.sdk)
