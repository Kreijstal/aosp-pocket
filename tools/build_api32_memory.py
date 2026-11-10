#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the rootless API 32 guest memory supervisor and protocol probe."""
import argparse
from pathlib import Path
import subprocess
from prepare_api32 import ROOT, CACHE, prepare, prepare_sources


def build_memory(ndk):
    prepare_sources(['libcxx', 'system-memory-lmkd'])
    reference = prepare()
    out = CACHE / 'memory'
    out.mkdir(parents=True, exist_ok=True)
    compiler = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang++'
    bionic = reference / 'apex/com.android.runtime/lib64/bionic'
    libs = reference / 'system/lib64'
    common = [str(compiler), '-std=c++17', '-O2', '-fno-rtti', '-fno-exceptions',
              '-nostdinc++', '-nostdlib++', '-isystem', str(CACHE / 'libcxx/include'),
              '-D_LIBCPP_SUPPORT_XLOCALE_POSIX_L_FALLBACK_H', '-D_GNU_SOURCE',
              '-I'+str(CACHE / 'system-memory-lmkd/include'),
              '-Wall', '-Wextra', '-Werror', '-fPIE', '-pie']
    link = [str(bionic / 'libc.so'), '-L'+str(libs), '-L'+str(bionic),
            '-Wl,-rpath-link,'+str(libs), '-Wl,-rpath-link,'+str(bionic), '-lc++',
            '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384']
    for source, target in [('memory_service.cpp', 'pocket-memory-api32'),
                           ('memory_probe.cpp', 'pocket-memory-probe-api32')]:
        subprocess.run(common + [str(ROOT / 'runtime/api32' / source)] + link +
                       ['-o', str(out / target)], check=True)
    print('API32_MEMORY_BUILD_PASS', out)
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, required=True)
    args = parser.parse_args()
    build_memory(args.ndk.resolve())
