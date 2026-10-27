#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package the Android 10 platform as an immutable, downloadable Manager image."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import urllib.parse
import zipfile
import zlib
from target import ROOT


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fixture-root',type=Path,default=ROOT/'out/api29/system-server-probe/root')
    p.add_argument('--out',type=Path,default=ROOT/'out/api29/image')
    p.add_argument('--version',default='0.77.0')
    p.add_argument('--url',required=True)
    p.add_argument('--probe',action='store_true')
    args=p.parse_args();source=args.fixture_root.resolve();out=args.out.resolve()
    url=urllib.parse.urlsplit(args.url)
    if not (url.scheme=='https' or args.probe and url.scheme=='http' and url.hostname=='127.0.0.1'):
        p.error('Image URL must use HTTPS (loopback HTTP requires --probe)')
    if url.username or url.password or url.fragment:p.error('Invalid image URL')
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+)*',args.version):p.error('Invalid image version')
    if out.is_relative_to(source):p.error('Output must be outside the input image')
    assert 'ro.build.version.sdk=29\n' in (source/'system/build.prop').read_text()
    out.mkdir(parents=True,exist_ok=True)
    rc=(source/'system/etc/init/init.pocket.rc').read_text()
    rc=rc[rc.index('\non boot\n'):]
    before,ui=rc.split('\non property:sys.boot_completed=1\n',1)
    assert before.count('    start pocket-surfaceflinger\n')==1
    before=before.replace('    start pocket-surfaceflinger\n','    start pocket-surfaceflinger\n    start pocket-platform-ready\n')
    ui=ui.replace('pocket-ui-check','pocket-control')
    ui=ui.replace('dev.kreijstal.aosppocket.guest.FrameworkProbe /system/etc/pocket-smoke.apk','dev.kreijstal.aosppocket.guest.GuestControl')
    ui=ui.replace('    oneshot\n','    oneshot\n    socket pocket_control stream 0600 1000 1000\n')
    ui=ui.replace('/tmp/properties.log','/pocket-console')
    ready='''
service pocket-platform-ready /system/bin/pocket-framework-probe-api29 --start
    class hardware
    user 0
    group 0
    oneshot
    setenv LD_PRELOAD /system/lib64/libpocket_binder_api29.so
'''
    rc='# Android 10 Manager startup; matching AOSP init service supervisor.\n'+before+ready+'\non property:sys.boot_completed=1\n'+ui
    assert 'pocket-init-test' not in rc and 'pocket-smoke.apk' not in rc and 'GuestControl' in rc
    generated={'system/etc/init/init.syscall.rc':rc.encode(),
        'system/etc/pocket-api29.env':('\n'.join(line for line in (source/'probe-environment').read_text().splitlines()
            if not line.startswith('POCKET_'))+'\n').encode()}
    excluded={'system/etc/init/init.pocket.rc','system/etc/init/pocket-import.rc','system/etc/pocket-smoke.apk'}
    for name in ['property-probe','binder-probe','init-test','zygote-client','hardware-probe','gralloc-probe','egl-probe']:
        excluded.add('system/bin/pocket-'+name+'-api29')
    files={}
    for top in ['system','vendor','apex']:
        for path in sorted((source/top).rglob('*')):
            if path.is_symlink():raise ValueError('Image source contains a symlink: '+str(path))
            if not path.is_file():continue
            name=path.relative_to(source).as_posix()
            if name not in excluded:files[name]=path
    files.update(generated)
    entries={}
    target=out/('pocket-api29-arm64-'+args.version+'.zip')
    with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as archive:
        for name,value in sorted(files.items()):
            data=value.read_bytes() if isinstance(value,Path) else value
            mode=0o755 if data.startswith((b'\x7fELF',b'#!')) else 0o644
            entry=zipfile.ZipInfo(name,(1980,1,1,0,0,0));entry.compress_type=zipfile.ZIP_DEFLATED
            entry.external_attr=(0o100000|mode)<<16
            archive.writestr(entry,data,compresslevel=1)
            entries[name]={'size':len(data),'crc32':zlib.crc32(data),'sha256':hashlib.sha256(data).hexdigest(),'mode':mode}
        manifest={'schema':1,'profile':'api29-direct','sdk':29,'abi':'arm64-v8a','runtime':1,'version':args.version,'entries':entries}
        raw=json.dumps(manifest,sort_keys=True,separators=(',',':')).encode()+b'\n'
        entry=zipfile.ZipInfo('pocket-image.json',(1980,1,1,0,0,0));entry.compress_type=zipfile.ZIP_DEFLATED
        archive.writestr(entry,raw,compresslevel=1)
    image={key:manifest[key] for key in ['profile','sdk','abi','runtime','version']}
    image.update(url=args.url,bytes=target.stat().st_size,unpacked_bytes=sum(e['size'] for e in entries.values()),
        sha256=hashlib.file_digest(target.open('rb'),'sha256').hexdigest(),manifest_sha256=hashlib.sha256(raw).hexdigest(),manifest=manifest)
    (out/'pocket-images.json').write_text(json.dumps({'schema':1,'images':[image]},sort_keys=True,separators=(',',':'))+'\n')
    (out/'image-inputs.lock.json').write_text(json.dumps({'sources':json.loads((ROOT/'sources.lock.json').read_text())['guest_api29'],
        'imageSha256':image['sha256'],'files':len(entries),'productionManagerVerified':False},indent=2)+'\n')
    print('API29_IMAGE_BUILD_PASS',target,'files='+str(len(entries)),'bytes='+str(image['bytes']),'sha256='+image['sha256'])


if __name__=='__main__':main()
