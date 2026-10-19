#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Guarded first-run regression; preserve the prior Pocket guest by rename.

Copy the full APK to /home/pi/aosp-pocket-runtime.apk first. This installs
only our app on the existing VM, uses no external image import, and retains
the old rootfs under rootfs-pre-bundled-TIMESTAMP. Never clears app data.
"""
import json
from pathlib import Path
import re
import shlex
import sys
import time
import xml.etree.ElementTree as ET

PACKAGE = 'dev.kreijstal.aosppocket.runtime'
FILES = '/data/user/0/' + PACKAGE + '/files'

def main():
    sys.path.insert(0, '/home/pi/lineage-cuttlefish/tools')
    import cuttlefish_arm64 as c
    c.require_virtual()
    def adb(*args, timeout=40):
        result = c.adb(*args, check=False, timeout=timeout)
        if result.returncode: raise RuntimeError(result.stdout + result.stderr)
        return result.stdout.strip()
    def shell(command, root=False):
        return adb('-s', c.serial(), 'shell', '-T', 'su -c '+shlex.quote(command) if root else command)
    def exists(path): return shell('if [ -e '+shlex.quote(path)+' ]; then echo yes; fi', True) == 'yes'
    sentinel = FILES+'/rootfs/data/pocket-bundled-upgrade-probe'
    previous = exists(FILES+'/rootfs/init')
    if previous: shell('printf preserved > '+sentinel, True)
    print('Installing full APK; existing guest data will be retained', flush=True)
    assert 'Success' in adb('-s', c.serial(), 'install', '--no-streaming', '-r',
        '/home/pi/aosp-pocket-runtime.apk', timeout=240)
    backup = None
    if previous:
        assert shell('cat '+sentinel, True) == 'preserved'
        backup = FILES+'/rootfs-pre-bundled-'+str(int(time.time()))
        shell('mv '+FILES+'/rootfs '+backup, True)
        print('PASS: upgrade preserved prior image; backup:', backup, flush=True)
    shell('input keyboard -d 0 keyevent KEYCODE_WAKEUP')
    shell('wm dismiss-keyguard')
    assert 'mKeyguardShowing=true' not in shell('dumpsys activity activities'), 'Unlock the VM normally before continuing'
    shell('am start -n '+PACKAGE+'/.MainActivity')
    time.sleep(1)
    host = shell('pidof '+PACKAGE)
    assert host.isdecimal(), host
    def log(): return adb('-s', c.serial(), 'shell', 'logcat -d --pid='+host+' -s PocketRuntime:I')
    def wait_log(text, seconds=30):
        deadline = time.monotonic()+seconds
        while text not in log():
            if time.monotonic() > deadline: raise RuntimeError('Missing state: '+text)
            time.sleep(.25)
    shell('uiautomator dump /sdcard/pocket-first-run.xml')
    tree = ET.fromstring(shell('cat /sdcard/pocket-first-run.xml'))
    buttons = {n.get('text').casefold():n for n in tree.iter('node') if n.get('package') == PACKAGE and n.get('class') == 'android.widget.Button'}
    assert buttons['start'].get('enabled') == buttons['import guest'].get('enabled') == 'true'
    def tap(label):
        left, top, right, bottom = map(int, re.findall(r'\d+', buttons[label].get('bounds')))
        shell(f'input touchscreen -d 0 tap {(left+right)//2} {(top+bottom)//2}')
    tap('start')
    wait_log('Setting up included Android')
    assert 'isForeground=true' in shell('dumpsys activity services '+PACKAGE)
    tap('stop')
    wait_log('Setup cancelled')
    assert not exists(FILES+'/rootfs')
    assert not shell('find '+FILES+' -maxdepth 1 -name "import-*"', True)
    print('PASS: automatic extraction cancelled cleanly; retry available', flush=True)
    tap('start')
    # Closing the manager must not cancel extraction or require another Start.
    tap('close manager')
    time.sleep(1)
    assert shell('pidof '+PACKAGE) == host
    assert 'isForeground=true' in shell('dumpsys activity services '+PACKAGE)
    shell('am start -n '+PACKAGE+'/.GuestActivity')
    marker = FILES+'/rootfs/data/pocket-boot-complete'
    deadline = time.monotonic()+300
    while not exists(marker) or shell('cat '+marker, True) != '1':
        if time.monotonic()>deadline: raise RuntimeError('Bundled Android failed to finish setup and boot')
        time.sleep(2)
    assert shell('pidof '+PACKAGE) == host
    assert not shell('find '+FILES+' -maxdepth 1 -name "import-*"', True)
    assert not exists(FILES+'/incoming.zip')
    if backup: assert shell('cat '+backup+'/data/pocket-bundled-upgrade-probe', True) == 'preserved'
    before = shell('ps -A -o UID,PID,PPID,NAME')
    shell('am start -n '+PACKAGE+'/.GuestActivity')
    time.sleep(1)
    rows = [row for row in before.splitlines() if row.split()[-1] == 'init']
    after = shell('ps -A -o UID,PID,PPID,NAME')
    assert all(row in after.splitlines() for row in rows)
    shell('screencap -p /sdcard/pocket-bundled-launcher.png')
    adb('-s', c.serial(), 'pull', '/sdcard/pocket-bundled-launcher.png', '/home/pi/pocket-bundled-launcher.png')
    report = dict(runtime_version='0.3-bundled', automatic_image_setup=True,
        no_manual_image_import=True, setup_cancellation_and_retry=True,
        setup_survives_manager_close=True, foreground_service=True,
        manual_import_available_before_setup=True, no_staged_default_zip=True,
        boot_completed=True, repeated_open_preserves_guest=True,
        upgrade_preserved_previous_guest=previous, previous_guest_backup=backup,
        host_pid=int(host), screenshot='/home/pi/pocket-bundled-launcher.png')
    Path('/home/pi/pocket-bundled-validation.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2), flush=True)
    print('PASS: full APK, automatic setup, cancellation/retry and default guest boot', flush=True)

if __name__ == '__main__': main()
