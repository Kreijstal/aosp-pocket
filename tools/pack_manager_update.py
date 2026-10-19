#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Small development host update over an already installed full Manager seed.

The seed's private guest files must remain installed. This APK is deliberately
not a standalone distribution; manager-verify builds the full upgrade APK.
"""
import argparse,json,zipfile
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--seed',type=Path,required=True)
p.add_argument('--dex',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--base',type=Path,help='Fresh managerdev base APK supplying the current manifest')
p.add_argument('--image',type=Path,help='Current full Manager APK; include guest payloads changed from the seed')
a=p.parse_args()
if a.image and not a.base:p.error('--image requires --base so manifest permissions match the updated host')
with zipfile.ZipFile(a.seed) as seed,zipfile.ZipFile(a.out,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as apk:
    manifest=seed.read('AndroidManifest.xml')
    package='dev.kreijstal.aosppocket.managerdev'
    if package.encode() not in manifest and package.encode('utf-16le') not in manifest:
        raise SystemExit('Host updates are restricted to the separate managerdev package')
    if a.base:
        with zipfile.ZipFile(a.base) as base:manifest=base.read('AndroidManifest.xml')
        if package.encode() not in manifest and package.encode('utf-16le') not in manifest:
            raise SystemExit('Update manifest must use the separate managerdev identity')
    delta=0
    with zipfile.ZipFile(a.image or a.seed) as current:
        for entry in current.infolist():
            name=entry.filename
            if name in ('classes.dex','AndroidManifest.xml') or name.startswith(('META-INF/','assets/default-guest/')):continue
            if name.startswith('assets/api28-rootfs/') and name!='assets/api28-rootfs/system/build.prop.payload':
                previous=seed.getinfo(name) if name in seed.namelist() else None
                if not a.image or previous and (previous.CRC,previous.file_size)==(entry.CRC,entry.file_size):continue
                delta+=1
            apk.writestr(entry,current.read(entry))
    apk.writestr('AndroidManifest.xml',manifest)
    apk.write(a.dex,'classes.dex')
    apk.writestr('assets/manager-dev-update.json',json.dumps({'format':1,'requires_full_manager_seed':True,'guest_sdk':28,'guest_delta_entries':delta})+'\n')
print('Packed development-only host update:',a.out)
