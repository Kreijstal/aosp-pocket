#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Rebuild only the direct libc shim against a bundled Manager's exact ABI."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zipfile

from build_boot import TARGETS

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-apk', type=Path, required=True)
    parser.add_argument('--out-dir', type=Path, required=True)
    parser.add_argument('--ndk', type=Path, required=True)
    args = parser.parse_args()
    with zipfile.ZipFile(args.base_apk) as apk:
        libc = apk.read('assets/api28-rootfs/system/lib64/libc.so.payload')
        hooks = json.loads(apk.read('assets/direct-boot-hooks.json'))
    assert libc[:6] == b'\x7fELF\x02\x01', 'Expected little-endian ELF64'
    machine, = struct.unpack_from('<H', libc, 18)
    triple = {183: 'aarch64-linux-android', 62: 'x86_64-linux-android'}[machine]
    offset, = struct.unpack_from('<Q', libc, 32)
    stride, count = struct.unpack_from('<HH', libc, 54)
    assert stride == 56
    segments = [struct.unpack_from('<IIQQQQQQ', libc, offset+i*stride) for i in range(count)]
    assert set(hooks['libc_hooks']) == set(TARGETS), 'Bundled hook set differs from this runtime'
    lines = ['// Verified against the exact libc in the supplied Manager.', 'static Hook hooks[] = {']
    for name, hook in hooks['libc_hooks'].items():
        assert hook['target'] == TARGETS[name]
        address = hook['offset']
        expected = bytes.fromhex(hook['expected'])
        assert len(expected) == 16
        segment = next(p for p in segments if p[0] == 1 and p[3] <= address and address+16 <= p[3]+p[5])
        file_offset = segment[2]+address-segment[3]
        assert libc[file_offset:file_offset+16] == expected, 'Bundled libc hook bytes differ: '+name
        lines.append('{"%s",0x%x,{%s},(void*)%s},' %
                     (name, address, ','.join(map(str, expected)), hook['target']))
    state = hooks['property_state']
    assert state['size'] == 0x450
    lines += ['};', '#define PROPERTY_STATE_OFFSET 0x%x' % state['offset'],
              '#define PROPERTY_STATE_SIZE %d' % state['size']]
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir/'boot_hooks.h').write_text('\n'.join(lines)+'\n')
    (args.out_dir/'libc.so').write_bytes(libc)
    compiler = args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'/(triple+'28-clang')
    command = [compiler, '-std=c11', '-O2', '-g', '-fPIC', '-shared', '-Wall', '-Wextra', '-Werror',
               '-Wno-misleading-indentation', '-DPOCKET_DIRECT_BOOT', '-I'+str(args.out_dir),
               ROOT/'runtime/direct/guest_paths.c', ROOT/'runtime/direct/boot_runtime.c',
               args.out_dir/'libc.so', '-ldl', '-Wl,--no-undefined', '-Wl,-z,max-page-size=16384',
               '-Wl,-soname,libpocket_direct_boot.so', '-o', args.out_dir/'libpocket_direct_boot.so']
    subprocess.run(list(map(str, command)), check=True)
    print('PASS:', triple, 'direct runtime rebuilt against', len(TARGETS), 'verified libc hooks')


if __name__ == '__main__':
    main()
