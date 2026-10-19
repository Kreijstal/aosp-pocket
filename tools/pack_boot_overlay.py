#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package changed guest assets for explicit local development without reinstall."""
import argparse
import json
from pathlib import Path
import uuid
import zipfile
import zlib
from prepare_api28 import ROOT

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base-apk',type=Path,required=True,help='APK actually installed in the test VM')
parser.add_argument('--output',type=Path,default=ROOT/'out/boot/native-overlay.zip')
parser.add_argument('--proof-marker',action='store_true')
args=parser.parse_args()
files={p.relative_to(ROOT/'out/boot/rootfs').as_posix():p for p in (ROOT/'out/boot/rootfs').rglob('*')
       if p.is_file() and p.relative_to(ROOT/'out/boot/rootfs').parts[0] in ('system','vendor')}
files['system/framework/pocket-zygote.jar']=ROOT/'out/boot/pocket-zygote.jar'
changed=[]
with zipfile.ZipFile(args.base_apk) as baseline:
    for name,path in (('classes.dex',ROOT/'out/boot/classes.dex'),
                      ('AndroidManifest.xml',ROOT/'out/boot/AndroidManifest.xml'),
                      ('lib/arm64-v8a/libpocket_runtime.so',ROOT/'out/runtime-native/libpocket_runtime.so')):
        old=baseline.getinfo(name);data=path.read_bytes()
        if (old.CRC,old.file_size)!=(zlib.crc32(data),len(data)):
            raise SystemExit('Host APK changed; reinstall it before using native overlays: '+name)
with zipfile.ZipFile(args.base_apk) as base,zipfile.ZipFile(args.output,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as output:
    for name,path in sorted(files.items()):
        data=path.read_bytes()
        try: previous=base.getinfo('assets/api28-rootfs/'+name+'.payload')
        except KeyError: previous=None
        if previous and previous.file_size==len(data) and previous.CRC==zlib.crc32(data): continue
        output.writestr(name,data);changed.append(name)
    if args.proof_marker:
        name='system/etc/pocket-overlay-proof';output.writestr(name,uuid.uuid4().hex+'\n');changed.append(name)
    output.writestr('manifest.json',json.dumps({'schema':1,'profile':'api28-direct','files':changed,
        'libc_hooks':len(json.loads((ROOT/'out/boot/hooks.json').read_text())['libc_hooks']),
        'runtime_verification_required':True},indent=2)+'\n')
print(json.dumps({'output':str(args.output),'changed_files':changed,'bytes':args.output.stat().st_size}))
