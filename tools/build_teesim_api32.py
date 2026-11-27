#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the API 32 attestation identity library (libpocket_teesim_api32.so).

Unlike the Android 9 adapter, this library does not link TEESimulator's Rust
KeyMint TA: on API 32 the guest's keystore2 talks to a Keymaster 4.1 HIDL HAL,
so only the HAL's boot record can be replaced without a KeyMint-to-KM4 bridge.
See /home/pi/work/teesim/REPORT.md, stage 2.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'runtime/attestation/api32'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path,
                        default=Path.home()/'.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c')
    parser.add_argument('--out-dir', type=Path, default=ROOT/'out/teesim-api32')
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    llvm = args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    if not (llvm/'aarch64-linux-android32-clang++').is_file():
        parser.error('NDK not found at %s' % args.ndk)
    release = args.out_dir/'libpocket_teesim_api32.so'
    subprocess.run([
        str(llvm/'aarch64-linux-android32-clang++'), '-std=c++17', '-O2', '-fPIC', '-shared',
        '-fno-rtti', '-fno-exceptions', '-nostdlib++', '-Wall', '-Wextra', '-Werror',
        '-Wl,-z,max-page-size=16384', '-Wl,--exclude-libs,ALL', '-Wl,--no-undefined',
        '-o', str(release), str(SOURCE/'boot_context.cpp')], check=True)
    subprocess.run([str(llvm/'llvm-strip'), '--strip-unneeded', str(release)], check=True)
    validation = {
        'api': 32, 'abi': 'arm64-v8a', 'nativeBuild': 'PASS',
        'interposed': [
            '_ZNK9keymaster24PureSoftKeymasterContext21GetVerifiedBootParamsEP17keymaster_error_t',
            '_ZNK9keymaster22SoftAttestationContext21GetVerifiedBootParamsEP17keymaster_error_t',
        ],
        'activation': 'system-data/misc/keystore/pocket-attestation-backend = teesimulator '
                      'and system-data/misc/keystore/pocket-teesim.json (owner, 0600)',
        'preloadTarget': 'pocket-keymaster (/system/bin/pocket-crypto-api32)',
        'privateKeysBundled': False,
        'sha256': hashlib.sha256(release.read_bytes()).hexdigest(),
        'size': release.stat().st_size,
    }
    (args.out_dir/'build-validation32.json').write_text(json.dumps(validation, indent=2) + '\n')
    print('PASS: API 32 adapter:', release, validation['size'], 'bytes')


if __name__ == '__main__':
    main()
