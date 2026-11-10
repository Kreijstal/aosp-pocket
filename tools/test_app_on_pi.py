#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Install and launch an unchanged APK in the isolated API 29 Manager fixture.

Run on the Pi after building with build_api29_manager.py --app-probe. The fixture
must already have its pinned Android 10 image installed. No real Pocket profile
is modified, and no account enrollment or message sending is performed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import sys
import time

sys.path.insert(0, '/home/pi/lineage-cuttlefish/tools')
import cuttlefish_arm64 as c
from test_api29_on_pi import vm_network_mode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manager', type=Path, required=True)
    parser.add_argument('--apk', type=Path, required=True)
    parser.add_argument('--package', required=True)
    parser.add_argument('--out', type=Path, default=Path.home() / 'pocket-app-validation')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+', args.package):
        parser.error('Invalid application package')
    for path in (args.manager, args.apk):
        if not path.is_file():
            parser.error('Missing APK: ' + str(path))
    c.require_virtual()  # Fail before writes if ADB is unauthorized or this is a phone.
    package = 'dev.kreijstal.aosppocket.api29manager'
    files = '/data/user/0/' + package + '/files/'

    def adb(*argv, timeout=40):
        result = c.adb('-s', c.serial(), *argv, timeout=timeout, check=False)
        if result.returncode:
            raise RuntimeError((result.stdout + result.stderr)[-3000:])
        return result.stdout

    def owned(command):
        return adb('shell', 'su -c ' + shlex.quote(command))

    assert owned('test -f ' + files + 'api29-native-rootfs/system/etc/pocket-api29.env && echo ready').strip() == 'ready', 'Initialize the isolated API 29 image first'
    args.out.mkdir(parents=True, exist_ok=True)
    temporary = '/data/local/tmp/pocket-compatibility.apk'
    try:
        with vm_network_mode(True) as environment:
            adb('install', '--no-incremental', '-r', str(args.manager), timeout=180)
            adb('shell', 'pm grant ' + package + ' android.permission.POST_NOTIFICATIONS')
            adb('shell', 'am force-stop ' + package)
            adb('push', str(args.apk), temporary, timeout=180)
            # Preserve owner and mode by writing the existing fixture-owned file.
            uid = re.search(r'uid:(\d+)', adb('shell', 'pm list packages -U ' + package))
            assert uid, 'Fixture UID is missing'
            owned('cat ' + temporary + ' > ' + files + 'compatibility.apk && chown ' + uid[1] + ':' + uid[1] + ' ' + files + 'compatibility.apk && chmod 600 ' + files + 'compatibility.apk')
            previous = files + 'compatibility-result.json'
            owned('if [ -f ' + previous + ' ]; then mv ' + previous + ' ' + previous + '.' + str(time.time_ns()) + '; fi')
            adb('shell', 'am start -n ' + package + '/dev.kreijstal.aosppocket.runtime.AppProbe --es app_package ' + args.package)
            deadline = time.monotonic() + 650
            while time.monotonic() < deadline:
                raw = owned('if [ -f ' + previous + ' ]; then cat ' + previous + '; fi')
                if raw.strip():
                    report = json.loads(raw)
                    break
                time.sleep(2)
            else:
                raise RuntimeError('App fixture timed out; guest data and logs retained')
            report.update(hostNetworkEnvironment=environment,
                          apkSha256=hashlib.file_digest(args.apk.open('rb'), 'sha256').hexdigest(),
                          managerSha256=hashlib.file_digest(args.manager.open('rb'), 'sha256').hexdigest())
            (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
            for name, source in [('guest.log', files + 'api29-native-rootfs/logs/manager-init.log'), ('screenshot.png', files + 'compatibility.png')]:
                # c.adb decodes text; pull binary screenshots through a temporary VM file.
                if name.endswith('.png'):
                    if owned('if [ -f ' + source + ' ]; then echo present; fi').strip() != 'present':
                        continue
                    screenshot = '/data/local/tmp/pocket-compatibility.png'
                    owned('cp ' + source + ' ' + screenshot + ' && chmod 644 ' + screenshot)
                    adb('pull', screenshot, str(args.out / name))
                    owned('rm -f ' + screenshot)
                else:
                    result = c.adb('-s', c.serial(), 'exec-out', 'su -c ' + shlex.quote('cat ' + source), timeout=40, check=False)
                    (args.out / name).write_text(result.stdout)
            print(json.dumps({k: v for k, v in report.items() if k not in ('activities', 'processes')}, indent=2), flush=True)
            assert report.get('launchValidated'), 'App launch not validated; inspect report, screenshot and guest log'
    finally:
        owned('rm -f ' + temporary)


if __name__ == '__main__':
    main()
