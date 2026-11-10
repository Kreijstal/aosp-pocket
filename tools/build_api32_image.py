#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package the normal 12L runtime as a separate downloadable Manager image."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import urllib.parse
import zipfile
import zlib
from prepare_api32 import ROOT,CACHE
from build_boot import reserve_interpreter
from build_api32_manager_tools import build as build_manager_tools
from build_api32_guest_app import build_guest_bridge
from build_api32_runtime_tools import build as build_runtime_tools
from build_api32_bootstrap import build as build_bootstrap
from api32_runtime_boot import NORMAL_CORE_SERVICES,eager_camera_boot,shared_gatekeeper_frontend,shared_installd_frontend
from build_api32_core import build_core
from build_api32_crypto import build_crypto
from build_api32_tombstoned import build_tombstoned
from build_api32_network import build_network
from validate_native_loader import validate as validate_loader
from api32_device_profile import device_profile_payloads

def build_loader(ndk,out):
    target=out/'pocket-loader';cc=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    command=[cc,'-O2','-g','-Wall','-Wextra','-Werror','-mno-outline-atomics','-ffreestanding',
        '-fno-builtin','-fno-stack-protector','-fno-pic','-nostdlib','-static',
        '-Wl,-T,'+str(ROOT/'runtime/loader/loader.ld')+',-z,max-page-size=16384',
        ROOT/'runtime/loader/loader.c',ROOT/'runtime/loader/entry.S','-o',target]
    subprocess.run(list(map(str,command)),check=True,cwd=ROOT);validate_loader(target)
    assert target.read_bytes().count(b'PCKELF01'+bytes(3072))==1
    inputs=[p for p in (ROOT/'runtime/loader').rglob('*') if p.is_file() and p.suffix in {'.c','.h','.S','.ld'}]
    inputs += [ROOT/'tools/build_api32_image.py',ROOT/'tools/validate_native_loader.py']
    receipt={'sdk':32,'command':list(map(str,command)),
        'sourceSha256':{p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)},
        'ndkPropertiesSha256':hashlib.sha256((ndk/'source.properties').read_bytes()).hexdigest(),
        'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'bytes':target.stat().st_size,'runtimeValidated':False}
    (out/'loader-build.json').write_text(json.dumps(receipt,indent=2)+'\n')
    return target

PARTITIONS=('system','system_ext','vendor','product','apex','odm','oem')
REQUIRED=('system/bin/pocket-loader','system/bin/pocket-init-api32','system/bin/pocket-core-api32',
    'system/bin/pocket-crypto-api32','system/etc/init/pocket-runtime.rc',
    'system/etc/pocket-runtime-boot.sh','system/etc/pocket-runtime-env.sh',
    'system/framework/pocket-zygote.jar','system/framework/pocket-tools.jar',
    'system/bin/mediaextractor','system/bin/mediaserver','system/bin/mediaswcodec','system/bin/adbd',
    'system/lib64/libpocket_media_api32.so','system/lib64/libpocket_camera_api32.so',
    'system/lib64/libpocket_adbd_api32.so','system/etc/permissions/pocket-camera.xml',
    'system/bin/abb','system/bin/am','system/bin/cmd','system/bin/dumpsys','system/bin/input',
    'system/bin/pm','system/bin/settings','system/bin/wm','vendor/etc/external_camera_config.xml',
    'system/framework/pocket-microg.jar','system/etc/pocket-microg/google-cert.hex','system/etc/sysconfig/pocket-microg.xml',
    'apex/com.android.art/javalib/arm64/boot.art','apex/com.android.runtime/lib64/bionic/libc.so',
    'product/overlay/PocketMemory/PocketMemory.apk','system/usr/keychars/Virtual.kcm',
    '.pocket-readonly-system','bootstrap-linkerconfig')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fixture-root',type=Path,default=CACHE/'android-probe/root')
    p.add_argument('--out',type=Path,default=CACHE/'image')
    p.add_argument('--version',default='0.78.0')
    p.add_argument('--url',required=True)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--probe',action='store_true')
    p.add_argument('--external-volume',action='store_true',help='Select the rootless emulated-volume session backend; requires the matching Manager host adapter')
    p.add_argument('--installd-in-core',action='store_true',help='Build and select the complete pinned installd in the UID-0 core')
    p.add_argument('--mediaprovider-dir',type=Path,help='Use the verified source-built private-view MediaProvider package receipt')
    args=p.parse_args();source=args.fixture_root.resolve();out=args.out.resolve()
    if args.mediaprovider_dir and not args.external_volume:p.error('The private provider view requires --external-volume')
    url=urllib.parse.urlsplit(args.url)
    if not (url.scheme=='https' or args.probe and url.scheme=='http' and url.hostname=='127.0.0.1'):
        p.error('Image URL must use HTTPS (loopback HTTP requires --probe)')
    if not url.hostname or url.username or url.password or url.fragment:p.error('Invalid image URL')
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+)*',args.version):p.error('Invalid image version')
    if out.is_relative_to(source):p.error('Output must be outside the source image')
    build=json.loads((source.parent/'build.json').read_text())
    if build.get('sdk')!=32 or build.get('coreServicesLayout')!=2 or not build.get('runtimeBootRequested') or not build.get('runtimeSystemServerBridge') or not build.get('runtimeMediaServices') or not build.get('runtimeMediaMetricsInCore') or not build.get('runtimeMicrogSupport'):
        p.error('Use the complete API32 layout-2 normal-runtime fixture with its embedded bridge, shared media metrics, media and microG support')
    if not build.get('runtimeKernelChildOwnership') or b'API32_INIT_SUBREAPER_READY kernel_child_ownership=1' not in (source/'system/bin/pocket-init-api32').read_bytes():
        p.error('Rebuild the normal runtime with kernel descendant ownership in native init')
    for name,digest in build['files'].items():
        file=source/name
        if hashlib.file_digest(file.open('rb'),'sha256').hexdigest()!=digest:raise ValueError('Fixture input changed: '+name)
    if 'ro.build.version.sdk=32\n' not in (source/'system/build.prop').read_text():raise ValueError('Expected SDK32')
    for name in REQUIRED:
        if not (source/name).is_file():raise ValueError('Missing runtime input: '+name)
    out.mkdir(parents=True,exist_ok=True)
    rc=(source/'system/etc/init/pocket-runtime.rc').read_text()
    services=re.findall(r'^service (\S+) ',rc,re.M)
    if len(services)!=len(set(services)) or set(services)!=NORMAL_CORE_SERVICES:raise ValueError('Expected the supervised layout-2 service set with the embedded bridge')
    if rc.count('    setenv POCKET_API32_SYSTEM_SERVER_BRIDGE 1\n')!=1:raise ValueError('Embedded bridge is not enabled exactly once')
    if rc.count('    setenv POCKET_API32_METRICS_IN_CORE 1\n')!=1:raise ValueError('Stock media metrics must be hosted by the native core')
    if 'pocket-core-test-api32' in rc:raise ValueError('Test registry cannot be a Manager service')
    # The scheduling experiment is independently selected by the test fixture.
    # Manager images retain the established default until startup is reliable.
    rc=rc.replace(':/system/lib64/libpocket_renderer_workers_api32.so','')
    boot=(source/'system/etc/pocket-runtime-boot.sh').read_text()
    boot=boot.replace('/system/bin/pocket-boot-probe-api32','/system/bin/pocket-runtime-prepare-api32')
    boot=boot.replace('/system/bin/pocket-framework-probe-api32','/system/bin/pocket-runtime-wait-api32')
    boot=eager_camera_boot(boot)
    generated={'system/etc/init/pocket-runtime.rc':rc.encode(),
        'system/etc/pocket-runtime-boot.sh':boot.encode(),
        'system/bin/pocket-runtime-wait-api32':(source/'system/bin/pocket-framework-probe-api32').read_bytes()}
    device_profile,device_payloads,omitted_package_dirs=device_profile_payloads()
    generated.update(device_payloads)
    generated['system/etc/pocket-manager-control32']=b'input-control-v1\n'
    generated['system/etc/pocket-app-readiness32']=b'boot-broadcasts-and-volume-v1\n'
    # Bootstrap participates in the current loader's internal lower-view API;
    # do not copy an older fixture executable after changing that contract.
    preparation=out/'pocket-runtime-prepare-api32';cc=args.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang'
    subprocess.run(list(map(str,[cc,'-O2','-Wall','-Wextra','-Werror',ROOT/'runtime/direct/boot_probe.c','-ldl','-Wl,-z,max-page-size=16384','-o',preparation])),check=True)
    if not reserve_interpreter(preparation):raise ValueError('Runtime preparation needs a loader interpreter reservation')
    generated['system/bin/pocket-runtime-prepare-api32']=preparation.read_bytes()
    generated['system/framework/pocket-manager.jar']=build_manager_tools(out/'manager-tools').read_bytes()
    generated['system/framework/pocket-tools.jar']=build_runtime_tools(out/'runtime-tools').read_bytes()
    generated['system/framework/pocket-zygote.jar']=build_bootstrap(out/'bootstrap').read_bytes()
    if args.external_volume:generated['system/etc/pocket-external-volume32']=b'rootless-v1\n'
    provider=None
    if args.mediaprovider_dir:
        from package_api32_mediaprovider import load_package
        provider=load_package(args.mediaprovider_dir.resolve())
        generated['apex/com.android.mediaprovider/priv-app/MediaProvider/MediaProvider.apk']=(args.mediaprovider_dir/'MediaProvider.apk').read_bytes()
    core_dir=build_core(args.ndk.resolve(),reuse_generated=True,out=out/'native-core',installd=args.installd_in_core)
    core_info=json.loads((core_dir/'build.json').read_text())
    if hashlib.file_digest((source/'system/lib64/libpocket_hidl_api32.so').open('rb'),'sha256').hexdigest()!=core_info['hardwareLibrarySha256']:
        raise ValueError('Runtime core and fixture hardware library differ')
    core=out/'pocket-core-runtime-api32';shutil.copyfile(core_dir/'pocket-core-api32',core)
    if not reserve_interpreter(core):raise ValueError('Runtime core needs a loader interpreter reservation')
    generated['system/bin/pocket-core-api32']=core.read_bytes()
    # Build the frontend and its matching HAL host from the current pinned
    # sources. The immutable fixture may still use standalone gatekeeperd.
    crypto_dir=build_crypto(args.ndk.resolve(),reuse_generated=True,frontend=True,out=out/'native-crypto')
    crypto=out/'pocket-crypto-runtime-api32';shutil.copyfile(crypto_dir/'pocket-crypto-api32',crypto)
    if not reserve_interpreter(crypto):raise ValueError('Runtime crypto needs a loader interpreter reservation')
    generated['system/bin/pocket-crypto-api32']=crypto.read_bytes()
    rc,boot=shared_gatekeeper_frontend(rc,boot,generated['system/bin/pocket-crypto-api32'])
    if args.installd_in_core:
        rc,boot=shared_installd_frontend(rc,boot,generated['system/bin/pocket-core-api32'])
    if set(re.findall(r'^service (\S+) ',rc,re.M))!=NORMAL_CORE_SERVICES-{'gatekeeperd'}-({'pocket-installd'} if args.installd_in_core else set()):
        raise ValueError('Unexpected shared-frontend runtime service set')
    generated['system/etc/init/pocket-runtime.rc']=rc.encode()
    generated['system/etc/pocket-runtime-boot.sh']=boot.encode()
    crypto_info=json.loads((crypto_dir/'build.json').read_text())
    # The frozen fixture is an immutable baseline, not a source freshness
    # cache. Rebuild netd so RPC/error-path fixes cannot silently ship with
    # an older daemon while Manager embeds the newly built network client.
    network_dir=build_network(args.ndk.resolve())
    network=out/'pocket-netd-runtime-api32';shutil.copyfile(network_dir/'pocket-netd-api32',network)
    if not reserve_interpreter(network):raise ValueError('Runtime netd needs a loader interpreter reservation')
    generated['system/bin/pocket-netd-api32']=network.read_bytes()
    network_info={'sha256':hashlib.sha256(network.read_bytes()).hexdigest(),
        'probeSha256':hashlib.file_digest((network_dir/'pocket-network-probe-api32').open('rb'),'sha256').hexdigest(),
        'sources':{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in [
            'tools/build_api32_image.py','tools/build_api32_network.py','runtime/api32/network_service.cpp',
            'runtime/api32/network_probe.cpp','runtime/api32/received_rights.h','runtime/api32/binder_listeners.h',
            'runtime/api32/service_output.h','runtime/direct/kernel_identity.h','runtime/loader/network_policy.h',
            'runtime/api28/binder_identity.h']}}
    generated['system/bin/pocket-loader']=build_loader(args.ndk.resolve(),out).read_bytes()
    # This provider is a production package-lifecycle observer, protected by
    # DUMP and a system-UID check. It contains no test activity or boot probes.
    generated['system/app/PocketServices/PocketServices.apk']=build_guest_bridge().read_bytes()
    # Production adapters use the immutable-runtime marker, rather than the
    # isolated fixture's marker. Rebuild instead of altering a verified fixture.
    generated['system/lib64/libpocket_tombstoned_api32.so']=(build_tombstoned(args.ndk.resolve())/'libpocket_tombstoned_api32.so').read_bytes()
    generated['system/bin/pocket-manager-api32']=b'''#!/system/bin/sh
set -eu
. /system/etc/pocket-runtime-env.sh
export LD_PRELOAD=/system/lib64/libpocket_binder_api32.so
exec /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-manager.jar /system/bin dev.kreijstal.aosppocket.guest.ManagerTools32 "$@"
'''
    with tempfile.TemporaryDirectory(prefix='pocket-api32-getprop-') as temporary:
        executable=Path(temporary)/'getprop'
        cc=args.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang'
        subprocess.run(list(map(str,[cc,'-std=c11','-D_GNU_SOURCE','-O2','-Wall','-Wextra','-Werror','-fPIE','-pie',
            ROOT/'runtime/api32/getprop.c','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',executable])),check=True)
        if not reserve_interpreter(executable):raise ValueError('getprop needs a loader interpreter reservation')
        generated['system/bin/getprop']=executable.read_bytes()
    excluded={'system/etc/init/pocket-api32.rc','system/etc/init/pocket-native-test.rc',
        'system/etc/pocket-runtime-validate.sh','system/etc/pocket-api32-smoke.apk',
        'system/app/PocketServices/PocketServices.apk','system/bin/pocket-core-test-api32',
        'system/bin/pocket-boot-probe-api32','system/bin/pocket-framework-probe-api32'}
    files={}
    for top in PARTITIONS:
        for path in sorted((source/top).rglob('*')):
            if path.is_symlink():raise ValueError('Symlink in immutable source: '+str(path))
            if not path.is_file():continue
            name=path.relative_to(source).as_posix()
            if name in excluded or name.startswith('system/etc/init/reject-'):continue
            # Retain the established runtime profile: the emulator search app
            # is absent from the existing Manager image as well.
            if name.startswith('product/app/QuickSearchBox/'):continue
            if name.startswith(omitted_package_dirs):continue
            if name.startswith('system/bin/pocket-') and ('-probe-' in name or '-test-' in name or '-launcher-' in name):continue
            files[name]=path
    for name in ['.pocket-readonly-system','bootstrap-linkerconfig']:files[name]=source/name
    files.update(generated)
    target=out/('pocket-api32-arm64-'+args.version+'.zip');entries={}
    with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as archive:
        for name,item in sorted(files.items()):
            data=item.read_bytes() if isinstance(item,Path) else item
            mode=0o755 if data.startswith((b'\x7fELF',b'#!')) else 0o644
            member=zipfile.ZipInfo(name,(1980,1,1,0,0,0));member.compress_type=zipfile.ZIP_DEFLATED
            member.external_attr=(0o100000|mode)<<16;archive.writestr(member,data,compresslevel=1)
            entries[name]={'size':len(data),'crc32':zlib.crc32(data),'sha256':hashlib.sha256(data).hexdigest(),'mode':mode}
        manifest={'schema':1,'profile':'api32-direct','sdk':32,'abi':'arm64-v8a','runtime':1,'version':args.version,
            'experimental':True,'service_layout':2,'boot_protocol':'api32-v1','storage':'app-private',
            'child_supervision':'kernel-subreaper','crypto_frontend':'same-uid-v1','entries':entries}
        if args.external_volume:manifest['external_volume']='rootless-v1'
        if args.installd_in_core:manifest['installd_frontend']='same-uid-v1'
        if provider:manifest['private_dir_policy']='provider-fuse-v1'
        raw=json.dumps(manifest,sort_keys=True,separators=(',',':')).encode()+b'\n'
        member=zipfile.ZipInfo('pocket-image.json',(1980,1,1,0,0,0));member.compress_type=zipfile.ZIP_DEFLATED
        archive.writestr(member,raw,compresslevel=1)
    image={key:value for key,value in manifest.items() if key!='entries'};image.pop('schema')
    image.update(url=args.url,bytes=target.stat().st_size,unpacked_bytes=sum(v['size'] for v in entries.values()),
        sha256=hashlib.file_digest(target.open('rb'),'sha256').hexdigest(),manifest_sha256=hashlib.sha256(raw).hexdigest(),manifest=manifest)
    (out/'pocket-images.json').write_text(json.dumps({'schema':1,'images':[image]},sort_keys=True,separators=(',',':'))+'\n')
    (out/'image-inputs.lock.json').write_text(json.dumps({'sources':json.loads((ROOT/'sources.lock.json').read_text())['guest_api32'],
        'imageSha256':image['sha256'],'files':len(entries),'sourceFixture':str(source),'productionManagerVerified':False,
        'freshImageBootVerified':False,'rendererWorkerScheduling':False,'rootlessExternalVolumeSelected':args.external_volume,
        'mediaProviderNativePackage':provider,'networkSourceBuild':network_info,'cryptoSourceBuild':crypto_info,
        'nativeCoreSourceBuild':core_info,
        'loaderSourceBuild':json.loads((out/'loader-build.json').read_text()),
        'deviceProfile':device_profile},indent=2)+'\n')
    print('API32_IMAGE_BUILD_PASS',target,'files='+str(len(entries)),'bytes='+str(image['bytes']),'sha256='+image['sha256'])

if __name__=='__main__':main()
