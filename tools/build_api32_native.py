#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the Android 12L native service adapters against the pinned API 32 ABI."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
from prepare_api32 import ROOT, CACHE, prepare, prepare_sources, prepare_aidl


def build_properties(ndk):
    prepare_sources(['bionic', 'libcxx', 'system-core', 'libbase', 'system-logging'])
    reference = prepare()
    out = CACHE / 'native'
    headers = out / 'headers/sys'
    headers.mkdir(parents=True, exist_ok=True)
    shutil.copy2(CACHE / 'bionic/libc/include/sys/_system_properties.h', headers / '_system_properties.h')
    compiler = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    property_root = CACHE / 'system-core/property_service'
    serializer, parser = property_root / 'libpropertyinfoserializer', property_root / 'libpropertyinfoparser'
    includes = [out / 'headers', ROOT / 'runtime/api28', CACHE / 'libbase/include',
                CACHE / 'system-logging/liblog/include', serializer / 'include', parser / 'include']
    includes += [CACHE / 'system-core' / name / 'include' for name in ['libutils', 'libcutils', 'libsystem']]
    common = [str(compiler), '-DPOCKET_GUEST_API=32', '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H', '-nostdinc++', '-isystem', str(CACHE / 'libcxx/include'),
              '-std=c++17', '-O2', '-fno-rtti', '-fno-exceptions', '-nostdlib++', '-Wall', '-Wextra', '-Werror', '-fPIE', '-pie']
    common += ['-I' + str(path) for path in includes]
    libs = reference / 'system/lib64'
    bionic = reference / 'apex/com.android.runtime/lib64/bionic'
    link = [str(bionic / 'libc.so'), '-L' + str(libs), '-L' + str(bionic), '-Wl,-rpath-link,' + str(libs),
            '-Wl,-rpath-link,' + str(bionic), '-lbase', '-lc++', '-llog', '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384']
    sources = [serializer / name for name in ['property_info_serializer.cpp', 'trie_builder.cpp', 'trie_serializer.cpp']]
    sources += [parser / 'property_info_parser.cpp', ROOT / 'runtime/api28/property_info.cpp', ROOT / 'runtime/api28/property_service.cpp']
    for files, name in [(sources, 'pocket-property-service-api32'), ([ROOT / 'runtime/api28/property_probe.cpp'], 'pocket-property-probe-api32'),
                        ([ROOT/'runtime/api32/property_set.cpp'],'pocket-setprop-api32')]:
        subprocess.run(common + list(map(str, files)) + link + ['-o', str(out / name)], check=True)
    (out / 'properties-build.json').write_text(json.dumps({'sdk': 32, 'component': 'properties', 'nativeBuild': 'PASS', 'runtimeVerified': False}, indent=2) + '\n')
    print('API32_PROPERTIES_BUILD_PASS', out)
    return out


def build_binder(ndk):
    prepare_sources(['libcxx', 'system-core', 'libbase', 'system-logging', 'frameworks-native'])
    reference = prepare()
    out = CACHE / 'native'
    out.mkdir(parents=True, exist_ok=True)
    aidl = prepare_aidl()
    definitions = CACHE / 'frameworks-native/libs/binder/aidl'
    generated = out / 'generated'
    generated.mkdir(parents=True,exist_ok=True)
    manager_source=(CACHE/'frameworks-native/libs/binder/IServiceManager.cpp').read_text()
    declaration='class ServiceManagerShim : public IServiceManager'
    assert manager_source.count(declaration)==1
    shim=manager_source.split(declaration,1)[1].split('\n};',1)[0]
    license=manager_source.split('#define LOG_TAG',1)[0]
    (generated/'pocket_service_manager_shim32.h').write_text(license+declaration+shim+'\n};\n')
    subprocess.run([str(aidl), '--lang=cpp', '--out=' + str(out / 'aidl'), '--header_out=' + str(generated),
        '-I' + str(definitions)] + [str(definitions / ('android/os/' + name + '.aidl'))
        for name in ['IServiceManager', 'IServiceCallback', 'IClientCallback', 'ServiceDebugInfo']], check=True)
    compiler = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    includes = [generated, ROOT / 'runtime/api28', CACHE / 'frameworks-native/libs/binder/include', CACHE / 'libbase/include',
                CACHE / 'system-logging/liblog/include']
    includes += [CACHE / 'system-core' / name / 'include' for name in ['libutils', 'libcutils', 'libsystem']]
    common = [str(compiler), '-DPOCKET_GUEST_API=32', '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H',
              '-nostdinc++', '-isystem', str(CACHE / 'libcxx/include'), '-std=c++17', '-O2', '-fno-rtti', '-fno-exceptions',
              '-nostdlib++', '-Wall', '-Wextra', '-Werror']
    common += ['-I' + str(path) for path in includes]
    libs = reference / 'system/lib64'
    bionic = reference / 'apex/com.android.runtime/lib64/bionic'
    link = [str(bionic / 'libc.so'), '-L' + str(libs), '-L' + str(bionic), '-Wl,-rpath-link,' + str(libs),
            '-Wl,-rpath-link,' + str(bionic), '-lbinder', '-lutils', '-lc++', '-llog', '-ldl',
            '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384']
    # Delegate to the stock API 32 token implementation after adding guest-role
    # metadata for one-way calls. Retain SYST, WorkSource, both overloads and RPC
    # framing; the shared dispatch adapter checks the sender and scopes identity.
    subprocess.run(common + ['-fPIC', '-shared', str(ROOT / 'runtime/api32/binder_bridge.cpp'),
        str(ROOT / 'runtime/api32/service_activation.cpp'),
        str(ROOT / 'runtime/api32/guest_credentials.cpp'),
        str(ROOT / 'runtime/direct/boot_binder.cpp')] + link + ['-Wl,-soname,libpocket_binder_api32.so',
        '-o', str(out / 'libpocket_binder_api32.so')], check=True)
    # The generated Bn/Bp implementation is already exported by stock libbinder;
    # only its version-matched headers enter our private registry implementation.
    subprocess.run(common + ['-fPIE', '-pie', str(ROOT / 'runtime/api32/service_probe.cpp'),
        str(ROOT / 'runtime/api32/service_registry.cpp'), '-L' + str(out), '-lpocket_binder_api32'] + link +
        ['-o', str(out / 'pocket-binder-probe-api32')], check=True)
    subprocess.run(common + ['-fPIE', '-pie', str(ROOT / 'runtime/api32/servicemanager_main.cpp'),
        str(ROOT / 'runtime/api32/service_registry.cpp'), '-L' + str(out), '-lpocket_binder_api32'] + link +
        ['-o', str(out / 'pocket-servicemanager-api32')], check=True)
    print('API32_BINDER_BUILD_PASS', out)
    return out


def build_init(ndk):
    out = build_properties(ndk)
    prepare_sources(['fmtlib', 'system-tools-hidl'])
    reference = prepare()
    source = out / 'init-source'
    if source.exists():
        shutil.rmtree(source)
    shutil.copytree(CACHE / 'system-core/init', source, symlinks=True)
    subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(ROOT / 'runtime/api32/init-rootless.patch')], cwd=source, check=True)
    compiler = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    property_root = CACHE / 'system-core/property_service'
    serializer, parser = property_root / 'libpropertyinfoserializer', property_root / 'libpropertyinfoparser'
    includes = [out / 'headers', source, ROOT / 'runtime/api28', ROOT / 'runtime/loader',
                CACHE / 'libbase/include', CACHE / 'fmtlib/include', CACHE / 'system-tools-hidl/utils/include',
                CACHE / 'system-logging/liblog/include', serializer / 'include', parser / 'include']
    includes += [CACHE / 'system-core' / part / 'include' for part in ['libutils', 'libcutils', 'libsystem']]
    common = [str(compiler), '-DPOCKET_GUEST_API=32', '-DPOCKET_INIT_INTEGRATION',
              '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H', '-nostdinc++', '-isystem', str(CACHE / 'libcxx/include'),
              '-std=c++17', '-O2', '-fno-rtti', '-fno-exceptions', '-nostdlib++',
              '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra', '-Werror',
              '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-missing-field-initializers']
    common += ['-I' + str(path) for path in includes]
    libs = reference / 'system/lib64'
    bionic = reference / 'apex/com.android.runtime/lib64/bionic'
    link = [str(bionic / 'libc.so'), '-L' + str(libs), '-L' + str(bionic), '-Wl,-rpath-link,' + str(libs),
            '-Wl,-rpath-link,' + str(bionic), '-lbase', '-lcutils', '-lhidl-gen-utils', '-lc++', '-llog',
            '-Wl,--no-undefined', '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384']
    sources = [source / (name + '.cpp') for name in ['action', 'action_manager', 'action_parser',
        'parser', 'tokenizer', 'import_parser', 'service', 'service_list', 'service_parser', 'service_utils',
        'interface_utils', 'rlimit_parser', 'util']]
    sources += [serializer / name for name in ['property_info_serializer.cpp', 'trie_builder.cpp', 'trie_serializer.cpp']]
    sources += [parser / 'property_info_parser.cpp', ROOT / 'runtime/api28/property_info.cpp',
                ROOT / 'runtime/api28/property_service.cpp', ROOT / 'runtime/api32/init_main.cpp']
    subprocess.run(common + ['-fPIE', '-pie'] + list(map(str, sources)) + link +
                   ['-o', str(out / 'pocket-init-api32')], check=True)
    subprocess.run(common + ['-fPIE', '-pie', str(ROOT / 'runtime/api32/init_test.cpp')] + link +
                   ['-o', str(out / 'pocket-init-test-api32')], check=True)
    print('API32_INIT_BUILD_PASS', out)
    return out


def build_trace(ndk):
    prepare_sources(['art'])
    out=CACHE/'native';out.mkdir(parents=True,exist_ok=True)
    compiler=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang'
    subprocess.run(list(map(str,[compiler,'-std=c11','-D_GNU_SOURCE','-O2',
        '-Wall','-Wextra','-Werror','-fPIC','-shared','-I'+str(CACHE/'art/libartpalette/include'),
        ROOT/'runtime/api32/art_trace.c','-ldl','-Wl,--no-undefined','-Wl,-z,max-page-size=16384',
        '-Wl,-soname,libpocket_trace_api32.so','-o',out/'libpocket_trace_api32.so'])),check=True)
    print('API32_TRACE_BUILD_PASS',out)
    return out


def build_zygote(ndk):
    prepare_sources(['bionic', 'libcxx', 'system-core', 'libbase', 'system-logging',
                     'frameworks-native', 'frameworks-base', 'libnativehelper', 'art', 'packages-statsd'])
    reference = prepare()
    out = CACHE / 'native'
    out.mkdir(parents=True, exist_ok=True)
    source = out / 'zygote-source'
    if source.exists():
        shutil.rmtree(source)
    shutil.copytree(CACHE / 'frameworks-base', source, symlinks=True, ignore=shutil.ignore_patterns('.git', '.clang-format'))
    subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(ROOT / 'runtime/api32/zygote-rootless.patch')], cwd=source, check=True)
    subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(ROOT / 'runtime/api32/startup-trace.patch')], cwd=source, check=True)
    subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(ROOT / 'runtime/api32/system-server-bridge.patch')], cwd=source, check=True)
    compiler = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    jni = source / 'core/jni'
    includes = [ROOT / 'runtime/api28', ROOT / 'runtime/loader', jni, jni / 'include',
                CACHE / 'bionic/libc/private', CACHE / 'bionic/libc/platform', CACHE / 'bionic/libc/async_safe/include',
                CACHE / 'frameworks-native/libs/binder/include', CACHE / 'libnativehelper/include',
                CACHE / 'libnativehelper/include_jni', CACHE / 'libnativehelper/include_platform', CACHE / 'libnativehelper/header_only_include',
                CACHE / 'libbase/include', CACHE / 'system-logging/liblog/include', CACHE / 'packages-statsd/lib/libstatssocket/include']
    includes += [CACHE / 'system-core' / part / 'include' for part in ['libutils', 'libcutils', 'libsystem']]
    common = [str(compiler), '-DPOCKET_GUEST_API=32', '-nostdinc++', '-isystem', str(CACHE / 'libcxx/include'),
              '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H', '-std=c++17', '-O2', '-fno-rtti', '-fno-exceptions',
              '-nostdlib++', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function',
              '-Wno-missing-field-initializers', '-Wno-writable-strings']
    common += ['-I' + str(path) for path in includes]
    libraries = [reference / name for name in ['system/lib64', 'apex/com.android.runtime/lib64/bionic',
                                              'apex/com.android.art/lib64', 'apex/com.android.os.statsd/lib64']]
    link = [str(libraries[1] / 'libc.so')]
    for path in libraries:
        link += ['-L' + str(path), '-Wl,-rpath-link,' + str(path)]
    link += ['-landroid_runtime', '-lbinder', '-lutils', '-lcutils', '-lbase', '-lc++', '-llog', '-ldl',
             '-lnativehelper', '-lstatssocket', '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384']
    build_trace(ndk)
    subprocess.run(common + ['-fPIC', '-shared', str(jni / 'com_android_internal_os_Zygote.cpp'),
        str(jni / 'fd_utils.cpp'), str(ROOT / 'runtime/api32/zygote_bootstrap.cpp'),
        str(ROOT / 'runtime/api32/microg_bootstrap32.cpp')] + link +
        ['-o', str(out / 'libpocket_zygote_api32.so')], check=True)
    # API 32 app_process uses the shared ART sigchain, unlike the API 29 static
    # arrangement. Force its otherwise-unused DT_NEEDED entry before libc and
    # retain the platform executable's export restrictions.
    subprocess.run(common + ['-fPIE', '-pie', str(source / 'cmds/app_process/app_main.cpp'),
        '-Wl,--version-script=' + str(source / 'cmds/app_process/version-script.txt'),
        '-Wl,--no-as-needed', '-lsigchain', '-lwilhelm'] + link +
        ['-o', str(out / 'pocket-app-process-api32')], check=True)
    subprocess.run([str(compiler.parent / 'aarch64-linux-android32-clang'), '-O2', '-Wall', '-Wextra', '-Werror',
        str(ROOT / 'runtime/tests/api32/zygote_launcher.c'), '-Wl,-z,max-page-size=16384',
        '-o', str(out / 'pocket-zygote-launcher-api32')], check=True)
    subprocess.run([str(compiler), '-O2', '-Wall', '-Wextra', '-Werror', '-nostdlib++',
        str(ROOT / 'runtime/api32/zygote_client.cpp'), '-Wl,-z,max-page-size=16384',
        '-o', str(out / 'pocket-zygote-client-api32')], check=True)
    print('API32_ZYGOTE_BUILD_PASS', out)
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    choice = parser.add_mutually_exclusive_group()
    choice.add_argument('--binder', action='store_true')
    choice.add_argument('--zygote', action='store_true')
    choice.add_argument('--init', action='store_true')
    args = parser.parse_args()
    (build_init if args.init else build_zygote if args.zygote else build_binder if args.binder else build_properties)(args.ndk.resolve())
