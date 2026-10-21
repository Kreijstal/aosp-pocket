#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the pinned TEESimulator Android 9 adapter against a complete Pocket APK."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REVISION = '2fda3b1c67f3f00f1a1d343cee760eeb26a190bc'
SOURCE = ROOT/'out/teesimulator-source'
ADAPTER = ROOT/'runtime/attestation'


def run(*args, cwd=ROOT, env=None):
    subprocess.run(list(map(str, args)), cwd=cwd, env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-apk', type=Path, required=True)
    parser.add_argument('--out-dir', type=Path, default=ROOT/'out/teesim-api28')
    parser.add_argument('--ndk', type=Path,
                        default=Path.home()/'.local/share/android-neutral-test/sdk/ndk/android-ndk-r27c')
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    args.out_dir = args.out_dir.resolve()
    args.ndk = args.ndk.resolve()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    if not (ROOT/'out/api28/frameworks-native/libs/binder/include/binder/Parcel.h').is_file():
        parser.error('Prepare the pinned Android 9 sources first (tools/prepare_api28.py)')
    if not SOURCE.exists():
        run('git', 'init', SOURCE)
        run('git', 'remote', 'add', 'origin', 'https://github.com/JingMatrix/TEESimulator.git', cwd=SOURCE)
        run('git', 'fetch', '--depth', '1', 'origin', REVISION, cwd=SOURCE)
        run('git', 'checkout', '--detach', REVISION, cwd=SOURCE)
    actual = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != REVISION:
        parser.error('Unexpected TEESimulator revision; preserve that checkout and use the pinned source')
    patch = ADAPTER/'teesimulator-api28.patch'
    applied = subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)],
                             cwd=SOURCE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    if not applied:
        run('git', 'apply', '--check', patch, cwd=SOURCE)
        run('git', 'apply', patch, cwd=SOURCE)
    run('git', 'submodule', 'update', '--init', '--depth', '1', '--jobs', '3',
        'third_party/keymint', 'third_party/boringssl', 'third_party/lsplt', cwd=SOURCE)
    shutil.copyfile(ADAPTER/'Cargo.lock', SOURCE/'rust/Cargo.lock')
    tools = ROOT/'out/teesim-tools'
    if not (tools/'bin/cargo-ndk').is_file():
        run('cargo', 'install', 'cargo-ndk', '--version', '4.1.2', '--locked', '--root', tools)
    libraries = args.out_dir/'lib64'
    libraries.mkdir(exist_ok=True)
    with zipfile.ZipFile(args.base_apk) as apk:
        prefix = 'assets/api28-rootfs/system/lib64/'
        for entry in apk.infolist():
            if entry.filename.startswith(prefix) and entry.filename.endswith('.so.payload'):
                name = entry.filename[len(prefix):-8]
                if '/' not in name:
                    (libraries/name).write_bytes(apk.read(entry))
        linker = args.out_dir/'linker64'
        linker.write_bytes(apk.read('assets/api28-rootfs/system/bin/linker64.payload'))
        linker.chmod(0o755)
    machine = struct.unpack_from('<H', (libraries/'libc.so').read_bytes(), 18)[0]
    if machine != 183:
        parser.error('The validated adapter currently targets the ARM64 Pocket distribution')
    env = dict(os.environ, PATH=str(tools/'bin')+os.pathsep+os.environ.get('PATH', ''),
               NDK_HOME=str(args.ndk), ABI='arm64-v8a', API='28')
    run('bash', 'rust/build.sh', cwd=SOURCE, env=env)
    if (SOURCE/'rust/Cargo.lock').read_bytes() != (ADAPTER/'Cargo.lock').read_bytes():
        raise RuntimeError('Cargo changed the pinned lockfile; inspect dependency changes before packaging')
    run('cmake', '-S', ADAPTER, '-B', args.out_dir/'build', '-G', 'Ninja',
        '-DCMAKE_TOOLCHAIN_FILE='+str(args.ndk/'build/cmake/android.toolchain.cmake'),
        '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-28', '-DANDROID_STL=c++_static',
        '-DCMAKE_BUILD_TYPE=Release', '-DTEESIM_SOURCE='+str(SOURCE),
        '-DAPI28_SOURCE='+str(ROOT/'out/api28'), '-DGUEST_LIBS='+str(libraries),
        '-DRUST_TA='+str(SOURCE/'rust/target/aarch64-linux-android/release/libteesim_km.a'),
        '-DGUEST_LINKER='+str(linker))
    run('cmake', '--build', args.out_dir/'build', '--target',
        'pocket_teesim_api28', 'pocket_teesim_wire_test', '-j', args.jobs)
    llvm = args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    release = args.out_dir/'libpocket_teesim_api28.so'
    shutil.copyfile(args.out_dir/'build/libpocket_teesim_api28.so', release)
    run(llvm/'llvm-strip', '--strip-unneeded', release)
    run('python3', ROOT/'tools/build_exec_runtime.py', '--base-apk', args.base_apk,
        '--out-dir', args.out_dir/'exec-runtime', '--ndk', args.ndk)
    validation = {'upstreamRevision': REVISION, 'api': 28, 'abi': 'arm64-v8a',
                  'nativeBuild': 'PASS', 'deviceBinderValidation': 'pending',
                  'activation': 'owner config /data/misc/keystore/pocket-teesim.json, mode 0600',
                  'privateKeysBundled': False}
    (args.out_dir/'build-validation.json').write_text(json.dumps(validation, indent=2)+'\n')
    print('PASS: Android 9 adapter:', release)


if __name__ == '__main__':
    main()
