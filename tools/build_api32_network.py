#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the typed Android 12L netd adapter against frozen AIDL version 7."""
import argparse
from pathlib import Path
import re
import subprocess
from prepare_api32 import ROOT, CACHE, prepare, prepare_sources, prepare_aidl


def build_network(ndk, dns=False):
    prepare_sources(['libcxx', 'system-core', 'libbase', 'system-logging', 'frameworks-native', 'system-netd'])
    if dns:prepare_sources(['dnsresolver'])
    reference = prepare()
    out = CACHE / 'network'
    generated = out / 'generated'
    generated.mkdir(parents=True, exist_ok=True)
    definitions = CACHE / 'system-netd/server/aidl_api/netd_aidl_interface/7'
    digest = (definitions / '.hash').read_text().splitlines()[-1]
    assert digest == '850353de5d19a0dd718f8fd20791f0532e6a34c7'
    subprocess.run([str(prepare_aidl()), '--lang=cpp', '--structured', '--version=7', '--hash=' + digest,
        '--out=' + str(generated / 'src'), '--header_out=' + str(generated), '-I' + str(definitions)] +
        list(map(str, sorted(definitions.rglob('*.aidl')))), check=True)
    # Preserve stock typed dispatch for every method. Unsupported host-kernel
    # operations return an error; no generic success reply is manufactured.
    declarations = (generated / 'android/net/INetd.h').read_text().splitlines()
    methods = []
    for line in declarations:
        if not line.strip().startswith('virtual ::android::binder::Status ') or not line.endswith(' = 0;'):
            continue
        declaration = line.strip().removeprefix('virtual ').removesuffix(' = 0;')
        declaration = declaration.split(' __attribute__', 1)[0]
        name = re.search(r'Status (\w+)\(', declaration).group(1)
        methods.append(declaration + ' override { return unsupportedNetd32("' + name + '"); }')
    assert len(methods) == 107, 'Unexpected frozen netd method set'
    (generated / 'NetdUnsupported32.h').write_text(
        '// Generated from the pinned frozen API 7.\n#pragma once\n'
        '#include <android/net/BnNetd.h>\n#include <stdio.h>\n#include <errno.h>\n'
        'inline android::binder::Status unsupportedNetd32(const char* operation) {\n'
        ' fprintf(stderr,"API32_NETD_UNSUPPORTED operation=%s host_kernel=unchanged\\n",operation);\n'
        ' return android::binder::Status::fromServiceSpecificError(EOPNOTSUPP,"Operation unavailable in the host-delegated guest");\n}\n'
        'class NetdUnsupported32 : public android::net::BnNetd { public:\n' + '\n'.join(methods) + '\n};\n')
    includes = [generated, ROOT / 'runtime/direct', CACHE / 'system-netd/include', CACHE / 'frameworks-native/libs/binder/include',
                CACHE / 'libbase/include', CACHE / 'system-logging/liblog/include']
    includes += [CACHE / 'system-core' / part / 'include' for part in ['libutils', 'libcutils', 'libsystem']]
    compiler = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    common = [str(compiler), '-DPOCKET_GUEST_API=32', '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H',
        '-nostdinc++', '-isystem', str(CACHE / 'libcxx/include'), '-std=c++17', '-O2', '-fno-rtti',
        '-fno-exceptions', '-nostdlib++', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-Wno-deprecated-declarations', '-Wno-inconsistent-missing-override', '-fPIE', '-pie']
    common += ['-I' + str(path) for path in includes]
    libs = reference / 'system/lib64'
    bionic = reference / 'apex/com.android.runtime/lib64/bionic'
    link = [str(bionic / 'libc.so'), '-L' + str(libs), '-L' + str(bionic),
        '-Wl,-rpath-link,' + str(libs), '-Wl,-rpath-link,' + str(bionic),
        '-l:netd_aidl_interface-V7-cpp.so', '-lbinder', '-lutils', '-lc++', '-llog',
        '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384']
    for source, target in [('network_service.cpp', 'pocket-netd-api32'), ('network_probe.cpp', 'pocket-network-probe-api32')]:
        subprocess.run(common + [str(ROOT / 'runtime/api32' / source)] + link + ['-o', str(out / target)], check=True)
    if dns:
        resolver = CACHE / 'dnsresolver/aidl_api/dnsresolver_aidl_interface/7'
        events = CACHE / 'system-netd/server/aidl_api/netd_event_listener_interface/1'
        resolver_hash = (resolver / '.hash').read_text().splitlines()[-1]
        event_hash = (events / '.hash').read_text().splitlines()[-1]
        assert resolver_hash == 'a1dc9394598357ccaa74e96f564e7f248b72bad2'
        assert event_hash == '8e27594d285ca7c567d87e8cf74766c27647e02b'
        for definitions, version, api_hash in [(events,1,event_hash),(resolver,7,resolver_hash)]:
            subprocess.run([str(prepare_aidl()), '--lang=cpp', '--structured', '--version=' + str(version),
                '--hash=' + api_hash, '--out=' + str(generated / 'src'), '--header_out=' + str(generated),
                '-I' + str(resolver), '-I' + str(events)] + list(map(str,sorted(definitions.rglob('*.aidl')))),check=True)
        dns_link = link + ['-l:dnsresolver_aidl_interface-V7-cpp.so', '-l:netd_event_listener_interface-V1-cpp.so']
        for source, target in [('dns_service.cpp','pocket-dns-api32'),('dns_probe.cpp','pocket-dns-probe-api32')]:
            subprocess.run(common + [str(ROOT / 'runtime/api32' / source)] + dns_link + ['-o', str(out / target)],check=True)
        print('API32_DNS_BUILD_PASS',out)
    print('API32_NETWORK_BUILD_PASS', out)
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--dns', action='store_true')
    args=parser.parse_args()
    build_network(args.ndk.resolve(), args.dns)
