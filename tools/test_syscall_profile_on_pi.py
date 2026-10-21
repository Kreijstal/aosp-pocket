#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the selected-openat prototype inside the Pi's already paired Pocket guest.

Copy this script, pocketctl.py and the probe to the Pi. No host root is used.
The existing Cuttlefish helper guards against accidentally targeting a phone.
"""
import argparse
import io
import json
from pathlib import Path
import re
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path.home()/'.config/pocket-pi-debug.json')
    parser.add_argument('--probe', type=Path, default=Path.home()/'syscall-profile-probe')
    parser.add_argument('--fixture-parent', default='/data/user/0/dev.kreijstal.aosppocket.runtime/files/api28-rootfs/system-data/local/tmp')
    parser.add_argument('--output', type=Path, default=Path.home()/'pocket-syscall-profile-validation.json')
    args = parser.parse_args()
    sys.path.insert(0, '/home/pi/lineage-cuttlefish/tools')
    import cuttlefish_arm64 as c
    from pocketctl import request
    c.require_virtual()
    config = json.loads(args.config.read_text())

    def call(op, argv=(), source=None):
        out, err = io.BytesIO(), io.BytesIO()
        code = request(config, op, argv, source=source, stdout=out, stderr=err)
        return code, out.getvalue().decode(errors='replace'), err.getvalue().decode(errors='replace')

    code, text, error = call(1)
    assert code == 0, error
    state = json.loads(text)
    assert state['sdk'] == 28 and state['ready'], 'Start the paired Android 9 guest first'
    target = '/data/local/tmp/pocket-syscall-profile-probe'
    with args.probe.open('rb') as source:
        code, text, error = call(2, ['/system/bin/sh', '-c',
            '/system/bin/toybox cat > "$1.next" && /system/bin/toybox chmod 755 "$1.next" && /system/bin/toybox mv "$1.next" "$1"',
            'upload', target], source)
    assert code == 0, error
    code, text, error = call(2, ['/system/bin/toybox', 'timeout', '45', target, args.fixture_parent])
    print(text, end='', flush=True)
    if error:
        print(error, file=sys.stderr, end='', flush=True)
    passed = re.findall(r'^PASS: ([\w-]+) status=0$', text, re.M)
    uid = re.search(r'kernel_uid=(\d+)', text)
    report = {'guest': 'Pocket Android 9 inside Pi ARM64 Cuttlefish VM',
        'command': 'python3 tools/test_syscall_profile_on_pi.py',
        'exitCode': code, 'passed': passed, 'stdout': text, 'stderr': error,
        'kernelUid': int(uid[1]) if uid else None,
        'productionAppLaunchIntegration': False, 'filesystemSandbox': False}
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    assert code == 0 and len(passed) == 8 and uid and int(uid[1]) != 0, report
    print('PASS: eight cases in ordinary Pocket host UID; no host root used')


if __name__ == '__main__':
    main()
