#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the complete pinned 12L installd daemon before shared-core integration."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from prepare_api32 import ROOT, CACHE, prepare, prepare_aidl, prepare_sources

UNITS = [
    'CacheItem.cpp', 'CacheTracker.cpp', 'CrateManager.cpp',
    'InstalldNativeService.cpp', 'QuotaUtils.cpp', 'dexopt.cpp',
    'execv_helper.cpp', 'globals.cpp', 'run_dex2oat.cpp', 'unique_file.cpp',
    'utils.cpp', 'utils_default.cpp', 'view_compiler.cpp',
]


def build(ndk, out, component=False):
    prepare_sources(['frameworks-native', 'bionic', 'libcxx', 'system-core',
                     'system-logging', 'libbase', 'libselinux', 'boringssl',
                     'server-configurable-flags'])
    reference = prepare()
    out = out.resolve()
    generated = out / 'generated'
    generated.mkdir(parents=True, exist_ok=True)
    upstream = CACHE / 'frameworks-native/cmds/installd'
    bp = (upstream / 'Android.bp').read_text()
    defaults = bp.split('name: "installd_defaults",', 1)[1].split('shared_libs:', 1)[0]
    # Keep the whole daemon: app-data, cache, quota, dexopt and worker paths.
    import re
    assert re.findall(r'"([A-Za-z0-9_]+[.]cpp)"', defaults) == UNITS
    commands = []

    def run(*args):
        command = list(map(str, args))
        commands.append(command)
        subprocess.run(command, check=True)

    definitions = upstream / 'binder'
    aidls = sorted(definitions.rglob('*.aidl'))
    run(prepare_aidl(), '--lang=cpp', '--out=' + str(generated / 'src'),
        '--header_out=' + str(generated), '-I' + str(definitions), *aidls)
    llvm = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    compiler = llvm / 'aarch64-linux-android32-clang++'
    includes = [
        generated, upstream, CACHE / 'libbase/include',
        CACHE / 'system-logging/liblog/include',
        CACHE / 'system-logging/logwrapper/include',
        CACHE / 'frameworks-native/libs/binder/include',
        CACHE / 'libselinux/libselinux/include',
        CACHE / 'boringssl/src/include',
        CACHE / 'server-configurable-flags/libflags/include',
        CACHE / 'bionic/libc/async_safe/include',
        CACHE / 'bionic/libc',
        CACHE / 'bionic/libc/platform',
    ]
    includes += [CACHE / 'system-core' / name / 'include'
                 for name in ['libutils', 'libcutils', 'libsystem', 'libprocessgroup']]
    common = [
        compiler, '-D_GNU_SOURCE', '-DPOCKET_GUEST_API=32',
        '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H',
        '-nostdinc++', '-isystem', CACHE / 'libcxx/include', '-nostdlib++',
        '-std=c++17', '-O2', '-fno-rtti', '-fno-exceptions', '-fPIE',
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        # The pinned platform build predates this clang diagnostic.
        '-Wno-unused-but-set-variable',
        '-Wno-inconsistent-missing-override', '-Wno-deprecated-declarations',
        '-ffile-prefix-map=' + str(out) + '=pocket-installd-generated',
        '-ffile-prefix-map=' + str(CACHE) + '=pocket-api32-source',
        *['-I' + str(path) for path in includes],
    ]
    sources = [upstream / name for name in [*UNITS, 'installd.cpp']]
    sources += sorted((generated / 'src').rglob('*.cpp'))
    # libasync_safe is static in the upstream build. Compile its real source.
    sources.append(CACHE / 'bionic/libc/async_safe/async_safe_log.cpp')
    patched = None
    if component:
        import shutil
        patched = out / 'component'
        patched.mkdir(exist_ok=True)
        expected = {
            'installd.cpp': 'c46fb6184172c20e5d062e69c67a47d4f1b1c003debeb495963d0adac6b7a7fb',
            'InstalldNativeService.cpp': 'b36e5ea58ed5da2af03060ffd666d3556b9ab47c12eb9442d2c53c52d6575048',
        }
        for name, digest in expected.items():
            assert hashlib.sha256((upstream / name).read_bytes()).hexdigest() == digest
            shutil.copyfile(upstream / name, patched / name)
        patch = ROOT / 'runtime/api32/installd-frontend.patch'
        command = ['patch', '--batch', '--fuzz=0', '-p1', '-i', str(patch)]
        commands.append(command)
        subprocess.run(command, cwd=patched, check=True)
        sources = [patched / p.name if p.parent == upstream and p.name in expected else p
                   for p in sources]
        common += ['-DPOCKET_INSTALLD_COMPONENT', '-Wno-unused-function',
                   '-Dis_selinux_enabled=pocketInstalldSelinuxEnabled32',
                   '-Dselinux_android_restorecon=pocketInstalldRestorecon32',
                   '-Dselinux_android_restorecon_pkgdir=pocketInstalldRestoreconPkgdir32',
                   '-Dexecv=pocketInstalldExecv32']
    objects = []
    for index, source in enumerate(sources):
        target = out / ('unit-' + str(index) + '.o')
        run(*common, '-c', source, '-o', target)
        objects.append(target)
    if component:
        policy = out / 'installd-component-policy.o'
        run(llvm / 'aarch64-linux-android32-clang', '-std=c11', '-D_GNU_SOURCE',
            '-O2', '-Wall', '-Wextra', '-Werror', '-fPIE',
            '-I' + str(CACHE / 'libselinux/libselinux/include'),
            ROOT / 'runtime/api32/installd_component_policy.c', '-c', '-o', policy)
        objects.append(policy)
    libs = reference / 'system/lib64'
    bionic = reference / 'apex/com.android.runtime/lib64/bionic'
    link = [
        bionic / 'libc.so', '-L' + str(libs), '-L' + str(bionic),
        '-Wl,-rpath-link,' + str(libs), '-Wl,-rpath-link,' + str(bionic),
        '-lbase', '-lbinder', '-lcrypto', '-lcutils', '-llog', '-llogwrap',
        '-lprocessgroup', '-lselinux', '-lutils',
        '-l:server_configurable_flags.so', '-lc++',
        '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384',
    ]
    target = out / ('installd-component.a' if component else 'pocket-installd-source-api32')
    if component:run(llvm / 'llvm-ar', 'rcs', target, *objects)
    else:run(compiler, '-fPIE', '-pie', *objects, *link, '-o', target)
    inputs = [*sources, *aidls, upstream / 'Android.bp',
              *sorted(upstream.glob('*.h')), Path(__file__).resolve(),
              ROOT / 'sources.lock.json']
    if component:
        inputs += [ROOT / 'runtime/api32/installd-frontend.patch',
                   ROOT / 'runtime/api32/installd_component_policy.c',
                   ROOT / 'runtime/api32/installd_worker_env.h']
    receipt = {
        'status': 'PASS_FULL_INSTALLD_COMPONENT_OBJECT_BUILD' if component else 'PASS_FULL_PINNED_INSTALLD_SOURCE_BUILD',
        'sdk': 32, 'upstreamImplementationUnits': UNITS,
        'sourceSha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in inputs},
        'binarySha256': hashlib.sha256(target.read_bytes()).hexdigest(),
        'binaryBytes': target.stat().st_size,
        'commands': commands, 'upstreamDaemonModified': component,
        'componentMode': component,
        'componentOnlyStartupPatch': component,
        'permissionAndDataMethodBodiesUnchanged': True,
        'objects': [str(p) for p in objects],
        'linkLibraries': list(map(str,link)),
        'sharedCoreIntegrationImplemented': False, 'runtimeVerified': False,
        'processBudgetVerified': False, 'productionReleaseReady': False,
    }
    (out / 'build.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(receipt['status'], target)
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=CACHE / 'installd-source')
    parser.add_argument('--component', action='store_true')
    args = parser.parse_args()
    build(args.ndk.resolve(), args.out, args.component)
