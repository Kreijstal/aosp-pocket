#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import argparse,json,subprocess,zipfile
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('apk',type=Path)
parser.add_argument('--ndk',type=Path,required=True)
args=parser.parse_args()
with zipfile.ZipFile(args.apk) as apk:
    names=apk.namelist()
    assert len(names)==len(set(names)),'Duplicate APK entries'
    assert 'lib/arm64-v8a/libproot.so' in names,'API 25 PRoot missing'
    layout=json.loads(apk.read('assets/default-guest/pocket-layout.json'))
    assert layout.get('guest_sdk',25)==25
    props=apk.read('assets/api28-rootfs/system/build.prop.payload').decode()
    assert '\nro.build.version.sdk=28\n' in '\n'+props
    rc=apk.read('assets/api28-rootfs/system/etc/init/init.manager.rc.payload').decode()
    assert 'start-system-server' in rc and 'GuestControl' in rc
    assert 'socket pocket_control stream 0600 1000 1000' in rc
    assert 'service pocket-gatekeeper /system/bin/gatekeeperd /data/misc/gatekeeper' in rc
    assert 'assets/api28-rootfs/system/bin/gatekeeperd.payload' in names
    assert 'FrameworkProbe' not in rc,'Development harness must not run in Manager'
    assert 'assets/api28-rootfs/system/framework/pocket-zygote.jar.payload' in names
    assert 'assets/licenses/android9-NOTICE.xml.gz' in names
    assert not any(n.endswith('.bc') for n in names),'Guest RenderScript must remain opaque to host ABI scan'
    for name in ('libpocket_direct_boot.so','libpocket_binder_api28.so','libpocket_hidl_api28.so'):
        assert 'assets/api28-rootfs/system/lib64/'+name+'.payload' in names
print('PASS: isolated API 25/API 28 image layouts, direct service, licenses and ABI-safe payloads')
