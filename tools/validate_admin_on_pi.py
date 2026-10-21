#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run on the guarded Pi VM only: install 0.18, verify its owner console and disable it."""
import importlib.util
import json
from pathlib import Path
import re
import shlex
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
import zipfile

sys.path.insert(0, '/home/pi/lineage-cuttlefish/tools')
import cuttlefish_arm64 as c

PKG = 'dev.kreijstal.aosppocket.runtime'
ROOT = '/data/user/0/'+PKG+'/files/api28-rootfs'
APK = Path('/home/pi/aosp-pocket-manager-0.18.apk')
assert c.shell('getprop ro.product.device') == 'vsoc_arm64', 'Pi ARM64 VM required'
assert APK.is_file()
with zipfile.ZipFile(APK) as archive:
    assert 'assets/pocketctl.py' in archive.namelist(), 'Complete admin-capable APK required'


def shell(command, timeout=40):
    return c.shell(command, timeout=timeout)


def su(command):
    return shell('su -c '+shlex.quote(command))


def tree():
    shell('uiautomator dump /sdcard/pocket-admin-test.xml')
    return ET.fromstring(shell('cat /sdcard/pocket-admin-test.xml'))


def click(text):
    for attempt in range(5):
        nodes = list(tree().iter('node'))
        node = next((n for n in nodes if n.get('text', '').casefold() == text.casefold() and n.get('enabled') == 'true'), None)
        if node is not None:
            a, b, d, e = map(int, re.findall(r'\d+', node.get('bounds')))
            shell('input tap %d %d' % ((a+d)//2, (b+e)//2))
            return
        bounds = next(n.get('bounds') for n in nodes if n.get('bounds') and n.get('package') == PKG)
        a, b, d, e = map(int, re.findall(r'\d+', bounds))
        shell('input swipe %d %d %d %d 350' % ((a+d)//2, e-80, (a+d)//2, b+180))
    raise RuntimeError('Missing settings control '+text)


def main():
    print(su('am startservice -n '+PKG+'/.GuestService -a dev.kreijstal.aosppocket.STOP'), flush=True)
    for _ in range(40):
        # Only this Manager's supervisor, never the outer VM or another app.
        rows = shell('ps -A -o PID,ARGS')
        if not any(ROOT+'/system/bin/pocket-init-api28' in row for row in rows.splitlines()):
            break
        time.sleep(1)
    else:
        raise RuntimeError('Guest supervisor did not stop; refusing to update a live guest')
    print(c.adb('-s', c.serial(), 'install', '--no-incremental', '-r', str(APK), timeout=240).stdout, flush=True)
    print(shell('am start -n '+PKG+'/.MainActivity --ez start_guest true'), flush=True)
    time.sleep(2)
    su('am start -n '+PKG+'/.SettingsActivity')
    click('Enable Termux administration')
    for _ in range(20):
        try:
            prefs = ET.fromstring(su('cat /data/user/0/'+PKG+'/shared_prefs/guest-admin.xml'))
            token = next(n.text for n in prefs if n.get('name') == 'key')
            break
        except (ET.ParseError, StopIteration):
            time.sleep(.5)
    else:
        raise RuntimeError('Admin key not created')

    spec = importlib.util.spec_from_file_location('pocketctl', '/home/pi/pocketctl.py')
    client = importlib.util.module_from_spec(spec);spec.loader.exec_module(client)
    c.adb('-s', c.serial(), 'forward', 'tcp:22280', 'tcp:2228')
    config = {'port': 22280, 'token': token}

    def request(op, argv=(), source=None, cwd='/'):
        import io
        out, err = io.BytesIO(), io.BytesIO()
        code = client.request(config, op, argv, cwd=cwd, source=source, stdout=out, stderr=err)
        return code, out.getvalue(), err.getvalue()

    evidence = {'scope': 'physical Pi host, ARM64 outer LineageOS VM, Android 9 Pocket guest', 'version': '0.18', 'checks': []}
    try:
        for attempt in range(120):
            code, output, error = request(1)
            assert code == 0, error
            status = json.loads(output)
            if status['ready']:
                break
            if attempt % 10 == 0:
                print('Waiting for Android 9:', status, flush=True)
            time.sleep(2)
        else:
            raise RuntimeError('Guest did not become ready')
        assert status['sdk'] == 28, status
        print('Android 9 root-console endpoint ready', flush=True)
        code, output, error = request(2, ['/system/bin/sh', '-c',
            'id -u; id -g; getprop ro.build.version.sdk; /system/bin/sh -c "id -u"; grep "^Uid:" /proc/self/status'])
        assert code == 0, (code, output, error)
        lines = output.decode().splitlines()
        assert lines[:4] == ['0', '0', '28', '0'], (lines, error)
        kernel_uid = int(lines[4].split()[1])
        assert kernel_uid >= 10000, lines
        evidence['identity'] = {'guestUid': 0, 'guestGid': 0, 'nestedGuestUid': 0, 'sdk': 28, 'kernelUid': kernel_uid}
        evidence['checks'].append('root shell, nested exec and real unprivileged kernel UID verified')
        print('PASS: guest UID 0, nested UID 0, Android 9; kernel UID', kernel_uid, flush=True)

        code, output, error = request(2, ['/system/bin/pm', 'list', 'packages'])
        assert code == 0 and b'package:android' in output, (code, output, error)
        evidence['checks'].append('guest package-manager Binder access')
        print('PASS: guest package-manager access', flush=True)
        assert request(2, ['/system/bin/pwd'], cwd='/data/local/tmp')[1].strip() == b'/data/local/tmp'

        import io
        data = bytes(range(256))*2048
        destination = '/data/local/tmp/pocket-admin-validation.bin'
        try:
            code, output, error = request(2, ['/system/bin/sh', '-c', 'cat > "$1"', 'pocket-test', destination], io.BytesIO(data))
            assert code == 0, (code, output, error)
            code, output, error = request(2, ['/system/bin/cat', destination])
            assert code == 0 and output == data, (code, len(output), error)
        finally:
            request(2, ['/system/bin/rm', '-f', destination])
        evidence['checks'].append('512 KiB binary upload/download through real Android guest shell')
        print('PASS: binary upload/download', flush=True)

        code, output, error = request(2, ['/system/bin/sh', '-c', 'printf out; printf err >&2; exit 7'])
        assert (code, output, error) == (7, b'out', b'err'), (code, output, error)
        evidence['checks'].append('stdout/stderr separation and guest exit status')

        click('Revoke connection token')
        try:
            client.connect(config)
            raise AssertionError('Old token still authorized')
        except OSError:
            pass
        evidence['checks'].append('settings revocation rejects previous token')
        print('PASS: token revoked through settings', flush=True)
        Path('/home/pi/pocket-admin-validation.json').write_text(json.dumps(evidence, indent=2)+'\n')
        print(json.dumps(evidence), flush=True)
    finally:
        try:
            click('Disable Termux administration')
        finally:
            c.adb('-s', c.serial(), 'forward', '--remove', 'tcp:22280', check=False)
    print('Administration disabled after testing; guest data preserved', flush=True)


if __name__ == '__main__':
    main()
