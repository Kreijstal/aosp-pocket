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
from build_boot import reserve_interpreter
from build_host_native import load_manifest as load_host_native_manifest
from build_api32_external_host import load_manifest as load_external_manifest,LIBRARY as EXTERNAL_LIBRARY


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-apk',type=Path,default=ROOT/'out/download-image/release/aosp-pocket-manager-0.76.apk')
    p.add_argument('--host-native-dir',type=Path,help='Use all four source-built ARM64 host libraries with a verified build.json')
    p.add_argument('--external-native-dir',type=Path,help='Source-built host AppFuse JNI for selected API32 emulated-volume images')
    p.add_argument('--catalog',type=Path,required=True)
    p.add_argument('--api32-catalog',type=Path,help='Include the experimental, separately downloaded Android 12L image')
    p.add_argument('--out',type=Path,default=ROOT/'out/api29/manager')
    p.add_argument('--media-session-patch32',type=Path,help='Additional reversible native/config patches for the MediaProvider trial')
    p.add_argument('--media-session-image32',action='store_true',help='Validate the packaged API32 image without replacing its runtime code')
    p.add_argument('--media-session-lazy-probe32',type=Path,help='Media activation client for the packaged-image trial')
    p.add_argument('--media-session-metrics-probe32',type=Path,help='Stock media.metrics caller-policy probe for the packaged-image trial')
    p.add_argument('--ce-init-probe32',type=Path,help='Reversible native-init replacement for the preserved CE authless trial')
    p.add_argument('--gatekeeper-client32',type=Path,help='Native credential lifecycle client for a separate app-probe diagnostic')
    p.add_argument('--installd-client32',type=Path,help='Native app-data/permissions/dexopt checks before the full API32 app workload')
    p.add_argument('--installd-postboot32',action='store_true',help='Require real DEXOPT_BOOTCOMPLETE compilation in the native installd check')
    probes=p.add_mutually_exclusive_group()
    probes.add_argument('--probe',action='store_true')
    probes.add_argument('--app-probe',action='store_true',help='Build the isolated real-APK compatibility fixture')
    probes.add_argument('--image-probe32',action='store_true',help='Validate API32 download/preparation in a separate app sandbox')
    probes.add_argument('--session-probe32',action='store_true',help='Validate the actual Manager API32 live session')
    probes.add_argument('--persistence-probe32',action='store_true',help='Validate cached-image restart and private app/keystore data across Manager restart')
    probes.add_argument('--app-probe32',type=Path,help='Validate an unchanged API32 APK set using its pinned download specification')
    probes.add_argument('--ce-probe32',action='store_true',help='Validate real12L credential-encrypted app storage in a separate profile')
    probes.add_argument('--media-session-probe32',type=Path,help='Reversible real MediaProvider session trial using an existing cached plaintext profile')
    p.add_argument('--ce-probe32-package',choices=('dev.kreijstal.aosppocket.api32ceimage','dev.kreijstal.aosppocket.api32ceimage2','dev.kreijstal.aosppocket.api32ceimage3'),default='dev.kreijstal.aosppocket.api32ceimage',help='Separate sandbox for a new CE trial while preserving earlier profiles')
    p.add_argument('--image-probe32-package',choices=('dev.kreijstal.aosppocket.api32managerimage','dev.kreijstal.aosppocket.api32managerimage2','dev.kreijstal.aosppocket.api32managerimage3'),help='Separate non-CE API32 test sandbox, preserving existing profiles')
    args=p.parse_args();args.image_probe32=args.image_probe32 or args.session_probe32 or args.persistence_probe32 or bool(args.app_probe32) or args.ce_probe32 or bool(args.media_session_probe32);args.probe=args.probe or args.app_probe or args.image_probe32
    if args.image_probe32_package and (not args.image_probe32 or args.ce_probe32):p.error('--image-probe32-package requires a non-CE API32 probe')
    if args.media_session_patch32 and not args.media_session_probe32:p.error('--media-session-patch32 requires --media-session-probe32')
    if args.media_session_image32 and (not args.media_session_probe32 or args.media_session_patch32):p.error('--media-session-image32 requires --media-session-probe32 and no code patches')
    if args.media_session_image32 and not args.media_session_lazy_probe32:p.error('--media-session-image32 requires --media-session-lazy-probe32 for the complete media checks')
    if args.media_session_lazy_probe32 and not args.media_session_image32:p.error('--media-session-lazy-probe32 requires --media-session-image32')
    if args.media_session_metrics_probe32 and not args.media_session_image32:p.error('--media-session-metrics-probe32 requires --media-session-image32')
    if args.ce_init_probe32 and not args.ce_probe32:p.error('--ce-init-probe32 requires --ce-probe32')
    if args.gatekeeper_client32 and not args.app_probe32:p.error('--gatekeeper-client32 requires --app-probe32')
    if args.installd_client32 and not (args.app_probe32 or args.ce_probe32):p.error('--installd-client32 requires --app-probe32 or --ce-probe32')
    if args.installd_postboot32 and not args.installd_client32:p.error('--installd-postboot32 requires --installd-client32')
    if args.image_probe32 and not args.api32_catalog:p.error('--image-probe32 requires --api32-catalog')
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    host_native=load_host_native_manifest(args.host_native_dir) if args.host_native_dir else None
    external_native=load_external_manifest(args.external_native_dir) if args.external_native_dir else None
    if external_native and args.media_session_probe32:p.error('--external-native-dir and the legacy MediaProvider component fixture use separate host adapters')
    catalog=json.loads((ROOT/'runtime/images/api28-arm64.json').read_text())
    new=json.loads(args.catalog.read_text());assert catalog['schema']==new['schema']==1
    catalog['images']=[image for image in catalog['images'] if image['sdk']==28]
    images=[image for image in new['images'] if image['sdk']==29];assert len(images)==1
    catalog['images']+=images
    if args.api32_catalog:
        extra=json.loads(args.api32_catalog.read_text());assert extra['schema']==1
        images=[image for image in extra['images'] if image['sdk']==32];assert len(images)==1
        for metadata in (images[0],images[0]['manifest']):
            assert metadata['experimental'] is True and metadata['service_layout']==2
            assert metadata['boot_protocol']=='api32-v1' and metadata['storage']=='app-private'
        catalog['images']+=images
        if images[0]['manifest'].get('external_volume')=='rootless-v1' and not external_native:p.error('Selected API32 external volume requires --external-native-dir')
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
    probe_class='ManagerMediaSessionProbe32' if args.media_session_probe32 else 'ManagerCeProbe32' if args.ce_probe32 else 'ManagerAppProbe32' if args.app_probe32 else 'ManagerPersistenceProbe32' if args.persistence_probe32 else 'ManagerSessionProbe32' if args.session_probe32 else 'ImageProbe32' if args.image_probe32 else 'AppProbe' if args.app_probe else 'ManagerProbe'
    if args.probe:java.append(ROOT/('runtime/tests/'+('api32/' if args.image_probe32 else 'api29/')+probe_class+'.java'))
    if args.session_probe32 or args.persistence_probe32 or args.app_probe32 or args.ce_probe32 or args.media_session_probe32:java.append(ROOT/'runtime/tests/api32/ImageProbe32.java')
    if args.persistence_probe32 or args.app_probe32 or args.ce_probe32 or args.media_session_probe32:java.append(ROOT/'runtime/tests/api32/ManagerSessionProbe32.java')
    if args.app_probe32:java += [ROOT/'runtime/tests/api32/AppInspector32.java',ROOT/'runtime/tests/api32/VolumeControl32.java',ROOT/'runtime/tests/api32/MediaPermissions32.java']
    if args.ce_probe32:java.append(ROOT/'runtime/tests/api32/CeAppInspector32.java')
    run('javac','--release','8','-cp',android,'-d',out/'classes',*java)
    run('jar','cf',out/'classes.jar','-C',out/'classes','.')
    run('java','-cp',r8,'com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',out/'dex',out/'classes.jar')
    manifest=ET.parse(ROOT/'runtime/app/AndroidManifest.xml');ns='{http://schemas.android.com/apk/res/android}'
    if args.probe:
        manifest.getroot().set('package',args.ce_probe32_package if args.ce_probe32 else (args.image_probe32_package or 'dev.kreijstal.aosppocket.api32managerimage') if args.image_probe32 else 'dev.kreijstal.aosppocket.api29manager')
        for element in manifest.getroot().iter():
            for key in ['name','targetActivity']:
                value=element.get(ns+key,'')
                if value.startswith('.'):element.set(ns+key,'dev.kreijstal.aosppocket.runtime'+value)
        app=manifest.getroot().find('application');app.set(ns+'usesCleartextTraffic','true')
        probe_activity=ET.SubElement(app,'activity',{ns+'name':'dev.kreijstal.aosppocket.runtime.'+probe_class,ns+'exported':'true'})
        if args.ce_probe32 or args.media_session_probe32:probe_activity.set(ns+'launchMode','2') # singleTask; binary encoder accepts numeric enums
        if args.app_probe32 and json.loads(args.app_probe32.read_text()).get('managerCrashRecovery'):
            probe_activity.set(ns+'launchMode','2')
        if args.session_probe32 or args.persistence_probe32 or args.app_probe32 or args.ce_probe32:ET.SubElement(app,'activity',{ns+'name':'dev.kreijstal.aosppocket.runtime.ImageProbe32',ns+'exported':'true'})
    ET.register_namespace('android',ns[1:-1]);manifest.write(out/'manifest-source.xml',encoding='utf-8',xml_declaration=True)
    run('javac','--release','8','-cp',tool/'build/tools','-d',out/'manifest-tools',ROOT/'runtime/tools/PocketManifest.java')
    version='0.78' if args.api32_catalog else '0.77';version_code='78' if args.api32_catalog else '77'
    run('java','-cp',str(tool/'build/tools')+':'+str(out/'manifest-tools'),'PocketManifest',out/'manifest-source.xml',out/'AndroidManifest.xml','28','28',version_code,version)
    with zipfile.ZipFile(args.base_apk) as base,zipfile.ZipFile(out/'unsigned.apk','w',zipfile.ZIP_DEFLATED,compresslevel=1) as apk:
        for entry in base.infolist():
            if external_native and entry.filename=='lib/arm64-v8a/'+EXTERNAL_LIBRARY:continue
            if args.ce_probe32 and entry.filename in ['assets/api32-ce-init','assets/api32-ce-init.json','assets/api32-ce-test.apk']:continue
            if entry.filename in ['classes.dex','AndroidManifest.xml','assets/pocket-images.json','assets/licenses/android10-NOTICE.xml.gz','assets/licenses/android12l-NOTICE.xml.gz'] or entry.filename.startswith(('META-INF/','assets/api28-rootfs/','assets/default-guest/')):continue
            if args.app_probe32 and entry.filename in ['assets/api32-app-spec.json','assets/api32-network-client','assets/api32-dns-client']:continue
            if args.gatekeeper_client32 and entry.filename=='assets/api32-gatekeeper-client':continue
            if (args.app_probe32 or args.ce_probe32) and entry.filename in ['assets/api32-installd-client','assets/api32-installd-smoke.apk','assets/api32-installd-postboot']:continue
            if args.media_session_probe32 and (entry.filename.startswith('assets/media-session-patch/') or entry.filename in ['assets/media-session-patches.json','assets/api32-media-image.json','assets/api32-media-session.apk','assets/api32-media-client','assets/api32-lazy-media-probe','assets/api32-metrics-probe','assets/api32-media-proxy.json','lib/arm64-v8a/libpocket_external32.so']):continue
            if (args.session_probe32 or args.persistence_probe32 or args.app_probe32 or args.ce_probe32) and entry.filename=='assets/api32-tombstoned-client':continue
            if host_native and entry.filename.startswith('lib/arm64-v8a/') and entry.filename.removeprefix('lib/arm64-v8a/') in host_native['libraries']:
                apk.write(args.host_native_dir/entry.filename.removeprefix('lib/arm64-v8a/'),entry.filename);continue
            apk.writestr(entry,base.read(entry))
        apk.write(out/'dex/classes.dex','classes.dex');apk.write(out/'AndroidManifest.xml','AndroidManifest.xml')
        if external_native:apk.write(args.external_native_dir/EXTERNAL_LIBRARY,'lib/arm64-v8a/'+EXTERNAL_LIBRARY)
        apk.writestr('assets/pocket-images.json',json.dumps(catalog,sort_keys=True,separators=(',',':'))+'\n')
        apk.write(ROOT/'out/api29/reference/system/etc/NOTICE.xml.gz','assets/licenses/android10-NOTICE.xml.gz')
        if args.api32_catalog:apk.write(ROOT/'out/api32/android-probe/root/system/etc/NOTICE.xml.gz','assets/licenses/android12l-NOTICE.xml.gz')
        if args.media_session_probe32:
            media=args.media_session_probe32.resolve()
            media_spec=json.loads((media/'build.json').read_text())
            if media_spec.get('loaderSha256'):assert hashlib.sha256((media/'pocket-loader').read_bytes()).hexdigest()==media_spec['loaderSha256']
            apk.write(media/'media-session.apk','assets/api32-media-session.apk')
            apk.write(media/'pocket-media-client','assets/api32-media-client')
            patch_files={} if args.media_session_image32 else {'system/bin/pocket-loader':media/'pocket-loader','system/lib64/libpocket_zygote_api32.so':ROOT/'out/api32/native/libpocket_zygote_api32.so'}
            if args.media_session_image32:
                image32=next(i for i in catalog['images'] if i['sdk']==32)
                assert image32['service_layout']==2 and image32['boot_protocol']=='api32-v1'
                apk.writestr('assets/api32-media-image.json',json.dumps({'imageSha256':image32['sha256']}))
                if args.media_session_lazy_probe32:apk.write(args.media_session_lazy_probe32,'assets/api32-lazy-media-probe')
                if args.media_session_metrics_probe32:apk.write(args.media_session_metrics_probe32,'assets/api32-metrics-probe')
            if args.media_session_patch32:
                allowed={'system/bin/pocket-core-api32','system/lib64/libpocket_binder_api32.so','system/lib64/libpocket_hidl_api32.so','system/etc/init/pocket-runtime.rc','system/etc/pocket-runtime-boot.sh'}
                extra={str(f.relative_to(args.media_session_patch32)):f for f in args.media_session_patch32.rglob('*') if f.is_file()}
                lazy_probe=extra.pop('pocket-lazy-media-probe')
                if 'system/lib64/libpocket_media_api32.so' in extra:allowed.add('system/lib64/libpocket_media_api32.so')
                if 'system/bin/pocket-init-api32' in extra:allowed.add('system/bin/pocket-init-api32')
                memory_files={'system/etc/pocket-profile.prop','product/overlay/PocketMemory/PocketMemory.apk'}
                if memory_files & extra.keys():allowed.update(memory_files)
                bridge_files={'system/framework/pocket-zygote.jar','system/framework/pocket-tools.jar'}
                if bridge_files & extra.keys():allowed.update(bridge_files)
                assert set(extra)==allowed,'Unexpected trial patch files'
                patch_files.update(extra)
                apk.write(lazy_probe,'assets/api32-lazy-media-probe')
            for name,file in patch_files.items():apk.write(file,'assets/media-session-patch/'+name)
            apk.writestr('assets/media-session-patches.json',json.dumps({n:hashlib.sha256(f.read_bytes()).hexdigest() for n,f in patch_files.items()},sort_keys=True))
            if media_spec.get('externalProxy'):
                apk.write(media/'libpocket_external32.so','lib/arm64-v8a/libpocket_external32.so')
                apk.writestr('assets/api32-media-proxy.json',json.dumps(media_spec,sort_keys=True))
        if args.probe and not args.image_probe32:apk.write(ROOT/'out/runtime/guest-test.apk','assets/api29-smoke.apk')
        if args.session_probe32 or args.persistence_probe32 or args.app_probe32 or args.ce_probe32:
            if args.ce_probe32:
                apk.write(ROOT/'out/api32/ce-app/ce-test.apk','assets/api32-ce-test.apk')
                if args.ce_init_probe32:
                    ce_init=out/'ce-native-init';shutil.copyfile(args.ce_init_probe32,ce_init)
                    assert b'API32_INIT_SUBREAPER_READY' in ce_init.read_bytes()
                    assert reserve_interpreter(ce_init),'CE native init requires a loader reservation'
                    ce_init_spec={'sourceSha256':hashlib.sha256(args.ce_init_probe32.read_bytes()).hexdigest(),
                        'assetSha256':hashlib.sha256(ce_init.read_bytes()).hexdigest(),'kernelChildOwnership':True}
                    apk.write(ce_init,'assets/api32-ce-init');apk.writestr('assets/api32-ce-init.json',json.dumps(ce_init_spec,sort_keys=True))
            elif args.persistence_probe32:apk.write(ROOT/'out/api32/persistence-app/persistence-test.apk','assets/api32-persistence.apk')
            elif args.app_probe32:
                apk.write(args.app_probe32,'assets/api32-app-spec.json')
                network=out/'network-client'
                shutil.copyfile(ROOT/'out/api32/network/pocket-network-probe-api32',network)
                assert reserve_interpreter(network)
                apk.write(network,'assets/api32-network-client')
                dns=out/'dns-client'
                shutil.copyfile(ROOT/'out/api32/network/pocket-dns-probe-api32',dns)
                assert reserve_interpreter(dns)
                apk.write(dns,'assets/api32-dns-client')
                if args.gatekeeper_client32:
                    client=out/'gatekeeper-client'
                    shutil.copyfile(args.gatekeeper_client32,client)
                    assert reserve_interpreter(client)
                    apk.write(client,'assets/api32-gatekeeper-client')
            else:apk.write(ROOT/'out/runtime/guest-test.apk','assets/api32-manager-smoke.apk')
            if args.installd_client32:
                client=out/'installd-client'
                shutil.copyfile(args.installd_client32,client)
                assert reserve_interpreter(client)
                apk.write(client,'assets/api32-installd-client')
                apk.write(ROOT/'out/runtime/guest-test.apk','assets/api32-installd-smoke.apk')
                if args.installd_postboot32:apk.writestr('assets/api32-installd-postboot','v2\n')
            client=out/'tombstoned-client'
            shutil.copyfile(ROOT/'out/api32/tombstoned/pocket-tombstoned-probe-api32',client)
            assert reserve_interpreter(client)
            apk.write(client,'assets/api32-tombstoned-client')
    target=out/('api32-media-manager-probe.apk' if args.media_session_probe32 else 'api32-ce-manager-probe.apk' if args.ce_probe32 else 'api32-app-manager-probe.apk' if args.app_probe32 else 'api32-persistence-manager-probe.apk' if args.persistence_probe32 else 'api32-session-manager-probe.apk' if args.session_probe32 else 'api32-image-manager-probe.apk' if args.image_probe32 else 'api29-manager-probe.apk' if args.probe else 'aosp-pocket-manager-'+version+'.apk')
    signer=['java','-cp',tool/'build/apksigner.jar','com.android.apksigner.ApkSignerTool']
    run(*signer,'sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android','--out',target,out/'unsigned.apk')
    run(*signer,'verify',target)
    native_unchanged=True
    with zipfile.ZipFile(target) as apk,zipfile.ZipFile(args.base_apk) as base:
        assert not any(name.startswith(('assets/default-guest/','assets/api28-rootfs/','assets/api29-rootfs/','assets/api32-rootfs/')) for name in apk.namelist())
        for name in base.namelist():
            if name.startswith('lib/'):
                actual=apk.read(name);same=actual==base.read(name);native_unchanged=native_unchanged and same
                library=name.removeprefix('lib/arm64-v8a/')
                if host_native and library in host_native['libraries']:
                    assert hashlib.sha256(actual).hexdigest()==host_native['libraries'][library]['sha256'],'Source-built host JNI changed: '+name
                elif external_native and library==EXTERNAL_LIBRARY:assert hashlib.sha256(actual).hexdigest()==external_native['sha256']
                else:assert same,'Host JNI changed: '+name
        if host_native:
            for name,record in host_native['libraries'].items():assert hashlib.sha256(apk.read('lib/arm64-v8a/'+name)).hexdigest()==record['sha256']
        if external_native:assert hashlib.sha256(apk.read('lib/arm64-v8a/'+EXTERNAL_LIBRARY)).hexdigest()==external_native['sha256']
    report={'apk':str(target),'package':manifest.getroot().get('package'),'sha256':hashlib.file_digest(target.open('rb'),'sha256').hexdigest(),'bytes':target.stat().st_size,
        'probe':args.probe,'guestImagesBundled':0,'hostNativeLibrariesUnchanged':native_unchanged,'hostNativeBaseApkSha256':hashlib.file_digest(args.base_apk.open('rb'),'sha256').hexdigest(),'guestSdks':[image['sdk'] for image in catalog['images']],'runtimeValidated':False}
    if host_native:report['hostNativeSourceBuild']={'manifestSha256':hashlib.file_digest((args.host_native_dir/'build.json').open('rb'),'sha256').hexdigest(),'libraries':host_native['libraries']}
    if external_native:report['externalHostSourceBuild']={'manifestSha256':hashlib.file_digest((args.external_native_dir/'build.json').open('rb'),'sha256').hexdigest(),**external_native}
    if args.ce_init_probe32:report['ceInitTrial']=ce_init_spec
    if args.media_session_probe32 and media_spec.get('externalProxy'):report['externalFileJniAdditionSha256']=media_spec['externalJniSha256']
    if args.media_session_probe32:
        if media_spec.get('externalNamespace'):report['externalNamespaceTrial']=True
        with zipfile.ZipFile(target) as apk:
            report['packagedImageTrial']=args.media_session_image32
            report['guestCodePatchSha256']={name:hashlib.sha256(apk.read('assets/media-session-patch/'+name)).hexdigest() for name in patch_files}
            if 'system/lib64/libpocket_media_api32.so' in patch_files:
                report['extractorFirstDemand32']=b'API32_EXTRACTOR_PLUGINS_READY first_demand=1' in apk.read('assets/media-session-patch/system/lib64/libpocket_media_api32.so')
            if 'system/etc/pocket-runtime-boot.sh' in patch_files:
                boot=apk.read('assets/media-session-patch/system/etc/pocket-runtime-boot.sh').decode()
                report['extractorBootStart32']=('start_service pocket-mediaextractor\n' in boot and
                    boot.index('start_service pocket-mediaextractor\n')<boot.index('start_service pocket-framework\n'))
            if args.media_session_patch32 or args.media_session_lazy_probe32:report['lazyMediaProbeSha256']=hashlib.sha256(apk.read('assets/api32-lazy-media-probe')).hexdigest()
            if args.media_session_metrics_probe32:report['metricsProbeSha256']=hashlib.sha256(apk.read('assets/api32-metrics-probe')).hexdigest()
    (out/'build.json').write_text(json.dumps(report,indent=2)+'\n');print('API29_MANAGER_BUILD_PASS',json.dumps(report))


if __name__=='__main__':main()
