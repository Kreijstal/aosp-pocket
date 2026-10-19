#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Sequential Cuttlefish test: host-sized guest, scaled windows, real app taps.

Stage the development APK, display-test.apk and validate_manager_on_pi.py on
the Pi first. Stop the original guest through its own UI and restore it in
finally; never updates its APK or deletes either guest's data.
"""
import json
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

DEV = 'dev.kreijstal.aosppocket.managerdev'
OLD = 'dev.kreijstal.aosppocket.runtime'
CLASS = 'dev.kreijstal.aosppocket.runtime.'
ROOT = '/data/user/0/' + DEV + '/files/api28-rootfs'

def main():
    sys.path.insert(0, '/home/pi/lineage-cuttlefish/tools')
    import cuttlefish_arm64 as c
    c.require_virtual()
    def adb(*args, timeout=45):
        r = c.adb('-s', c.serial(), *args, check=False, timeout=timeout)
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
        return r.stdout.strip()
    def shell(command):
        return adb('shell', '-T', command)
    def processes():
        return [row.split() for row in shell('ps -A -o UID,PID,PPID,NAME').splitlines()[1:]]
    def host(package):
        return next((row for row in processes() if row[-1] == package), None)
    def children(package):
        h = host(package)
        return [r for r in processes() if h and r[0] == h[0] and r[1] != h[1]]
    def launch(package, activity='MainActivity', extras=''):
        shell('am start -f 0x10000000 -n ' + package + '/' + CLASS + activity + ' ' + extras)
    def hierarchy():
        for _ in range(5):
            path = '/sdcard/pocket-display-ui-' + str(time.time_ns()) + '.xml'
            try:
                shell('timeout 25 uiautomator dump ' + path)
                return ET.fromstring(shell('cat ' + path))
            except (RuntimeError, ET.ParseError):
                time.sleep(1)
        raise RuntimeError('Display test UI unavailable')
    def click(label, package=DEV):
        for _ in range(12):
            matches = [n for n in hierarchy().iter('node') if n.get('package') == package
                       and n.get('enabled') == 'true'
                       and (n.get('text', '').casefold().startswith(label.casefold())
                            or n.get('content-desc', '').casefold().startswith(label.casefold()))]
            # The navigation drawer comes after its obscured toolbar title.
            node = (matches[-1] if package == 'com.android.documentsui' else matches[0]) if matches else None
            if node is not None:
                l, t, r, b = map(int, re.findall(r'\d+', node.get('bounds')))
                if r > l and b > t:
                    shell(f'input tap {(l+r)//2} {(t+b)//2}')
                    time.sleep(1)
                    return
            time.sleep(1)
        raise RuntimeError('Display test control unavailable: ' + label)
    def stop(package):
        if not children(package):
            return
        launch(package)
        click('Stop', package)
        for _ in range(45):
            if not children(package):
                return
            time.sleep(1)
        raise RuntimeError('Guest did not stop cleanly: ' + package)
    def log():
        return shell('tail -n 1000 ' + ROOT + '/logs/manager-init.log')
    def screenshot(name):
        shell('screencap -p /sdcard/' + name + '.png')
        adb('pull', '/sdcard/' + name + '.png', '/home/pi/' + name + '.png')
    was_running = bool(children(OLD))
    original_host = host(OLD)
    result = {'passed': False, 'sequential_guests': True, 'original_was_running': was_running,
              'original_host_pid_before': int(original_host[1]) if original_host else None}
    failure = None
    try:
        stop(OLD)
        assert not children(OLD), 'Original guest is still running; refusing concurrent boot'
        subprocess.run([sys.executable, '-u', '/home/pi/aosp-pocket-validate-manager.py',
                        '--install', '--boot-only', '--original-stopped'], check=True, timeout=660)
        boot = json.loads(Path('/home/pi/pocket-manager9-validation.json').read_text())
        result['boot'] = boot
        geometry = boot['guest_display']
        w, h, dpi = geometry['width'], geometry['height'], geometry['density_dpi']
        assert (w, h) != (480, 800), 'Regression requires a non-480x800 host display'
        baseline_server = boot['system_server_pid']
        adb('push', '/home/pi/pocket-display-test.apk', '/sdcard/Download/pocket-display-test.apk')
        launch(DEV)
        offset = int(shell('stat -c %s ' + ROOT + '/logs/manager-init.log'))
        click('Install APK')
        click('Show roots', 'com.android.documentsui')
        click('Downloads', 'com.android.documentsui')
        click('pocket-display-test.apk', 'com.android.documentsui')
        time.sleep(4)
        click('Open last app')
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            new = shell('tail -c +' + str(offset + 1) + ' ' + ROOT + '/logs/manager-init.log')
            reports = re.findall(r'tag=PocketDisplayTest (\{[^\n]+\})', new)
            if reports and 'tag=PocketDisplayTest button=' in new:
                break
            time.sleep(1)
        else:
            raise RuntimeError('Display app did not report its real geometry')
        def assert_report(report, taps):
            assert (report['width'], report['height'], report['density_dpi']) == (w, h, dpi), report
            assert report['resource_density_dpi'] == dpi and report['guest_uid'] >= 10000, report
            assert report['taps'] == taps, report
        assert_report(json.loads(reports[-1]), 0)
        result['app_initial'] = json.loads(reports[-1])
        viewports = []
        def tap_guest(expected, window):
            text = log()
            x, y = map(int, re.findall(r'tag=PocketDisplayTest button=(\d+),(\d+)', text)[-1])
            surface = next(n for n in hierarchy().iter('node') if n.get('package') == DEV
                           and n.get('class') == 'android.view.SurfaceView')
            l, t, r, b = map(int, re.findall(r'\d+', surface.get('bounds')))
            assert abs((r-l)*h - (b-t)*w) <= max(w, h), (window, surface.attrib)
            viewports.append({'window': window, 'width': r-l, 'height': b-t})
            start = int(shell('stat -c %s ' + ROOT + '/logs/manager-init.log'))
            shell(f'input tap {l+x*(r-l)//w} {t+y*(b-t)//h}')
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                tail = shell('tail -c +' + str(start+1) + ' ' + ROOT + '/logs/manager-init.log')
                reports = re.findall(r'tag=PocketDisplayTest (\{[^\n]+\})', tail)
                if reports:
                    report = json.loads(reports[-1]);assert_report(report, expected)
                    return report
                time.sleep(.3)
            raise RuntimeError('Scaled touch missed the guest button: ' + window)
        tap_guest(1, 'embedded')
        screenshot('pocket-display-embedded')
        launch(DEV, 'GuestActivity')
        tap_guest(2, 'dedicated')
        screenshot('pocket-display-dedicated')
        click('Float')
        launch(DEV)
        tree = hierarchy()
        assert any(n.get('text', '').startswith('Android is open in its own window.') for n in tree.iter('node'))
        assert not any(n.get('package') == DEV and n.get('class') == 'android.view.SurfaceView' for n in tree.iter('node'))
        tasks = shell('dumpsys activity activities')
        assert any(DEV + '.android' in line and 'mode=pinned' in line and 'visible=true' in line
                   for line in tasks.splitlines()), 'PiP did not open'
        screenshot('pocket-display-pip')
        click('Android is open in its own window.')
        click('Close window')
        launch(DEV)
        result['app_final'] = tap_guest(3, 'embedded_after_pip')
        assert any(int(row[1]) == baseline_server for row in children(DEV)), 'Guest restarted during a view change'
        props = shell('cat ' + ROOT + '/system/build.prop')
        for key, value in [('ro.boot.redroid_width', w), ('ro.boot.redroid_height', h), ('ro.sf.lcd_density', dpi)]:
            assert re.search(r'^' + re.escape(key) + '=' + str(value) + '$', props, re.M)
        result.update({'passed': True, 'guest_pixels_constant_across_windows': True,
                       'viewports': viewports, 'guest_taps': 3, 'pip_exclusive': True,
                       'system_server_restarts': 0, 'host_dpi_relayed': True})
    except Exception as error:
        result['error'] = str(error);failure = error
    finally:
        stop(DEV)
        result['development_guest_reaped'] = not children(DEV)
        if was_running:
            launch(OLD, extras='--ei guest_sdk 25 --ez start_guest true')
            marker = '/data/user/0/' + OLD + '/files/rootfs/data/pocket-boot-complete'
            for _ in range(240):
                if any(r[-1] == 'system_server' for r in children(OLD)) and shell('if [ -f ' + marker + ' ]; then cat ' + marker + '; fi') == '1':
                    break
                time.sleep(1)
            else:
                raise RuntimeError('Original API25 guest did not restore')
            result['original_restored_from_existing_data'] = True
            result['original_host_pid_after'] = int(host(OLD)[1])
        Path('/home/pi/pocket-display-validation.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2), flush=True)
    if failure:
        raise failure

if __name__ == '__main__':
    main()
