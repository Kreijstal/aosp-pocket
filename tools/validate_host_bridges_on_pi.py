#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the development Manager through its UI in the guarded Cuttlefish VM.

Granting permissions is test setup. Broker and guest operations run as the
ordinary host app UID. Never places a call or sends a message.
"""
import argparse
import json
import re
import shlex
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

PACKAGE = 'dev.kreijstal.aosppocket.managerdev'
ROOT = '/data/user/0/' + PACKAGE + '/files/api28-rootfs'

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--network-app', action='store_true')
    parser.add_argument('--app-only', action='store_true', help='Resume app tests from the saved, successful core checkpoint')
    parser.add_argument('--phone-update', action='store_true',
                        help='Install the rebuilt guest phone delegate through Manager before probing')
    parser.add_argument('--original-stopped', action='store_true',
                        help='Sequential testing after an already stopped API25 guest')
    args = parser.parse_args()
    if args.app_only and not args.network_app:
        parser.error('--app-only requires --network-app')
    sys.path.insert(0, '/home/pi/lineage-cuttlefish/tools')
    import cuttlefish_arm64 as c
    c.require_virtual()
    def adb(*arguments, timeout=60):
        r = c.adb('-s', c.serial(), *arguments, check=False, timeout=timeout)
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
        return r.stdout.strip()
    def shell(command):
        return adb('shell', '-T', command)
    def original():
        rows = [r.split() for r in shell('ps -A -o UID,PID,PPID,NAME').splitlines()[1:]]
        host = next((r for r in rows if r[-1] == 'dev.kreijstal.aosppocket.runtime'), None)
        if host is None:
            return None
        proot = next(r for r in rows if r[2] == host[1] and r[-1] == 'libproot.so')
        init = next(r for r in rows if r[2] == proot[1] and r[-1] == 'init')
        return [host, proot, init]
    before = original()
    assert (before is None) == args.original_stopped, 'Original API25 presence differs from requested mode'
    def hierarchy():
        for _ in range(5):
            path = '/sdcard/pocket-host-ui-' + str(time.time_ns()) + '.xml'
            try:
                shell('timeout 30 uiautomator dump ' + path)
                return ET.fromstring(shell('cat ' + path))
            except (RuntimeError, ET.ParseError):
                time.sleep(1)
        raise RuntimeError('Could not read UI hierarchy')
    def click(label, package=PACKAGE):
        for attempt in range(6):
            tree = hierarchy()
            matches = [n for n in tree.iter('node') if n.get('package') == package
                         and n.get('enabled') == 'true'
                         and (n.get('text', '').casefold().startswith(label.casefold())
                              or n.get('content-desc', '').casefold().startswith(label.casefold()))]
            # DocumentsUI also reports the obscured toolbar title behind its
            # navigation drawer. Prefer the drawer entry, appended last.
            node = (matches[-1] if package == 'com.android.documentsui' else matches[0]) if matches else None
            if node is not None:
                l, t, r, b = map(int, re.findall(r'\d+', node.get('bounds')))
                if r > l and b > t:
                    shell(f'input touchscreen -d 0 tap {(l+r)//2} {(t+b)//2}')
                    time.sleep(1)
                    return
            if package==PACKAGE and label in ('Start','Stop','Install APK','Open last app','Close manager','Host peripherals','Import guest'):
                settings=next((n for n in tree.iter('node') if n.get('package')==PACKAGE and n.get('text','').casefold()=='settings' and n.get('clickable')=='true'),None)
                if settings is not None:
                    l,t,r,b=map(int,re.findall(r'\d+',settings.get('bounds')))
                    shell(f'input tap {(l+r)//2} {(t+b)//2}')
                    continue
            if attempt == 0 and package == PACKAGE:
                # Previous probes can leave this ScrollView below the target.
                for _ in range(3):
                    shell('input touchscreen -d 0 swipe 350 350 350 1050 250')
                continue
            shell('input touchscreen -d 0 swipe 350 1050 350 350 350')
        raise RuntimeError('UI control not found: ' + label)
    def peripherals():
        shell('am start -f 0x10000000 -n ' + PACKAGE + '/dev.kreijstal.aosppocket.runtime.MainActivity')
        click('Host peripherals')
    def install_guest(source, filename):
        adb('push', source, '/sdcard/Download/' + filename)
        shell('am start -f 0x10000000 -n ' + PACKAGE + '/dev.kreijstal.aosppocket.runtime.MainActivity')
        click('Install APK')
        for _ in range(3):
            click('Show roots', 'com.android.documentsui')
            click('Downloads', 'com.android.documentsui')
            if any(n.get('text') == filename for n in hierarchy().iter('node')):
                break
        else:
            raise RuntimeError('Downloads did not display the test APK: ' + filename)
        click(filename, 'com.android.documentsui')
        time.sleep(4)
        # On replacement, Android can still stop the auto-launched activity
        # after the install callback. Exercise the explicit reopen action.
        if filename != 'pocket-host-phone.apk':
            click('Open last app')
    def read_report(name):
        output = shell('cat ' + ROOT + '/logs/' + name + '.json 2>/dev/null || true')
        try:
            return json.loads(output)
        except ValueError:
            return {}
    def probe(label, name, timeout=90):
        old = read_report(name).get('guest_elapsed_ms')
        click(label)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            report = read_report(name)
            if report.get('guest_elapsed_ms') is not None and report['guest_elapsed_ms'] != old:
                print('Completed ' + name + ' at ' + str(report['guest_elapsed_ms']), flush=True)
                return report
            time.sleep(1)
        raise RuntimeError('No fresh probe report: ' + name)
    def wait_host_app(names):
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            activities = shell('dumpsys activity activities')
            resumed = [line.strip() for line in activities.splitlines() if 'topResumedActivity' in line]
            if any(any(name in line.lower() for name in names) for line in resumed):
                return resumed
            time.sleep(2)
        raise AssertionError('Host app did not open: ' + repr(resumed))
    if args.phone_update:
        install_guest('/home/pi/pocket-host-phone.apk', 'pocket-host-phone.apk')
        wait_host_app(['dialer'])
    if args.app_only:
        result = json.loads(Path('/home/pi/pocket-host-bridges-validation.json').read_text())
        assert result['network']['http_status'] == 200 and result['network']['guest_network_connected']
        assert result['repeat_import_idempotent'] and result['host_dialer_resumed']
        assert result['contacts']['matched_host_rows'] == result['contacts']['host_rows']
        result['resumed_from_core_checkpoint'] = True
    else:
        peripherals()
        denied = probe('Test camera, microphone and data bridges', 'host-device-check')
        if not denied['host']['permissions']['android.permission.CAMERA']:
            Path('/home/pi/pocket-host-bridges-denied.json').write_text(json.dumps(denied, indent=2) + '\n')
        permissions = denied['host']['permissions']
        for section, permission in [('camera', 'CAMERA'), ('audio', 'RECORD_AUDIO'),
                                    ('contacts', 'READ_CONTACTS'), ('phone', 'READ_PHONE_STATE')]:
            if not permissions['android.permission.' + permission]:
                assert '403' in denied[section].get('reason', ''), denied[section]
        # Administrative VM test setup, not a privilege used by the bridge itself.
        for permission in ['CAMERA', 'RECORD_AUDIO', 'READ_CONTACTS', 'READ_PHONE_STATE',
                           'ACCESS_COARSE_LOCATION', 'ACCESS_FINE_LOCATION']:
            shell('pm grant ' + PACKAGE + ' android.permission.' + permission)
        granted = probe('Test camera, microphone and data bridges', 'host-device-check')
        print('Granted device probe: ' + json.dumps(granted), flush=True)
        if granted['host']['camera_count']:
            camera = granted['camera']
            assert camera['status'] == 'ok' and camera['format'] == 35 and camera['timestamp_ns'] > 0, camera
            end = 0
            for plane in camera['planes']:
                assert plane['offset'] == end and plane['length'] > 0, camera
                assert plane['row_stride'] > 0 and plane['pixel_stride'] > 0, camera
                end += plane['length']
            assert len(camera['planes']) == 3 and end == camera['bytes'], camera
        audio = granted['audio']
        assert audio['status'] == 'ok' and audio['frames'] == audio['playback_frames'] == 12000, audio
        assert audio['sample_rate'] == 48000, audio
        assert granted['sensors']['status'] == 'ok' and granted['sensors']['samples'] > 0, granted['sensors']
        assert granted['phone']['status'] == 'ok', granted['phone']
        assert granted['contacts']['status'] == 'ok', granted['contacts']
        click('Import host contacts into guest')
        time.sleep(3)
        imported = probe('Test camera, microphone and data bridges', 'host-device-check')
        contacts = imported['contacts']
        assert contacts['status'] == 'ok', contacts
        assert contacts['guest_imported_contacts'] == contacts['host_contacts'], contacts
        assert contacts['matched_host_rows'] == contacts['host_rows'], contacts
        click('Import host contacts into guest')
        time.sleep(3)
        repeated = probe('Test camera, microphone and data bridges', 'host-device-check')
        assert repeated['contacts'] == contacts, repeated['contacts']
        network = probe('Test guest network', 'host-bridge-check', 45)
        assert network['guest_network_connected'] and network['http_status'] == 200, network
        click('Open host dialer from guest')
        resumed = wait_host_app(['dialer'])
        result = {'scope': 'Cuttlefish host APIs, not physical Samsung hardware',
                  'initial_device_probe': denied, 'devices': granted, 'contacts': contacts,
                  'repeat_import_idempotent': True, 'network': network,
                  'host_dialer_resumed': resumed, 'call_placed': False,
                  'original_api25_unchanged': before == original() if before else None,
                  'original_api25_running_during_test': before is not None}
        assert before == original()
        Path('/home/pi/pocket-host-bridges-validation.json').write_text(json.dumps(result, indent=2) + '\n')
    if args.network_app:
        offset = int(shell('stat -c %s ' + ROOT + '/logs/manager-init.log'))
        install_guest('/home/pi/pocket-network-test.apk', 'pocket-network-test.apk')
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            log = shell('tail -c +' + str(offset + 1) + ' ' + ROOT + '/logs/manager-init.log')
            matches = re.findall(r'tag=PocketHostNetworkTest (\{[^\n]+\})', log)
            if matches:
                app = json.loads(matches[-1])
                rows = [row.split() for row in shell('ps -A -o UID,PID,PPID,NAME').splitlines()[1:]]
                host_uid = int(next(row[0] for row in rows if row[-1] == PACKAGE))
                assert app['passed'] and host_uid >= 10000 and app['kernel_uid'] == host_uid, app
                result['ordinary_guest_app'] = app
                break
            time.sleep(1)
        else:
            raise RuntimeError('Ordinary guest network test did not report')
        matches = re.findall(r'tag=PocketHostSmsTest center=(\d+),(\d+)', log)
        assert matches, 'SMS test button was not laid out'
        x, y = map(int, matches[-1])
        surface = next(n for n in hierarchy().iter('node') if n.get('package') == PACKAGE
                       and n.get('class') == 'android.view.SurfaceView')
        l, t, r, b = map(int, re.findall(r'\d+', surface.get('bounds')))
        dimensions=re.findall(r'POCKET_CONTROL_READY sdk=28 width=(\d+) height=(\d+)',
                             shell('grep POCKET_CONTROL_READY '+ROOT+'/logs/manager-init.log'))[-1]
        guest_width,guest_height=map(int,dimensions)
        shell(f'input touchscreen -d 0 tap {l+x*(r-l)//guest_width} {t+y*(b-t)//guest_height}')
        resumed = wait_host_app(['messaging', 'mms'])
        result['host_sms_composer_resumed'] = resumed
        result['sms_sent'] = False
    result['passed'] = True
    Path('/home/pi/pocket-host-bridges-validation.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)

if __name__ == '__main__':
    main()
