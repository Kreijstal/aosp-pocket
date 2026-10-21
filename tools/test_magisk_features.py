#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run in paired phone Termux, with Pocket stopped: guest overlays and Zygisk."""
import io
import json
from pathlib import Path
import re
import tarfile
import threading
import time
import test_magisk_pocket as t

D = Path(__file__).resolve().parent
command = t.command
upload = t.upload


def boot(rc):
    command('/system/bin/sh', '-c', '/system/bin/toybox rm -f /.pocket-module-view/ready /dev/__properties__/*')
    upload(io.BytesIO(rc.encode()), '/data/local/tmp/pocket-feature-test.rc', '644')
    state = {}
    def run():
        try:
            state['result'] = command('/system/bin/sh', '-c',
                'exec /system/bin/toybox timeout 240 /system/bin/pocket-init-api28 '
                '/data/local/tmp/pocket-feature-test.rc < /dev/null > /pocket-console 2>&1', check=False)
            captured=command('/system/bin/toybox','cat','/pocket-console',check=False)
            state['result']=(state['result'][0],captured[1],state['result'][2])
        except Exception as error:
            state['error'] = str(error)
    worker = threading.Thread(target=run, daemon=True)
    worker.start()
    return worker, state


def stop(worker, state, name):
    command('/system/bin/setprop', 'pocket.control.stop', '1', check=False)
    worker.join(10)
    if 'result' in state:
        (D/(name+'.log')).write_bytes(state['result'][1]+state['result'][2])
    if worker.is_alive() or 'error' in state:
        raise RuntimeError('Init shutdown failed: '+str(state))


def ready(worker, state, expected=b'ready'):
    for _ in range(250):
        if not worker.is_alive(): raise RuntimeError('Init exited: '+str(state))
        if command('/system/bin/getprop', 'pocket.magisk.postfs')[1].strip() == b'1':
            assert command('/system/bin/getprop', 'pocket.magisk.status')[1].strip() == expected
            return
        time.sleep(.2)
    raise RuntimeError('Magisk boot stage did not finish')


def main():
    status = json.loads(t.request(1)[1])
    assert status['sdk'] == 28 and not status['running'] and not status['starting']
    command('/system/bin/sh', '-c', '/system/bin/toybox rm -f /.pocket-module-view/ready')
    with (D/'drop-uid').open('rb') as stream:
        upload(stream, '/data/local/tmp/pocket-magisk-drop-uid')
    with tarfile.open(D/'payload.tar') as tar:
        for member in tar:
            assert '..' not in Path(member.name).parts and not member.name.startswith('/')
            if member.isfile(): upload(tar.extractfile(member), '/'+member.name)
    module = '/data/adb/modules/pocket_feature_fixture'
    command('/system/bin/toybox', 'rm', '-rf', module)
    upload(io.BytesIO(b'id=pocket_feature_fixture\nname=Pocket feature test\nversion=1\nversionCode=1\nauthor=Pocket\ndescription=Offline test\n'), module+'/module.prop')
    upload(io.BytesIO(b'baseline\n'), '/system/etc/pocket-overlay-base')
    upload(io.BytesIO(b'overlaid\n'), module+'/system/etc/pocket-overlay-base')
    upload(io.BytesIO(b'new-file\n'), module+'/system/etc/pocket-overlay-new')
    with (D/'zygisk-fixture.so').open('rb') as stream:
        upload(stream, module+'/zygisk/arm64-v8a.so')
    # Pure overlay validation first, then include the Zygisk directory for boot 2.
    command('/system/bin/toybox', 'mv', module+'/zygisk', module+'/fixture-library')
    minimal = ('import /system/etc/init/init.magisk.rc\non boot\n'
               '    setprop dalvik.vm.jdwp-provider none\n'
               '    setprop dalvik.vm.heapstartsize 16m\n'
               '    setprop dalvik.vm.heapsize 192m\n'
               '    setprop sys.boot_completed 0\n    setprop pocket.data.ready 1\n')
    worker, state = boot(minimal)
    restore = None
    try:
        ready(worker, state)
        old = command('/sbin/magisk', '--sqlite', "SELECT value FROM settings WHERE key='zygisk';")[1]
        match = re.search(rb'value=(\d+)', old)
        restore = ("INSERT OR REPLACE INTO settings VALUES ('zygisk',"+match[1].decode()+');'
                   if match else "DELETE FROM settings WHERE key='zygisk';")
        assert command('/system/bin/toybox', 'cat', '/system/etc/pocket-overlay-base')[1] == b'overlaid\n'
        assert command('/system/bin/toybox', 'cat', '/system/etc/pocket-overlay-new')[1] == b'new-file\n'
        assert b'pocket-overlay-new' in command('/system/bin/toybox', 'ls', '/system/etc')[1]
        print('PASS: overlay replacement, addition and directory enumeration.', flush=True)
        command('/sbin/magisk', '--sqlite', "INSERT OR REPLACE INTO settings VALUES ('zygisk',1);")
        command('/system/bin/toybox', 'mv', module+'/fixture-library', module+'/zygisk')
    finally:
        stop(worker, state, 'features-first')
    # Start only the actual AOSP zygote, log writer and guest hardware registry.
    # Avoid the full UI's independent RAM/lifecycle problem for this API test.
    base = command('/system/bin/toybox', 'cat', '/system/etc/init/init.manager.rc')[1].decode()
    services = []
    for block in re.split(r'(?=^service )', base, flags=re.M):
        if re.match(r'service pocket-(?:log-writer|registry|zygote) ', block):
            services.append(block.split('\n\n')[0].replace(' start-system-server','')
                            .replace('/system/bin/pocket-hw-services','/system/bin/pocket-api28-probe manager'))
    assert len(services) == 3
    rc = minimal+'\n'+'\n\n'.join(services)+'\n'
    command('/system/bin/toybox', 'rm', '-f', '/data/local/tmp/pocket-zygisk-events', '/data/pocket-zygote-child.json')
    worker, state = boot(rc)
    try:
        ready(worker, state)
        assert command('/system/bin/getprop', 'pocket.zygisk.enabled')[1].strip() == b'1'
        result = command('/data/local/tmp/pocket-magisk-drop-uid', '39999',
                         '/system/bin/pocket-zygote-client-api28', 'fork', '0123456789abcdef')
        print(result[1].decode(), flush=True)
        events = command('/system/bin/toybox', 'cat', '/data/local/tmp/pocket-zygisk-events')[1]
        print(events.decode(), flush=True)
        assert b'onLoad uid=0' in events and b'preApp uid=0 target=39999' in events
        assert b'postApp uid=39999' in events
        assert b'jniHook uid=0' in events
        child = re.search(rb'child_pid=(\d+)',result[1])
        if child:
            pid = child[1].decode()
            process = command('/system/bin/toybox','cat','/proc/'+pid+'/cmdline',check=False)[1]
            if b'pocket-api28-child' in process:
                command('/system/bin/toybox','kill','-TERM',pid,check=False)
        print('PASS: real Zygisk module onLoad/preApp/postApp across guest specialization.', flush=True)
    finally:
        if restore: command('/sbin/magisk', '--sqlite', restore, check=False)
        command('/system/bin/toybox', 'touch', module+'/disable', check=False)
        stop(worker, state, 'features-zygisk')
    worker, state = boot(minimal)
    try:
        ready(worker, state)
        assert command('/system/bin/toybox', 'cat', '/system/etc/pocket-overlay-base')[1] == b'baseline\n'
        assert command('/system/bin/toybox', 'ls', '/system/etc/pocket-overlay-new', check=False)[0] != 0
        print('PASS: disabling module restores the base image on restart.', flush=True)
    finally:
        stop(worker, state, 'features-disabled')
    command('/system/bin/toybox', 'rm', '-rf', module)
    command('/system/bin/toybox', 'rm', '-f', '/system/etc/pocket-overlay-base')
    # Leave the optional test runtime inactive: the distribution only supports
    # it, and must work without an installed Magisk core or Manager APK.
    command('/system/bin/toybox','mv','/data/adb/magisk/magisk','/data/adb/magisk/magisk.test-only')
    worker,state=boot(minimal)
    try:
        ready(worker,state,b'absent')
        assert command('/system/bin/toybox','ls','/sbin/magisk',check=False)[0] != 0
        print('PASS: absent optional runtime does not block guest init or expose stale Magisk CLI.',flush=True)
    finally:
        stop(worker,state,'features-no-magisk')
    command('/system/bin/toybox','rm','-rf','/.pocket-module-view')
    (D/'features-validation.json').write_text(json.dumps({
        'guestApi':28, 'hostRootRequired':False, 'overlayOpenAndDirectoryEnumeration':'PASS',
        'moduleDisableRestoresBaseline':'PASS', 'zygiskOnLoadPrePostAppSpecialize':'PASS',
        'zygiskPublicJniHook':'PASS', 'bootWithoutMagiskRuntime':'PASS',
        'scope':'actual guest zygote with test module; full UI and third-party modules pending'
    }, indent=2)+'\n')


if __name__ == '__main__': main()
