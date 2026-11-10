#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate the host-only distribution and its pinned downloadable image."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import urllib.parse
import zipfile


def validate(apk_path,ndk,allow_image_probe32=False):
    with zipfile.ZipFile(apk_path) as apk,tempfile.TemporaryDirectory() as temporary:
        names=apk.namelist();assert len(names)==len(set(names)),'Duplicate APK entries'
        assert not any(n.startswith(('assets/default-guest/','assets/api28-rootfs/','assets/api29-rootfs/','assets/api32-rootfs/')) for n in names),'APK contains a guest image'
        if allow_image_probe32:
            assert any(package.encode('utf-16-le')+b'\0\0' in apk.read('AndroidManifest.xml') for package in ('dev.kreijstal.aosppocket.api32managerimage','dev.kreijstal.aosppocket.api32managerimage2','dev.kreijstal.aosppocket.api32managerimage3','dev.kreijstal.aosppocket.api32ceimage','dev.kreijstal.aosppocket.api32ceimage2','dev.kreijstal.aosppocket.api32ceimage3'))
            assert b'Ldev/kreijstal/aosppocket/runtime/ImageProbe32;' in apk.read('classes.dex')
        assert 'assets/licenses/android9-NOTICE.xml.gz' in names
        catalog=json.loads(apk.read('assets/pocket-images.json'));assert catalog['schema']==1
        sdks=[image['sdk'] for image in catalog['images']]
        assert sdks and len(sdks)==len(set(sdks)) and set(sdks)<=set((28,29,32))
        for image in catalog['images']:
            sdk=image['sdk'];manifest=image['manifest']
            assert image['profile']=='api'+str(sdk)+'-direct' and image['abi']=='arm64-v8a' and image['runtime']==1
            url=urllib.parse.urlsplit(image['url'])
            assert (url.scheme=='https' or allow_image_probe32 and sdk==32 and url.scheme=='http' and url.hostname=='127.0.0.1') and url.hostname and not url.username and not url.password and not url.fragment,'Invalid image URL'
            assert 0<image['bytes']<=2*1024**3 and 0<image['unpacked_bytes']<=2*1024**3
            assert re.fullmatch('[0-9a-f]{64}',image['sha256']) and re.fullmatch('[0-9a-f]{64}',image['manifest_sha256'])
            assert manifest['schema']==1
            for key in ('profile','sdk','abi','runtime','version'):assert image[key]==manifest[key]
            entries=manifest['entries'];assert 0<len(entries)<=20000
            assert sum(spec['size'] for spec in entries.values())==image['unpacked_bytes']
            prefixes=('system/','system_ext/','vendor/','product/','apex/','odm/','oem/') if sdk==32 else ('system/','vendor/','apex/') if sdk==29 else ('system/','vendor/')
            for name,spec in entries.items():
                assert (name.startswith(prefixes) or sdk==32 and name in ('.pocket-readonly-system','bootstrap-linkerconfig')) and '\\' not in name and '\0' not in name and all(p not in ('','.','..') for p in name.split('/'))
                assert spec['size']>=0 and re.fullmatch('[0-9a-f]{64}',spec['sha256']) and 0<=spec['mode']<=0o777
                assert 0<=spec['crc32']<=0xffffffff
            for required in ('system/bin/pocket-loader','system/bin/pocket-init-api'+str(sdk),'system/usr/keychars/Virtual.kcm','system/framework/pocket-zygote.jar'):
                assert required in entries,required
            if sdk==29:
                assert 'assets/licenses/android10-NOTICE.xml.gz' in names
                for required in ('apex/com.android.runtime/lib64/bionic/libc.so','system/etc/ld.config.29.txt','system/etc/pocket-api29.env'):
                    assert required in entries,required
            if sdk==32:
                if 'child_supervision' in image or 'child_supervision' in manifest:
                    assert image.get('child_supervision')==manifest.get('child_supervision')=='kernel-subreaper'
                assert 'assets/licenses/android12l-NOTICE.xml.gz' in names
                for metadata in (image,manifest):
                    assert metadata['experimental'] is True and metadata['service_layout']==2
                    assert metadata['boot_protocol']=='api32-v1' and metadata['storage']=='app-private'
                for required in ('.pocket-readonly-system','bootstrap-linkerconfig','system/bin/getprop','system/bin/pocket-setprop-api32',
                        'system/bin/pocket-manager-api32','system/framework/pocket-manager.jar',
                        'system/bin/pocket-core-api32','system/bin/pocket-crypto-api32','system/bin/pocket-runtime-prepare-api32',
                        'system/bin/pocket-runtime-wait-api32','system/etc/init/pocket-runtime.rc','system/etc/pocket-runtime-boot.sh',
                        'system/etc/pocket-runtime-env.sh','system/framework/pocket-tools.jar',
                        'apex/com.android.art/javalib/arm64/boot.art','apex/com.android.runtime/lib64/bionic/libc.so',
                        'product/overlay/PocketMemory/PocketMemory.apk'):
                    assert required in entries,required
        # Production never opts into loopback HTTP; that override is confined
        # to the separate download-test application's manifest.
        binary=apk.read('AndroidManifest.xml');resources=[];offset=8
        while offset<len(binary):
            kind,header,size=struct.unpack_from('<HHI',binary,offset);assert size>=header>=8 and offset+size<=len(binary)
            if kind==0x180:resources=list(struct.unpack_from('<'+'I'*((size-header)//4),binary,offset+header))
            elif kind==0x102:
                start,stride,count=struct.unpack_from('<HHH',binary,offset+24)
                for index in range(count):
                    attribute=offset+16+start+index*stride;name=struct.unpack_from('<I',binary,attribute+4)[0]
                    if name<len(resources) and resources[name]==0x010104ec:assert struct.unpack_from('<I',binary,attribute+16)[0]==(0xffffffff if allow_image_probe32 else 0),'Unexpected cleartext traffic policy'
            offset+=size
        readelf=Path(ndk)/'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf'
        for name in ('libpocket_runtime.so','libproot.so','libpocket_frames.so','libpocket_ce.so'):
            path=Path(temporary)/name;path.write_bytes(apk.read('lib/arm64-v8a/'+name))
            output=subprocess.check_output([str(readelf),'-hlW',str(path)],text=True)
            assert 'AArch64' in output,name
            loads=[line for line in output.splitlines() if line.strip().startswith('LOAD ')]
            assert loads and all(int(line.split()[-1],16)>=16384 for line in loads),name
    print('PASS: image-free '+('isolated image probe' if allow_image_probe32 else 'production APK')+', pinned SDK '+','.join(map(str,sdks))+' inventories, notices and ARM64/16-KiB host runtime')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('apk',type=Path);parser.add_argument('--ndk',required=True,type=Path)
    parser.add_argument('--allow-image-probe32',action='store_true',help='Allow loopback HTTP only for the isolated API32 image probe')
    args=parser.parse_args();validate(args.apk,args.ndk,args.allow_image_probe32)
