#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the manager, separate guest task, PiP and shared guest lifetime.

Run on the guarded Pi after installing the runtime and starting MainActivity
with --ez start_guest true. Only our runtime and bundled guest-test APK are used.
"""
import json
from pathlib import Path
import re
import shlex
import sys
import time
import xml.etree.ElementTree as ET

PACKAGE = 'dev.kreijstal.aosppocket.runtime'
TEST = 'dev.kreijstal.pocketguesttest'

def main():
    sys.path.insert(0, '/home/pi/lineage-cuttlefish/tools')
    import cuttlefish_arm64 as c
    c.require_virtual()
    def adb(*args, timeout=40):
        result = c.adb(*args, check=False, timeout=timeout)
        if result.returncode: raise RuntimeError(result.stdout + result.stderr)
        return result.stdout.strip()
    def outer(command, root=False):
        return adb('-s', c.serial(), 'shell', '-T', 'su -c ' + shlex.quote(command) if root else command)
    def processes():
        return [p.split(None, 3) for p in outer('ps -A -o UID,PID,PPID,NAME').splitlines()[1:]]
    def ids():
        rows = processes()
        host = next((p for p in rows if len(p) == 4 and p[3] == PACKAGE), None)
        if host is None: return None, None
        init = next((p for p in rows if len(p) == 4 and p[0] == host[0] and p[3] == 'init'), None)
        return host, init
    marker = '/data/user/0/' + PACKAGE + '/files/rootfs/data/pocket-boot-complete'
    deadline = time.monotonic() + 240
    while True:
        host, init = ids()
        if init and outer('if [ -f '+marker+' ]; then cat '+marker+'; else echo 0; fi', True) == '1': break
        if time.monotonic() > deadline: raise RuntimeError('Guest did not boot')
        time.sleep(2)
    original = (host, init)
    def unchanged():
        assert ids() == original, 'Window operation replaced or stopped guest processes'
    def hierarchy():
        outer('uiautomator dump /sdcard/pocket-windows.xml')
        return ET.fromstring(outer('cat /sdcard/pocket-windows.xml'))
    def bounds(node): return tuple(map(int, re.findall(r'\d+', node.get('bounds'))))
    def click(label):
        nodes = hierarchy().iter('node')
        button = next(n for n in nodes if n.get('package') == PACKAGE and
                      n.get('text', '').casefold().startswith(label.casefold()) and n.get('enabled') == 'true')
        left, top, right, bottom = bounds(button)
        outer(f'input touchscreen -d 0 tap {(left+right)//2} {(top+bottom)//2}')
        time.sleep(1)
    def launch(activity):
        outer('am start -f 0x10000000 -n '+PACKAGE+'/.'+activity)
        time.sleep(1)
    screenshots = []
    def screenshot(name):
        assert any(n.get('package') == PACKAGE for n in hierarchy().iter('node')), 'Our UI must be foreground'
        outer('screencap -p /sdcard/pocket-'+name+'.png')
        path = '/home/pi/pocket-'+name+'.png'
        adb('-s', c.serial(), 'pull', '/sdcard/pocket-'+name+'.png', path)
        screenshots.append(path)
    launchers = outer('cmd package query-activities --brief -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -p '+PACKAGE)
    assert PACKAGE+'/.MainActivity' in launchers and PACKAGE+'/.GuestActivity' in launchers, launchers
    services = outer('dumpsys activity services '+PACKAGE)
    assert 'isForeground=true' in services, services
    print('PASS: two launcher activities and foreground runtime service', flush=True)
    port = adb('-s', c.serial(), 'forward', 'tcp:0', 'tcp:5559')
    assert port.isdecimal(), port
    serial = '127.0.0.1:'+port
    try:
        print(adb('connect', serial), flush=True)
        def guest(command): return adb('-s', serial, 'shell', command)
        assert guest('getprop ro.build.version.release') == '7.1.1'
        assert guest('getprop sys.boot_completed') == '1'
        installed = adb('-s', serial, 'install', '--no-streaming', '-r', '/home/pi/pocket-guest-test.apk', timeout=60)
        assert 'Success' in installed, installed
        launch('MainActivity')
        guest('am start -n '+TEST+'/.MainActivity')
        size = tuple(map(int, re.findall(r'(\d+)x(\d+)', guest('wm size'))[-1]))
        def guest_counter():
            guest('uiautomator dump /data/local/tmp/pocket-windows.xml')
            tree = ET.fromstring(guest('cat /data/local/tmp/pocket-windows.xml'))
            button = next(n for n in tree.iter('node') if n.get('package') == TEST and
                          n.get('text', '').casefold().startswith('guest taps:'))
            return int(button.get('text').split(':')[-1]), bounds(button)
        def tap_guest():
            count, (x1,y1,x2,y2) = guest_counter()
            surface = next(n for n in hierarchy().iter('node') if n.get('package') == PACKAGE and
                           n.get('class') == 'android.view.SurfaceView')
            left, top, right, bottom = bounds(surface)
            # Assert letterboxing, and inject into the host view, never guest ADB.
            assert abs((right-left)/(bottom-top) - size[0]/size[1]) < .015
            x = left + (x1+x2)*(right-left)//(2*size[0])
            y = top + (y1+y2)*(bottom-top)//(2*size[1])
            outer(f'input touchscreen -d 0 tap {x} {y}')
            after, _ = guest_counter()
            assert after == count+1, (count, after)
            unchanged()
        tap_guest()
        screenshot('manager-preview')
        click('Open Android')
        tap_guest()
        screenshot('android-window')
        recents = outer('dumpsys activity recents')
        tasks = {}
        for line in recents.splitlines():
            match = re.search(r'Task\{[^\n]*#(\d+)[^\n]*A=\d+:'+re.escape(PACKAGE)+r'\.(manager|android)\b', line)
            # Recents is newest first; older retained tasks from previous APK
            # installs must not overwrite the currently exercised task IDs.
            if match: tasks.setdefault(match[2], int(match[1]))
        assert len(tasks) == 2 and tasks['manager'] != tasks['android'], recents
        print('PASS: separate task IDs, preview and full-window touch, unchanged guest PID', tasks, flush=True)
        launch('MainActivity')
        click('Close manager')
        unchanged()
        launch('GuestActivity')
        unchanged()
        # Callback comes from Android's real PiP transition; use fresh log lines.
        def pip_log(): return adb('-s', c.serial(), 'shell', 'logcat -d --pid='+host[1]+' -s PocketWindows:I')
        before = pip_log()
        click('Float')
        deadline = time.monotonic()+15
        while 'Guest PiP=true' not in pip_log()[len(before):]:
            if time.monotonic() > deadline: raise RuntimeError('No real PiP entry callback')
            time.sleep(1)
        shell = outer('dumpsys activity service com.android.systemui/.SystemUIService WMShell')
        pip_bounds = tuple(map(int, re.search(r'PipBoundsState\n\s+mBounds=Rect\((\d+), (\d+) - (\d+), (\d+)\)', shell).groups()))
        host_size = tuple(map(int, re.findall(r'(\d+)x(\d+)', outer('wm size'))[-1]))
        left, top, right, bottom = pip_bounds
        assert 0 <= left < right <= host_size[0] and 0 <= top < bottom <= host_size[1], \
            ('PiP is outside the host display; inspect host SystemUI display cache', pip_bounds, host_size)
        unchanged()
        launch('MainActivity')
        tap_guest()
        screenshot('floating-android')
        before = pip_log()
        launch('GuestActivity')
        deadline = time.monotonic()+15
        while 'Guest PiP=false' not in pip_log()[len(before):]:
            if time.monotonic() > deadline: raise RuntimeError('Launcher did not restore PiP to full screen')
            time.sleep(1)
        tap_guest()
        click('Close window')
        unchanged()
        launch('MainActivity')
        click('Close manager')
        unchanged()
        assert guest('getprop sys.boot_completed') == '1'
        # A new guest Binder client must work with both UI tasks gone.
        # appOp=0 in its bootstrap broadcast previously required foreground
        # location access and silently dropped these internal handoffs.
        assert 'found' in guest('service check activity')
        launch('GuestActivity')
        tap_guest()
        screenshot('android-restored')
        report = dict(two_launcher_entries=True, distinct_tasks=tasks,
            preview_touch=True, fullscreen_touch=True, aspect_preserved=True,
            picture_in_picture=True, pip_restore_from_launcher=True,
            pip_visible_bounds=list(pip_bounds), host_resolution=list(host_size),
            manager_close_preserves_guest=True, guest_window_close_preserves_guest=True,
            all_windows_closed_preserves_guest=True, unchanged_host_pid=int(host[1]),
            background_guest_binder_client=True,
            unchanged_guest_init_pid=int(init[1]), guest_resolution=list(size),
            foreground_service=True, screenshots=screenshots)
        Path('/home/pi/pocket-windows-validation.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report, indent=2), flush=True)
        print('PASS: manager, independent Android task, PiP, scaled touch and shared guest lifetime', flush=True)
    finally:
        adb('disconnect', serial)
        adb('-s', c.serial(), 'forward', '--remove', 'tcp:'+port)

if __name__ == '__main__': main()
