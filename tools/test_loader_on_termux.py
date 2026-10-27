#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the isolated native loader fixture in Termux, without ADB or root.

Requires --fixture from tools/build_exec_loader.py. Never accesses Pocket's
private image or data. Tests the guest-identity path that touches Binder BSS.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import time
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--root', type=Path, default=Path.home()/'pocket-loader-layout-test')
    args = parser.parse_args()
    root = args.root.resolve()
    if root.exists():
        assert (root/'.loader-layout-test').is_file(), 'Refusing to reuse an unrelated directory'
    else:
        root.mkdir(mode=0o700)
        (root/'.loader-layout-test').write_text('Isolated loader regression; no Pocket app data\n')
    with zipfile.ZipFile(args.fixture) as archive:
        for entry in archive.infolist():
            if not entry.filename.startswith('assets/root/') or entry.is_dir():
                continue
            relative = Path(entry.filename[len('assets/root/'):])
            assert not relative.is_absolute() and '..' not in relative.parts
            target = root/relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(entry))
            target.chmod(0o755)
    loader = root/'system/bin/pocket-loader'
    data = bytearray(loader.read_bytes())
    assert data.count(b'PCKELF01') == 1
    at = data.index(b'PCKELF01')+8
    for path in (root, loader, root/'system/bin/linker64'):
        encoded = str(path).encode()+b'\0'
        assert len(encoded) <= 1024
        data[at:at+1024] = encoded.ljust(1024, b'\0')
        at += 1024
    loader.write_bytes(data)
    for name in ('sh', 'toybox'):
        binary = root/'system/bin'/name
        data = bytearray(binary.read_bytes())
        offset, = struct.unpack_from('<Q', data, 32)
        stride, count = struct.unpack_from('<HH', data, 54)
        for i in range(count):
            p = struct.unpack_from('<IIQQQQQQ', data, offset+i*stride)
            if p[0] == 3:
                encoded = str(loader).encode()+b'\0'
                assert len(encoded) <= p[5], 'Fixture interpreter reservation is too small'
                data[p[2]:p[2]+p[5]] = encoded.ljust(p[5], b'\0')
                break
        else:
            raise AssertionError('Missing prepared interpreter: '+name)
        binary.write_bytes(data)
    (root/'.pocket-owner').mkdir(exist_ok=True)
    results = []
    for guest_identity in (False, True):
        env = {'POCKET_LOADER_BOOT': '1'} if guest_identity else {}
        command = ('echo NATIVE_SHELL_OK; /system/bin/toybox id -u; '
                   '/system/bin/sh -c "/system/bin/toybox cat /fixture.txt"; '
                   '/system/bin/toybox cat /proc/self/status; '
                   '/system/bin/toybox cat /proc/self/maps')
        result = subprocess.run([str(root/'system/bin/sh'), '-c', command],
                                cwd=root, env=env, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, (guest_identity, result.returncode, result.stderr)
        lines = result.stdout.splitlines()
        assert lines[:3] == ['NATIVE_SHELL_OK', '0' if guest_identity else str(os.getuid()), 'loader-fixture'], lines[:3]
        tracer = next(line for line in lines if line.startswith('TracerPid:'))
        uid = next(line for line in lines if line.startswith('Uid:'))
        assert int(tracer.split()[1]) == 0
        assert set(map(int, uid.split()[1:])) == {os.getuid()}, uid
        maps = [line for line in lines if len(line.split(None, 5)) == 6 and
                line.split(None, 5)[5] in ('/system/bin/pocket-loader', '/system/bin/linker64', '/system/lib64/libc.so')]
        assert {line.split(None, 5)[5] for line in maps} == {'/system/bin/pocket-loader', '/system/bin/linker64', '/system/lib64/libc.so'}
        results.append({'guestIdentity': guest_identity, 'exitCode': result.returncode,
                        'logicalUid': int(lines[1]), 'kernelUid': os.getuid(), 'tracerPid': 0,
                        'mappingSegments': maps, 'shellAndNestedExec': 'PASS'})
    report = {'result': 'PASS', 'device': subprocess.check_output(['getprop', 'ro.product.model'], text=True).strip(),
              'kernel': os.uname().release, 'checkedAtUtc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'scope': 'Isolated Termux child processes; no full Manager boot claim', 'runs': results}
    (root/'validation.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
