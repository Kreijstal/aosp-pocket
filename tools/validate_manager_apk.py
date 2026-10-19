#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import argparse,io,json,struct,subprocess,zipfile
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('apk',type=Path)
parser.add_argument('--ndk',type=Path,required=True)
args=parser.parse_args()
with zipfile.ZipFile(args.apk) as apk:
    names=apk.namelist()
    assert len(names)==len(set(names)),'Duplicate APK entries'
    host_manifest=apk.read('AndroidManifest.xml');offset=8;host_resources=[];back_flags=[]
    while offset<len(host_manifest):
        kind,header,size=struct.unpack_from('<HHI',host_manifest,offset)
        assert size>=header>=8 and offset+size<=len(host_manifest),'Malformed host manifest'
        if kind==0x180:
            host_resources=list(struct.unpack_from('<'+'I'*((size-header)//4),host_manifest,offset+header))
        elif kind==0x102:
            start,stride,count=struct.unpack_from('<HHH',host_manifest,offset+24)
            for index in range(count):
                attribute=offset+16+start+stride*index
                name=struct.unpack_from('<I',host_manifest,attribute+4)[0]
                if name<len(host_resources) and host_resources[name]==0x0101066c:
                    assert host_manifest[attribute+15]==0x12,'Back opt-in must be a boolean'
                    back_flags.append(struct.unpack_from('<I',host_manifest,attribute+16)[0]!=0)
        offset+=size
    assert back_flags==[True],'Guest viewer platform Back callback opt-in missing'
    assert 'lib/arm64-v8a/libproot.so' in names,'API 25 PRoot missing'
    assert 'lib/arm64-v8a/libpocket_frames.so' in names,'Host anonymous display mapper missing'
    assert 'assets/api28-rootfs/system/lib64/libpocket_frames.so.payload' in names,'Guest anonymous display mapper missing'
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
    for name in ('ContactsProvider','Contacts'):
        assert 'assets/api28-rootfs/system/priv-app/'+name+'/'+name+'.apk.payload' in names
    phone='assets/api28-rootfs/system/app/PocketHostPhone/PocketHostPhone.apk.payload'
    assert phone in names
    with zipfile.ZipFile(io.BytesIO(apk.read(phone))) as delegate:
        manifest=delegate.read('AndroidManifest.xml')
    offset=8;resources=[];schemes=[]
    while offset<len(manifest):
        kind,header,size=struct.unpack_from('<HHI',manifest,offset)
        assert size>=header>=8 and offset+size<=len(manifest)
        if kind==0x180:
            resources=list(struct.unpack_from('<'+'I'*((size-header)//4),manifest,offset+header))
        elif kind==0x102:
            start,stride,count=struct.unpack_from('<HHH',manifest,offset+24)
            for index in range(count):
                attribute=offset+16+start+stride*index
                name=struct.unpack_from('<I',manifest,attribute+4)[0]
                if name<len(resources) and resources[name]==0x01010027:
                    assert manifest[attribute+15]==3,'URI scheme must be a string'
                    schemes.append(struct.unpack_from('<I',manifest,attribute+16)[0])
        offset+=size
    assert len(schemes)==len(set(schemes))==3,'Encoded host-phone tel/sms/smsto schemes missing'
    assert 'assets/licenses/android9-NOTICE.xml.gz' in names
    assert not any(n.endswith('.bc') for n in names),'Guest RenderScript must remain opaque to host ABI scan'
    for name in ('libpocket_direct_boot.so','libpocket_binder_api28.so','libpocket_hidl_api28.so'):
        assert 'assets/api28-rootfs/system/lib64/'+name+'.payload' in names
print('PASS: isolated API 25/API 28 image layouts, direct service, licenses and ABI-safe payloads')
