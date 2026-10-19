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
a=p.parse_args()
with zipfile.ZipFile(a.seed) as seed,zipfile.ZipFile(a.out,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as apk:
    manifest=seed.read('AndroidManifest.xml')
    package='dev.kreijstal.aosppocket.managerdev'
    if package.encode() not in manifest and package.encode('utf-16le') not in manifest:
        raise SystemExit('Host updates are restricted to the separate managerdev package')
    for entry in seed.infolist():
        name=entry.filename
        if name=='classes.dex' or name.startswith(('META-INF/','assets/default-guest/')):continue
        if name.startswith('assets/api28-rootfs/') and name!='assets/api28-rootfs/system/build.prop.payload':continue
        apk.writestr(entry,seed.read(entry))
    apk.write(a.dex,'classes.dex')
    apk.writestr('assets/manager-dev-update.json',json.dumps({'format':1,'requires_full_manager_seed':True,'guest_sdk':28})+'\n')
print('Packed development-only host update:',a.out)
