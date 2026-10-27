#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare Attest Probe's guest proc paths against files and outer-kernel maps.

Requires the running native Manager, Attest Probe >=1.9 in the outer VM, the
staged RuntimeProbeRunner JAR, and the existing paired Pi owner console.
Only Attest Probe is relaunched. No guest restart or security change is made.
"""
import argparse
import io
import json
import os
from pathlib import Path
import re
import shlex
import sys
import time

sys.path.insert(0, '/home/pi/lineage-cuttlefish/tools')
sys.path.insert(0, '/home/pi')
import cuttlefish_arm64 as c
from pocketctl import request

PROBE = 'dev.kreijstal.attestprobe'
MANAGER = 'dev.kreijstal.aosppocket.runtime'
REPORT = '/data/user/0/' + PROBE + '/files/runtime-checks.json'
ROOT = '/data/user/0/' + MANAGER + '/files/api28-native-rootfs'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--guest-apk', help='Already staged guest APK pathname; update the probe before launching')
    args = parser.parse_args()
    c.require_virtual()
    config = json.loads((Path.home()/'.config/pocket-pi-debug.json').read_text())

    def root(command):
        return c.shell('su -c ' + shlex.quote(command))

    def rpc(operation, argv=()):
        out, err = io.BytesIO(), io.BytesIO()
        code = request(config, operation, argv, stdout=out, stderr=err)
        if code:
            raise RuntimeError((code, out.getvalue().decode(errors='replace'), err.getvalue().decode(errors='replace')))
        return out.getvalue().decode()

    def package_uid(package):
        listing = c.shell('cmd package list packages -U ' + package)
        match = re.search(r'package:' + re.escape(package) + r' uid:(\d+)', listing)
        if not match:
            raise RuntimeError('Missing package: ' + package)
        return int(match[1])

    def probe_pids(uid):
        found = []
        for line in root('ps -A -o UID,PID,NAME').splitlines()[1:]:
            fields = line.split()
            if len(fields) == 3 and fields[0] == str(uid) and fields[2] == PROBE:
                found.append(int(fields[1]))
        return found

    state = json.loads(rpc(1))
    assert state['ready'] and state['running'] and state['sdk'] == 28, state
    outer_uid, guest_kernel_uid = package_uid(PROBE), package_uid(MANAGER)
    root('rm -f ' + REPORT)
    print(c.shell('am start -f 0x10008000 -n ' + PROBE + '/.VisionTestActivity --ez runtime_only true'), flush=True)
    outer = None
    for _ in range(60):
        raw = root('if [ -f ' + REPORT + ' ]; then cat ' + REPORT + '; fi')
        try:
            outer = json.loads(raw)
            break
        except ValueError:
            time.sleep(1)
    assert outer is not None, 'Outer probe did not publish a new report'

    # Its existing Activity otherwise only comes to the front and does not
    # rerun onCreate. Restart the guest probe process, leaving other apps alone.
    for pid in probe_pids(guest_kernel_uid):
        root('kill -TERM ' + str(pid))
    rpc(2, ['/system/bin/toybox', 'rm', '-f', REPORT])
    command = ('export CLASSPATH=/system/framework/pocket-zygote.jar:/data/local/tmp/runtime-attest-runner.jar; '
               'exec /system/bin/pocket-app-process-api28 /system/bin RuntimeProbeRunner')
    if args.guest_apk:
        command += ' ' + shlex.quote(args.guest_apk)
    print(rpc(2, ['/system/bin/sh', '-c', command]), flush=True)
    guest = None
    for _ in range(60):
        try:
            guest = json.loads(rpc(2, ['/system/bin/toybox', 'cat', REPORT]))
            break
        except (RuntimeError, ValueError):
            time.sleep(1)
    assert guest is not None, 'Guest probe did not publish a new report'
    for label, report in [('outer', outer), ('guest', guest)]:
        data = report['checks']['runtime_bionic_mappings']['data']
        assert data['allMappedFilesMatch'] and not data['mismatchingFiles'] and not data['unavailableFiles'], (label, data)
        assert report['checks']['native_tracer']['data']['tracerPid'] == 0, (label, report)
    mappings = guest['checks']['runtime_bionic_mappings']['data']
    assert set(mappings['paths']) == {'/system/bin/pocket-loader', '/system/bin/linker64', '/system/lib64/libc.so'}, mappings

    guest_pids, outer_pids = probe_pids(guest_kernel_uid), probe_pids(outer_uid)
    assert len(guest_pids) == len(outer_pids) == 1, (guest_pids, outer_pids)
    raw_maps = root('cat /proc/' + str(guest_pids[0]) + '/maps')

    def visible(path):
        if path.startswith(ROOT + '/system-data/'):
            return '/data/' + path[len(ROOT + '/system-data/'):]
        return path[len(ROOT):] if path.startswith(ROOT + '/') else path

    raw_records = set()
    data_files = []
    seen_data = set()
    for line in raw_maps.splitlines():
        fields = line.split(None, 5)
        if len(fields) != 6:
            continue
        fields[5] = visible(fields[5])
        raw_records.add(tuple(fields))
        if fields[5].startswith('/data/') and not fields[5].endswith(' (deleted)') and fields[5] not in seen_data and len(data_files) < 3:
            major, minor = (int(part, 16) for part in fields[3].split(':'))
            expected = (os.makedev(major, minor), int(fields[4]))
            try:
                actual = tuple(map(int, rpc(2, ['/system/bin/toybox', 'stat', '-c', '%d %i', fields[5]]).split()))
            except RuntimeError:
                continue
            assert actual == expected, (fields[5], actual, expected)
            seen_data.add(fields[5])
            data_files.append({'path': fields[5], 'device': actual[0], 'inode': actual[1]})
    for segment in mappings['segments']:
        assert tuple(segment.split(None, 5)) in raw_records, ('Guest mapping changed or omitted kernel fields', segment)
    assert data_files, 'No guest /data mapping was available to verify the backing-directory alias'
    kernel = {}
    for label, pid in [('outer', outer_pids[0]), ('guest', guest_pids[0])]:
        status = root('cat /proc/' + str(pid) + '/status')
        tracer = re.search(r'^TracerPid:\s+(\d+)$', status, re.MULTILINE)
        assert tracer and int(tracer[1]) == 0, (label, status)
        kernel[label] = {'pid': pid, 'tracerPid': int(tracer[1])}
    result = {'result': 'PASS', 'checkedAtUtc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'command': shlex.join(['python3', '/home/pi/validate_proc_view_on_pi.py', *sys.argv[1:]]),
              'outerVm': outer, 'pocketGuest': guest, 'kernel': kernel, 'dataAliasFiles': data_files,
              'mappingSegmentsVerifiedAgainstKernel': len(mappings['segments']), 'guestStatus': json.loads(rpc(1))}
    destination = Path.home()/'proc-view-manager-validation.json'
    destination.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'result': 'PASS', 'paths': mappings['paths'], 'kernel': kernel,
                      'dataAliasFiles': data_files, 'report': str(destination)}, indent=2), flush=True)


if __name__ == '__main__':
    main()
