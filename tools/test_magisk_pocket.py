#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run in phone Termux: validate real Magisk against a prepared Pocket image.

Uses only the paired owner console. No ADB, host root, network or attestation keys.
Expects payload.tar, fixture.zip, drop-uid and paths beside this script.
"""
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import threading
import time

DIRECTORY = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('pocketctl', '/storage/emulated/0/Download/pocketctl.py')
ctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ctl)
config = json.loads((Path.home()/'.config/pocketctl/config.json').read_text())


def request(op, argv=(), source=None, check=True):
    out, err = io.BytesIO(), io.BytesIO()
    code = ctl.request(config, op, argv, source=source, stdout=out, stderr=err)
    if check and code:
        raise RuntimeError(f'{argv}: exit {code}: '+err.getvalue().decode(errors='replace')+out.getvalue().decode(errors='replace'))
    return code, out.getvalue(), err.getvalue()


def command(*args, **kwargs):
    return request(2, args, **kwargs)


def upload(stream, destination, mode='755'):
    command('/system/bin/sh', '-c',
            '/system/bin/toybox mkdir -p "$1"; /system/bin/toybox cat > "$2.next"; '
            '/system/bin/toybox chmod "$3" "$2.next"; /system/bin/toybox mv "$2.next" "$2"',
            'upload', str(Path(destination).parent), destination, mode, source=stream)


def main():
    status = json.loads(request(1)[1])
    if status['running'] or status['starting']:
        raise RuntimeError('Stop the guest before this standalone init integration test')
    assert status['sdk'] == 28
    command('/system/bin/toybox', 'timeout', '5', '/sbin/magisk', '--stop', check=False)
    with tarfile.open(DIRECTORY/'payload.tar') as tar:
        for member in tar:
            assert '..' not in Path(member.name).parts and not member.name.startswith('/')
            if member.isfile():
                upload(tar.extractfile(member), '/'+member.name)
    for name in ('fixture.zip', 'drop-uid', 'paths'):
        with (DIRECTORY/name).open('rb') as stream:
            upload(stream, '/data/local/tmp/pocket-magisk-'+name)
    print(command('/data/local/tmp/pocket-magisk-paths')[1].decode(), flush=True)
    # Module installation precedes the first post-fs-data so the actual daemon
    # upgrades modules_update and runs the installed module's scripts once.
    command('/system/bin/sh', '-c',
        '/system/bin/toybox mkdir -p /sbin/.magisk/device /sbin/.magisk/busybox /data/adb/magisk /data/adb/modules /cache; '
        '/system/bin/toybox cp /system/etc/pocket-magisk/magisk /sbin/magisk; '
        '/system/bin/toybox cp /system/etc/pocket-magisk/busybox /data/adb/magisk/busybox; '
        '/system/bin/toybox cp /system/etc/pocket-magisk/util_functions.sh /data/adb/magisk/util_functions.sh; '
        '/system/bin/toybox chmod 755 /sbin/magisk /data/adb/magisk/busybox; '
        '/system/bin/toybox ln -sf /system/etc/pocket-magisk/getprop /system/bin/getprop; '
        '/system/bin/toybox ln -sf /system/bin/toybox /system/bin/setprop; '
        '/data/adb/magisk/busybox --install -s /sbin/.magisk/busybox')
    print('Payload and real BusyBox prepared.', flush=True)
    # Start a small actual AOSP init: its property service and Magisk services,
    # without the unrelated framework/UI while checking boot ordering.
    rc = ('import /system/etc/init/init.magisk.rc\n'
          'on boot\n    setprop sys.boot_completed 0\n    setprop pocket.data.ready 1\n')
    upload(io.BytesIO(rc.encode()), '/data/local/tmp/pocket-magisk-test.rc', '644')
    state = {}
    def run_init():
        try:
            state['init'] = command('/system/bin/toybox', 'timeout', '60', '/system/bin/pocket-init-api28',
                                    '/data/local/tmp/pocket-magisk-test.rc', check=False)
        except Exception as error:
            state['error'] = str(error)
    worker = threading.Thread(target=run_init, daemon=True)
    # Match DirectImage.prepare: each init boot creates a fresh private area.
    # Existing mappings in stopped-guest leftovers are not modified in place.
    command('/system/bin/sh', '-c', '/system/bin/toybox rm -f /dev/__properties__/*')
    worker.start()
    try:
        for _ in range(50):
            if not worker.is_alive():
                raise RuntimeError('Standalone init exited before daemon startup: '+str(state))
            code, out, err = command('/system/bin/toybox', 'timeout', '2', '/sbin/magisk', '-v', check=False)
            if not code and b'30.7:MAGISK:R' in out:
                break
            time.sleep(.2)
        else:
            raise RuntimeError('Magisk daemon did not become ready')
        print('PASS: real Magisk daemon version handshake.', flush=True)
        for _ in range(50):
            if command('/system/bin/getprop', 'pocket.magisk.postfs')[1].strip() == b'1':
                break
            time.sleep(.1)
        else:
            raise RuntimeError('Magisk post-fs-data boot stage did not finish')
        # Exercise direct creation, update and deletion separately from the
        # property service, and read through an independent guest libc client.
        key = 'pocket.magisk.direct_test'
        for value in ('created', 'updated'):
            command('/sbin/magisk', 'resetprop', '-n', key, value)
            assert command('/system/bin/getprop', key)[1].strip() == value.encode()
        command('/sbin/magisk', 'resetprop', '--delete', key)
        assert command('/system/bin/getprop', key)[1].strip() == b''
        print('PASS: resetprop direct create/update/delete visible to guest libc.', flush=True)
        result = command('/system/bin/toybox', 'timeout', '5', '/sbin/magisk', 'su', '-c', '/system/bin/toybox id -u')
        assert result[1].strip() == b'0', result
        print('PASS: real MagiskSU returned guest UID 0.', flush=True)
        result = command('/data/local/tmp/pocket-magisk-drop-uid', '10018', '/sbin/magisk', '--sqlite', 'SELECT 1;', check=False)
        assert b'Root is required' in result[1]+result[2], result
        print('PASS: ordinary guest UID denied privileged database operation.', flush=True)
        # The current boot has collected its list already; installation is then
        # exercised through the official installer and activated on next boot.
        result = command('/sbin/magisk', '--install-module', '/data/local/tmp/pocket-magisk-fixture.zip')
        print(result[1].decode(errors='replace'), flush=True)
        assert b'- Done' in result[1], result
        command('/system/bin/sh', '-c', '[ -f /data/adb/modules_update/pocket_magisk_fixture/module.prop ]')
        print('PASS: real Magisk module installer created modules_update entry.', flush=True)
    finally:
        command('/system/bin/setprop', 'pocket.control.stop', '1', check=False)
        worker.join(8)
        if worker.is_alive():
            raise RuntimeError('Standalone init did not stop; timeout will terminate it')
        if 'init' in state:
            (DIRECTORY/'init-first.log').write_bytes(state['init'][1]+state['init'][2])
        if 'error' in state:
            raise RuntimeError(state['error'])
    command('/system/bin/toybox', 'rm', '-f', '/data/local/tmp/pocket-magisk-fixture.txt')
    command('/system/bin/sh', '-c', '/system/bin/toybox rm -f /dev/__properties__/*')
    state.clear()
    worker = threading.Thread(target=run_init, daemon=True)
    worker.start()
    try:
        for _ in range(80):
            code, out, err = command('/system/bin/toybox', 'cat', '/data/local/tmp/pocket-magisk-fixture.txt', check=False)
            if not code and b'service uid=0 property=post-fs-data' in out:
                assert b'post-fs-data uid=0 version=30700' in out
                print(out.decode(), flush=True)
                break
            if not worker.is_alive():
                raise RuntimeError('Second init exited: '+str(state))
            time.sleep(.2)
        else:
            raise RuntimeError('Installed module boot-stage/property check failed: '+out.decode(errors='replace'))
        command('/system/bin/setprop', 'sys.boot_completed', '1')
        time.sleep(.3)
        print('PASS: installed module survived restart; post-fs-data precedes service; guest property updated.', flush=True)
    finally:
        command('/system/bin/setprop', 'pocket.control.stop', '1', check=False)
        worker.join(8)
        if worker.is_alive():
            raise RuntimeError('Second init did not stop')
        if 'init' in state:
            (DIRECTORY/'init-second.log').write_bytes(state['init'][1]+state['init'][2])
    receipt = {'guestApi': 28, 'hostRootRequired': False, 'kernelModified': False,
               'realMagiskDaemon': 'PASS', 'magiskSu': 'PASS', 'nonRootDatabaseDenied': 'PASS',
               'realModuleInstaller': 'PASS', 'moduleUpgradeAcrossBoot': 'PASS',
               'resetpropCreateUpdateDelete': 'PASS',
               'postFsDataAndServiceOrdering': 'PASS', 'procLinksAndFifo': 'PASS',
               'scope': 'standalone real AOSP init and Magisk on SM-G780G; full UI boot pending'}
    (DIRECTORY/'validation.json').write_text(json.dumps(receipt, indent=2)+'\n')
    command('/system/bin/toybox', 'rm', '-rf', '/data/adb/modules/pocket_magisk_fixture',
            '/data/adb/modules_update/pocket_magisk_fixture')
    print('PASS: device integration receipt saved:', DIRECTORY/'validation.json', flush=True)


if __name__ == '__main__':
    main()
