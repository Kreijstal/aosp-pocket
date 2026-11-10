#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Link API32 native services into the supervised root-identity core."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from prepare_api32 import ROOT,CACHE,prepare
from build_api32_metrics import build_object as build_metrics_object

def build_core(ndk,reuse_generated=False,out=None,installd=False):
    if not reuse_generated:
        from build_api32_native import build_binder
        from build_api32_network import build_network
        from build_api32_storage import build_storage
        from build_api32_mdns import build_mdns
        from build_api32_memory import build_memory
        from build_api32_framework import build_apex,build_hardware
        build_binder(ndk);build_network(ndk,True);build_storage(ndk)
        build_mdns(ndk);build_memory(ndk);build_apex(ndk);build_hardware(ndk)
    reference=prepare();out=(out or CACHE/'core').resolve();out.mkdir(parents=True,exist_ok=True)
    generated=[CACHE/'native/generated',CACHE/'network/generated',CACHE/'storage/generated',CACHE/'framework/apex-generated']
    required=[generated[0]/'android/os/BnServiceManager.h',generated[1]/'NetdUnsupported32.h',
        generated[1]/'android/net/IDnsResolver.h',generated[2]/'VoldUnsupported32.h',
        generated[3]/'android/apex/BnApexService.h',CACHE/'mdns/MDnsSdListener.cpp',
        CACHE/'framework/libpocket_hidl_api32.so']
    if not all(p.is_file() for p in required):raise ValueError('Build the version-matched service generators first')
    includes=[*generated,CACHE/'framework',ROOT/'runtime/api32',ROOT/'runtime/api28',ROOT/'runtime/direct',
        CACHE/'system-netd/server',CACHE/'system-netd/include',CACHE/'system-netd/libnetdutils/include',
        CACHE/'mdnsresponder/mDNSShared',CACHE/'system-memory-lmkd/include',
        CACHE/'frameworks-native/libs/binder/include',CACHE/'frameworks-native/libs/binder/ndk/include_cpp',
        CACHE/'libbase/include',CACHE/'system-logging/liblog/include']
    includes += [CACHE/'system-core'/name/'include' for name in ['libutils','libcutils','libsystem','libsysutils']]
    compiler=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    common=[compiler,'-DPOCKET_GUEST_API=32','-DPOCKET_CORE_COMPONENT',
        '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H','-D_GNU_SOURCE',
        '-nostdinc++','-isystem',CACHE/'libcxx/include','-nostdlib++','-std=c++17','-O2',
        '-fno-rtti','-fno-exceptions','-fPIE','-Wall','-Wextra','-Werror',
        '-Wno-unused-parameter','-Wno-inconsistent-missing-override','-Wno-deprecated-declarations',
        *['-I'+str(p) for p in includes]]
    libs=reference/'system/lib64';bionic=reference/'apex/com.android.runtime/lib64/bionic'
    link=[bionic/'libc.so','-L'+str(libs),'-L'+str(bionic),'-L'+str(CACHE/'native'),
        '-Wl,-rpath-link,'+str(libs),'-Wl,-rpath-link,'+str(bionic),'-lpocket_binder_api32',
        '-L'+str(CACHE/'framework'),'-lpocket_hidl_api32',
        '-l:netd_aidl_interface-V7-cpp.so','-l:dnsresolver_aidl_interface-V7-cpp.so',
        '-l:netd_event_listener_interface-V1-cpp.so','-l:apex_aidl_interface-cpp.so',
        '-lmediametricsservice','-l:mediametricsservice-aidl-cpp.so',
        '-lbinder','-lsysutils','-lmdnssd','-lnetdutils','-lcutils','-lbase','-lutils','-llog','-lc++',
        '-Wl,--no-undefined','-Wl,-z,max-page-size=16384']
    def run(*args):subprocess.run(list(map(str,args)),check=True)
    units=[(ROOT/'runtime/api32'/(name+'_service.cpp'),name) for name in ['network','dns','memory','apex','storage']]
    units += [(ROOT/'runtime/api32/mdns_main.cpp','mdns')]
    renamed={'network':'netd','dns':'dns','memory':'memory','apex':'apex','storage':'storage','mdns':'mdns'}
    objects=[build_metrics_object(ndk,out)]
    installd_info=None
    if installd:
        from build_api32_installd_frontend import build as build_installd
        installd_dir=build_installd(ndk,out/'installd',component=True)
        installd_info=json.loads((installd_dir/'build.json').read_text())
        # Link every source-built object, including the complete generated
        # interface, rather than allowing an archive to omit unused methods.
        objects += [Path(name) for name in installd_info['objects']]
        link += installd_info['linkLibraries']
        common += ['-DPOCKET_CORE_INSTALLD']
    for source,name in units:
        target=out/(name+'.o');run(*common,'-Dmain=pocket_core_'+renamed[name],'-c',source,'-o',target);objects.append(target)
    sources=[ROOT/'runtime/api32/service_registry.cpp',CACHE/'mdns/MDnsSdListener.cpp',
        CACHE/'system-netd/server/NetdCommand.cpp',*sorted((CACHE/'storage/generated/src').rglob('*.cpp'))]
    for i,source in enumerate(sources):
        target=out/('support-'+str(i)+'.o');run(*common,'-c',source,'-o',target);objects.append(target)
    for test in [False,True]:
        extra=['-DPOCKET_CORE_TEST_ECHO'] if test else []
        main=out/('main-test.o' if test else 'main.o');run(*common,*extra,'-c',ROOT/'runtime/api32/core_main.cpp','-o',main)
        test_objects=[]
        if test:
            echo=out/'echo.o';run(*common,*extra,'-Dmain=pocket_core_unused_probe_main','-c',ROOT/'runtime/api32/service_probe.cpp','-o',echo);test_objects.append(echo)
        target=out/('pocket-core-test-api32' if test else 'pocket-core-api32')
        run(compiler,'-fPIE','-pie',main,*objects,*test_objects,*link,'-o',target)
    (out/'build.json').write_text(json.dumps({'sdk':32,'serviceUid':0,'components':8,
        'optionalMediaMetricsComponent':True,'normalRuntimeComponents':10 if installd else 9,
        'normalRuntimeProcessesSaved':9 if installd else 8,
        'optionalInstalldComponent':installd,'installdSourceBuild':installd_info,
        'installdLogicalUid':0 if installd else None,'installdLogicalGid':0 if installd else None,
        'installdEnabledBinderDefaultMinNice':0 if installd else None,
        'installdDisabledBinderDefaultMinNice':19,
        'installdWorkerEnvironment':'scoped-execve-without-daemon-preloads' if installd else None,
        'mediaMetricsLogicalUid':0,'standaloneMediaMetricsLogicalUid':1013,
        'mediaMetricsRecordLimits':'stock','mediaMetricsStandaloneAddressSpaceLimitRetained':False,
        'rootProcessesSaved':8 if installd else 7,'interfacesUnchanged':True,'runtimeVerified':False,
        'hardwareLibrarySha256':hashlib.sha256((CACHE/'framework/libpocket_hidl_api32.so').read_bytes()).hexdigest(),
        'stockMediaMetricsLibrarySha256':hashlib.sha256((reference/'system/lib64/libmediametricsservice.so').read_bytes()).hexdigest(),
        'metricsProbeSha256':hashlib.sha256((out/'pocket-metrics-probe-api32').read_bytes()).hexdigest(),
        'sourceSha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in [*(p for p,_ in units),ROOT/'runtime/api32/core_main.cpp',ROOT/'runtime/api32/metrics_core.cpp',
                ROOT/'runtime/api32/service_activation.h',ROOT/'runtime/api32/lazy_service_runtime.h',
                ROOT/'runtime/api32/external_volume.h',ROOT/'runtime/api32/external_open_rpc.h',
                ROOT/'runtime/api32/external_app_dirs.h',
                ROOT/'runtime/api32/external_files_rpc.h',ROOT/'runtime/api32/external_kernel.h',
                ROOT/'runtime/api32/directory_handles.h',ROOT/'runtime/api32/fuse_files.h',
                ROOT/'runtime/api32/fuse_namespace.h',ROOT/'runtime/api32/fuse_peer.h',ROOT/'runtime/api32/fuse_transport.h',
                ROOT/'runtime/loader/external_protocol.h',
                ROOT/'runtime/tests/api32/metrics_core_probe.cpp',
                ROOT/'tools/build_api32_metrics.py',ROOT/'tools/build_api32_core.py']}},indent=2)+'\n')
    print('API32_CORE_BUILD_PASS',out)
    return out

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,required=True)
    parser.add_argument('--reuse-generated',action='store_true')
    parser.add_argument('--installd',action='store_true')
    parser.add_argument('--out',type=Path)
    args=parser.parse_args()
    build_core(args.ndk.resolve(),args.reuse_generated,args.out,args.installd)
