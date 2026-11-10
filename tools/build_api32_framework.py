#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build version-matched Android 12L framework service adapters."""
import argparse
import json
from pathlib import Path
import subprocess
from prepare_api32 import ROOT, CACHE, prepare, prepare_sources, prepare_hidl, prepare_aidl


def build_hardware(ndk):
    prepare_sources(['libcxx', 'system-core', 'libbase', 'system-logging', 'libhidl',
                     'libhwbinder', 'libfmq', 'frameworks-native', 'hardware-interfaces', 'system-hardware-interfaces'])
    reference = prepare()
    out = CACHE / 'framework'
    generated = out / 'generated'
    generated.mkdir(parents=True, exist_ok=True)
    compiler = prepare_hidl()
    def run(*args):
        subprocess.run(list(map(str, args)), check=True)
    packages = ['android.hidl.base@1.0', 'android.hidl.manager@1.0', 'android.hidl.manager@1.1',
                'android.hidl.manager@1.2', 'android.system.suspend@1.0',
                'android.hardware.configstore@1.0', 'android.hardware.configstore@1.1',
                'android.hardware.health@1.0', 'android.hardware.health@2.0']
    # Local audio stream identity adapters need the typed7.0 declarations;
    # they do not add audio DSOs to every process's preload dependencies.
    header_packages = ['android.hidl.memory@1.0', 'android.hidl.safe_union@1.0',
                       'android.hardware.audio.common@7.0', 'android.hardware.audio@7.0']
    for package in packages + header_packages:
        run(compiler, '-o', generated, '-Lc++-headers', '-randroid.hidl:' + str(CACHE / 'libhidl/transport'),
            '-randroid.hardware:' + str(CACHE / 'hardware-interfaces'),
            '-randroid.system:' + str(CACHE / 'system-hardware-interfaces'), package)
    suspend = CACHE / 'system-hardware-interfaces/suspend'
    public = suspend / 'aidl/aidl_api/android.system.suspend.control/1'
    digest = (public / '.hash').read_text().splitlines()[-1]
    assert digest == '34506b107801d68c881c2c7368ad4c676aed3e9b'
    aidl = prepare_aidl()
    for definitions, flags, inputs in [
            (public, ['--version=1', '--hash=' + digest], sorted(public.rglob('*.aidl'))),
            (suspend / 'aidl', [], sorted((suspend / 'aidl/android/system/suspend/internal').glob('*.aidl')))]:
        run(aidl, '--lang=cpp', '--structured', *flags, '--out=' + str(generated / 'src'),
            '--header_out=' + str(generated), '-I' + str(definitions), *inputs)
    # Reuse the pinned stock callback/death-recipient implementation without
    # its kernel SystemSuspend backend. Its internal-service implementation is
    # replaced below by the host-delegated adapter, with explicit unsupported
    # responses for kernel operations. Preserve the upstream license headers.
    suspend_default = suspend / '1.0/default'
    public_source = (suspend_default / 'SuspendControlService.cpp').read_text()
    boundary = 'void SuspendControlServiceInternal::setSuspendService'
    assert public_source.count(boundary) == 1
    public_source = public_source.split(boundary)[0].replace('#include "SystemSuspend.h"', '')
    public_source = public_source.replace('using ::android::base::Result;', '')
    (generated / 'SuspendControlService.cpp').write_text(public_source + '\n} } } }\n')
    (generated / 'SuspendControlService.h').write_text(
        '#include <algorithm>\n#include <map>\n#include <mutex>\n#include <vector>\n' +
        (suspend_default / 'SuspendControlService.h').read_text())
    config = CACHE / 'hardware-interfaces/configstore/1.1/default'
    includes = [generated, ROOT / 'runtime/api28', ROOT / 'runtime/direct', CACHE / 'libbase/include',
                CACHE / 'system-logging/liblog/include', CACHE / 'libhwbinder/include',
                CACHE / 'libhidl/base/include', CACHE / 'libhidl/transport/include', CACHE / 'libfmq/include', CACHE / 'libfmq/base', config,
                CACHE / 'frameworks-native/libs/binder/include', suspend_default]
    includes += [CACHE / 'system-core' / part / 'include' for part in ['libutils', 'libcutils', 'libsystem']]
    llvm = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    common = [llvm / 'aarch64-linux-android32-clang++', '-DPOCKET_GUEST_API=32',
              '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H', '-nostdinc++', '-isystem', CACHE / 'libcxx/include',
              '-std=c++17', '-O2', '-fno-rtti', '-fno-exceptions', '-nostdlib++', '-Wall', '-Wextra', '-Werror',
              '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-missing-field-initializers',
              '-Wno-inconsistent-missing-override']
    common += ['-I' + str(path) for path in includes]
    libs = reference / 'system/lib64'
    bionic = reference / 'apex/com.android.runtime/lib64/bionic'
    link = [bionic / 'libc.so', '-L' + str(libs), '-L' + str(bionic),
            '-Wl,-rpath-link,' + str(libs), '-Wl,-rpath-link,' + str(bionic),
            '-lhidlbase', '-lutils', '-lcutils', '-lbase', '-lc++', '-llog', '-lbinder',
            '-l:android.system.suspend.control-V1-cpp.so', '-l:android.system.suspend.control.internal-cpp.so',
            '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384']
    link += ['-l:' + package + '.so' for package in packages if package.startswith(('android.hardware.', 'android.system.'))]
    sources = [ROOT / 'runtime/direct/boot_hardware.cpp', ROOT / 'runtime/direct/boot_health.cpp',
               ROOT / 'runtime/api29/power.cpp', ROOT / 'runtime/api32/hidl_services.cpp',
               ROOT / 'runtime/api32/audio_local_identity.cpp',
               ROOT / 'runtime/api32/suspend_control.cpp', generated / 'SuspendControlService.cpp',
               suspend_default / 'WakeLockEntryList.cpp',
               config / 'SurfaceFlingerConfigs.cpp']
    run(*common, '-fPIC', '-shared', *sources, *link, '-o', out / 'libpocket_hidl_api32.so')
    run(llvm / 'aarch64-linux-android32-clang', '-O2', '-Wall', '-Wextra', '-Werror',
        ROOT / 'runtime/direct/boot_hw_main.c', '-ldl', '-Wl,-z,max-page-size=16384',
        '-o', out / 'pocket-hw-services-api32')
    run(*common, '-fPIE', '-pie', ROOT / 'runtime/api32/hardware_probe.cpp',
        '-L' + str(out), '-lpocket_hidl_api32', *link, '-o', out / 'pocket-hardware-probe-api32')
    run(*common, '-fPIE', '-pie', ROOT / 'runtime/api32/suspend_probe.cpp',
        '-L' + str(out), '-lpocket_hidl_api32', *link, '-o', out / 'pocket-suspend-probe-api32')
    print('API32_HARDWARE_BUILD_PASS', out)
    return out


def build_apex(ndk):
    prepare_sources(['libcxx', 'system-core', 'libbase', 'system-logging', 'frameworks-native', 'system-apex'])
    reference = prepare()
    out = CACHE / 'framework'
    generated = out / 'apex-generated'
    generated.mkdir(parents=True, exist_ok=True)
    definitions = CACHE / 'system-apex/apexd/aidl'
    subprocess.run([str(prepare_aidl()), '--lang=cpp', '--out=' + str(generated / 'src'),
        '--header_out=' + str(generated), '-I' + str(definitions)] +
        [str(path) for path in sorted((definitions / 'android/apex').glob('*.aidl'))], check=True)
    modules = json.loads((reference / '.pocket-api32-reference.json').read_text())['apexModules']
    records, arrays = [], []
    for index, module in enumerate(modules):
        name = module['name']
        packages = [path for path in (reference / 'system/apex').iterdir() if path.stem == name or
                    (path.stem == 'com.android.vndk.current' and name.startswith('com.android.vndk.'))]
        assert len(packages) == 1, name
        manifest = (reference / 'apex' / name / 'apex_manifest.pb').read_bytes()
        arrays.append('static const unsigned char manifest_' + str(index) + '[]={' +
                      ','.join(str(value) for value in manifest) + '};')
        values = [json.dumps(name), json.dumps('/apex/' + name),
                  json.dumps('/' + packages[0].relative_to(reference).as_posix()),
                  json.dumps(module['versionName']), str(module['version']), 'manifest_' + str(index), str(len(manifest))]
        records.append('{' + ','.join(values) + '},')
    (out / 'apex_modules32.h').write_text('// Generated from the verified API32 image manifests.\n' +
        '\n'.join(arrays) + '\nstatic const Module modules[]={\n' + '\n'.join(records) + '\n};\n')
    compiler = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    includes = [generated, out, ROOT / 'runtime/api28', CACHE / 'libbase/include',
                CACHE / 'system-logging/liblog/include', CACHE / 'frameworks-native/libs/binder/include']
    includes += [CACHE / 'system-core' / part / 'include' for part in ['libutils', 'libcutils', 'libsystem']]
    common = [str(compiler), '-DPOCKET_GUEST_API=32', '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H',
              '-nostdinc++', '-isystem', str(CACHE / 'libcxx/include'), '-std=c++17', '-O2', '-fno-rtti',
              '-fno-exceptions', '-nostdlib++', '-Wall', '-Wextra', '-Werror']
    common += ['-I' + str(path) for path in includes]
    libs = reference / 'system/lib64'
    bionic = reference / 'apex/com.android.runtime/lib64/bionic'
    link = [str(bionic / 'libc.so'), '-L' + str(libs), '-L' + str(bionic),
            '-Wl,-rpath-link,' + str(libs), '-Wl,-rpath-link,' + str(bionic),
            '-l:apex_aidl_interface-cpp.so', '-lbinder', '-lutils', '-lc++', '-llog',
            '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384']
    subprocess.run(common + ['-fPIE', '-pie', str(ROOT / 'runtime/api32/apex_service.cpp')] + link +
                   ['-o', str(out / 'pocket-apex-service-api32')], check=True)
    print('API32_APEX_BUILD_PASS', out, 'modules=' + str(len(modules)))
    return out


def build_installd(ndk,probe_only=False):
    prepare_sources(['libcxx', 'system-core', 'libbase', 'system-logging', 'frameworks-native', 'libselinux'])
    reference = prepare()
    out = CACHE / 'framework'
    generated = out / 'installd-generated'
    generated.mkdir(parents=True, exist_ok=True)
    definitions = CACHE / 'frameworks-native/cmds/installd/binder'
    subprocess.run([str(prepare_aidl()), '--lang=cpp', '--out=' + str(generated / 'src'),
        '--header_out=' + str(generated), '-I' + str(definitions)] +
        [str(path) for path in sorted(definitions.rglob('*.aidl'))], check=True)
    llvm = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    includes = [generated, ROOT / 'runtime/api28', CACHE / 'frameworks-native/cmds/installd', CACHE / 'libbase/include',
                CACHE / 'system-logging/liblog/include', CACHE / 'frameworks-native/libs/binder/include']
    includes += [CACHE / 'system-core' / part / 'include' for part in ['libutils', 'libcutils', 'libsystem']]
    common = [str(llvm / 'aarch64-linux-android32-clang++'), '-DPOCKET_GUEST_API=32',
              '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H', '-nostdinc++', '-isystem', str(CACHE / 'libcxx/include'),
              '-std=c++17', '-O2', '-fno-rtti', '-fno-exceptions', '-nostdlib++', '-Wall', '-Wextra', '-Werror']
    common += ['-I' + str(path) for path in includes]
    libs = reference / 'system/lib64'
    bionic = reference / 'apex/com.android.runtime/lib64/bionic'
    link = [str(bionic / 'libc.so'), '-L' + str(libs), '-L' + str(bionic),
            '-Wl,-rpath-link,' + str(libs), '-Wl,-rpath-link,' + str(bionic),
            '-lbinder', '-lutils', '-lc++', '-llog', '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384']
    sources = [ROOT / 'runtime/api32/installd_probe.cpp', *sorted((generated / 'src').rglob('*.cpp'))]
    subprocess.run(common + ['-fPIE', '-pie'] + list(map(str, sources)) + link +
        ['-o', str(out / 'pocket-installd-probe-api32')], check=True)
    if probe_only:
        print('API32_INSTALLD_CLIENT_BUILD_PASS',out/'pocket-installd-probe-api32')
        return out
    subprocess.run(common + ['-fPIE', '-pie', str(ROOT / 'runtime/api32/framework_probe.cpp')] + link +
        ['-o', str(out / 'pocket-framework-probe-api32')], check=True)
    subprocess.run([str(llvm / 'aarch64-linux-android32-clang'), '-O2', '-Wall', '-Wextra', '-Werror',
        str(ROOT / 'runtime/direct/boot_probe.c'), '-ldl', '-Wl,-z,max-page-size=16384',
        '-o', str(out / 'pocket-boot-probe-api32')], check=True)
    subprocess.run([str(llvm / 'aarch64-linux-android32-clang'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-D_GNU_SOURCE', '-fPIC', '-shared', '-I' + str(CACHE / 'libselinux/libselinux/include'),
        str(ROOT / 'runtime/api32/service_policy.c'), '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384',
        '-o', str(out / 'libpocket_policy_api32.so')], check=True)
    subprocess.run([str(llvm / 'aarch64-linux-android32-clang'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-D_GNU_SOURCE', '-DPOCKET_FRAMEWORK_POLICY', '-fPIC', '-shared', '-I' + str(CACHE / 'libselinux/libselinux/include'),
        str(ROOT / 'runtime/api32/service_policy.c'), '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384',
        '-Wl,-soname,libpocket_framework_policy_api32.so', '-o', str(out / 'libpocket_framework_policy_api32.so')], check=True)
    print('API32_INSTALLD_PROBE_BUILD_PASS', out)
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    choice = parser.add_mutually_exclusive_group()
    choice.add_argument('--apex', action='store_true')
    choice.add_argument('--installd', action='store_true')
    choice.add_argument('--installd-probe-only', action='store_true')
    args = parser.parse_args()
    if args.installd_probe_only:build_installd(args.ndk.resolve(),probe_only=True)
    else:(build_installd if args.installd else build_apex if args.apex else build_hardware)(args.ndk.resolve())
