#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the image-free dual API 28/29 Manager using the established host JNI."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import urllib.parse
import xml.etree.ElementTree as ET
import zipfile
from target import ROOT


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-apk',type=Path,default=ROOT/'out/download-image/release/aosp-pocket-manager-0.76.apk')
    p.add_argument('--catalog',type=Path,required=True)
    p.add_argument('--out',type=Path,default=ROOT/'out/api29/manager')
    p.add_argument('--probe',action='store_true')
    args=p.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    catalog=json.loads((ROOT/'runtime/images/api28-arm64.json').read_text())
    new=json.loads(args.catalog.read_text());assert catalog['schema']==new['schema']==1
    catalog['images']=[image for image in catalog['images'] if image['sdk']==28]
    images=[image for image in new['images'] if image['sdk']==29];assert len(images)==1
    catalog['images']+=images
    for image in catalog['images']:
        assert image['profile']=='api'+str(image['sdk'])+'-direct' and image['abi']=='arm64-v8a' and image['runtime']==1
        url=urllib.parse.urlsplit(image['url'])
        assert url.scheme=='https' or args.probe and url.scheme=='http' and url.hostname=='127.0.0.1'
    tool=ROOT.parent/'helloworldandroid';android=tool/'vendor/android-platforms/android-36/android.jar'
    r8=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar'
    def run(*argv):subprocess.run(list(map(str,argv)),cwd=ROOT,check=True)
    for name in ['classes','dex','manifest-tools']:
        path=out/name
        if path.exists():shutil.rmtree(path)
        path.mkdir()
    java=sorted((ROOT/'runtime/app/src').rglob('*.java'))+sorted((ROOT/'runtime/storage/src').rglob('*.java'))+[ROOT/'runtime/direct/BootBattery.java']
    if args.probe:java.append(ROOT/'runtime/tests/api29/ManagerProbe.java')
    run('javac','--release','8','-cp',android,'-d',out/'classes',*java)
    run('jar','cf',out/'classes.jar','-C',out/'classes','.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',out/'dex',out/'classes.jar')
    manifest=ET.parse(ROOT/'runtime/app/AndroidManifest.xml');ns='{http://schemas.android.com/apk/res/android}'
    if args.probe:
        manifest.getroot().set('package','dev.kreijstal.aosppocket.api29manager')
        for element in manifest.getroot().iter():
            for key in ['name','targetActivity']:
                value=element.get(ns+key,'')
                if value.startswith('.'):element.set(ns+key,'dev.kreijstal.aosppocket.runtime'+value)
        app=manifest.getroot().find('application');app.set(ns+'usesCleartextTraffic','true')
        ET.SubElement(app,'activity',{ns+'name':'dev.kreijstal.aosppocket.runtime.ManagerProbe',ns+'exported':'true'})
    ET.register_namespace('android',ns[1:-1]);manifest.write(out/'manifest-source.xml',encoding='utf-8',xml_declaration=True)
    run('javac','--release','8','-cp',tool/'build/tools','-d',out/'manifest-tools',ROOT/'runtime/tools/PocketManifest.java')
    run('java','-cp',str(tool/'build/tools')+':'+str(out/'manifest-tools'),'PocketManifest',out/'manifest-source.xml',out/'AndroidManifest.xml','28','28','77','0.77')
    with zipfile.ZipFile(args.base_apk) as base,zipfile.ZipFile(out/'unsigned.apk','w',zipfile.ZIP_DEFLATED,compresslevel=1) as apk:
        for entry in base.infolist():
            if entry.filename in ['classes.dex','AndroidManifest.xml','assets/pocket-images.json'] or entry.filename.startswith(('META-INF/','assets/api28-rootfs/','assets/default-guest/')):continue
            apk.writestr(entry,base.read(entry))
        apk.write(out/'dex/classes.dex','classes.dex');apk.write(out/'AndroidManifest.xml','AndroidManifest.xml')
        apk.writestr('assets/pocket-images.json',json.dumps(catalog,sort_keys=True,separators=(',',':'))+'\n')
        apk.write(ROOT/'out/api29/reference/system/etc/NOTICE.xml.gz','assets/licenses/android10-NOTICE.xml.gz')
        if args.probe:apk.write(ROOT/'out/runtime/guest-test.apk','assets/api29-smoke.apk')
    target=out/('api29-manager-probe.apk' if args.probe else 'aosp-pocket-manager-0.77.apk')
    signer=['java','-cp',tool/'build/apksigner.jar','com.android.apksigner.ApkSignerTool']
    run(*signer,'sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android','--out',target,out/'unsigned.apk')
    run(*signer,'verify',target)
    with zipfile.ZipFile(target) as apk,zipfile.ZipFile(args.base_apk) as base:
        assert not any(name.startswith(('assets/default-guest/','assets/api28-rootfs/','assets/api29-rootfs/')) for name in apk.namelist())
        for name in base.namelist():
            if name.startswith('lib/'):assert apk.read(name)==base.read(name),'Host JNI changed: '+name
    report={'apk':str(target),'sha256':hashlib.file_digest(target.open('rb'),'sha256').hexdigest(),'bytes':target.stat().st_size,
        'probe':args.probe,'guestImagesBundled':0,'hostNativeLibrariesUnchanged':True,'guestSdks':[28,29],'runtimeValidated':False}
    (out/'build.json').write_text(json.dumps(report,indent=2)+'\n');print('API29_MANAGER_BUILD_PASS',json.dumps(report))


if __name__=='__main__':main()
