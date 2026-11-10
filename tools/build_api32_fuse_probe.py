#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build an isolated probe of packaged12L libfuse over a userspace channel."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import posixpath
import re
import shutil
import struct
import subprocess
import zipfile
from target import ROOT
from build_boot import reserve_interpreter
from validate_native_loader import validate


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--external-proxy',action='store_true',help='Test the host AppFuse data plane instead of the libfuse protocol')
    p.add_argument('--namespace-rpc',action='store_true',help='Test real-file-backed namespace broker/client over pinned12L Bionic, without publishing a guest volume')
    p.add_argument('--directory-rpc',action='store_true',help='Include logical directory descriptors, cursor/copy acknowledgement and kernel reference lifetime checks')
    p.add_argument('--volume-session',action='store_true',help='Native session broker and real host AppFuse adapter; filesystem model, not MediaProvider policy')
    args=p.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    args.namespace_rpc=args.namespace_rpc or args.directory_rpc
    if args.external_proxy and args.namespace_rpc:p.error('--external-proxy and --namespace-rpc are separate fixtures')
    if args.volume_session and (args.external_proxy or args.namespace_rpc):p.error('--volume-session is a separate fixture')
    reference=ROOT/'out/api32/emulated-reference/libfuse';pins=json.loads((reference/'sources.json').read_text())
    for name,pin in pins['files'].items():assert hashlib.sha256((reference/name).read_bytes()).hexdigest()==pin['sha256'],name
    llvm=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    def run(*argv):subprocess.run(list(map(str,argv)),cwd=ROOT,check=True)
    if args.namespace_rpc:
        run(llvm/'aarch64-linux-android28-clang','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-c',ROOT/'runtime/tests/external_namespace_runtime_host.c','-o',out/'namespace-runtime.o')
    native=ROOT/('runtime/tests/api32/volume_session.cpp' if args.volume_session else 'runtime/tests/api32/external_namespace_rpc.cpp' if args.namespace_rpc else 'runtime/tests/api32/fuse_proxy.cpp' if args.external_proxy else 'runtime/tests/api32/fuse_stock.cpp')
    extra=[out/'namespace-runtime.o'] if args.namespace_rpc else []
    if args.directory_rpc:extra+=['-DPOCKET_DIRECTORY_RPC32']
    run(llvm/'aarch64-linux-android28-clang++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-static-libstdc++','-pthread',
        '-I'+str(reference/'include'),'-Wl,-z,max-page-size=16384',native,*extra,'-ldl','-o',out/'fuse-stock')
    run(llvm/'aarch64-linux-android28-clang','-O2','-g','-Wall','-Wextra','-Werror','-mno-outline-atomics','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic',
        '-nostdlib','-static','-Wl,-T,runtime/loader/loader.ld,-z,max-page-size=16384',ROOT/'runtime/loader/loader.c',ROOT/'runtime/loader/entry.S','-o',out/'pocket-loader')
    validate(out/'pocket-loader');reserve_interpreter(out/'fuse-stock')
    package='dev.kreijstal.aosppocket.'+('api32volumetest' if args.volume_session else 'api32namespacerpctest' if args.namespace_rpc else 'api32externaltest' if args.external_proxy else 'api32fusetest')
    activity='VolumeSessionActivity' if args.volume_session else 'ExternalProxyActivity' if args.external_proxy else 'FuseTransportActivity'
    root='/data/user/0/'+package+'/files/'+('v' if args.volume_session else 'n' if args.namespace_rpc else 'fuse-root')
    raw=bytearray((out/'fuse-stock').read_bytes());phoff=struct.unpack_from('<Q',raw,32)[0];stride,count=struct.unpack_from('<HH',raw,54)
    for i in range(count):
        ph=struct.unpack_from('<IIQQQQQQ',raw,phoff+i*stride)
        if ph[0]==3:
            name=(root+'/system/bin/pocket-loader').encode()+b'\0';assert len(name)<=ph[5];raw[ph[2]:ph[2]+ph[5]]=name.ljust(ph[5],b'\0');break
    else:raise RuntimeError('Probe interpreter missing')
    files={'system/bin/fuse-stock':bytes(raw),'system/bin/pocket-loader':(out/'pocket-loader').read_bytes()}
    if args.volume_session:
        files['system/bin/pocket-core-api32']=files.pop('system/bin/fuse-stock')
        files['system/etc/pocket-external-volume32']=b'rootless-v1\n'
    files['system-data/media/0/lower-view']=b'pocket-lower-view32'
    with zipfile.ZipFile(args.image) as image:
        def read(name):
            for _ in range(12):
                entry=image.getinfo(name);data=image.read(name)
                if (entry.external_attr>>16)&0o170000!=0o120000:return data
                target=data.decode();name=posixpath.normpath(target.lstrip('/') if target.startswith('/') else posixpath.join(posixpath.dirname(name),target))
                assert not name.startswith('../')
            raise RuntimeError('Image symlink loop')
        apk=read('apex/com.android.mediaprovider/priv-app/MediaProvider/MediaProvider.apk')
        assert hashlib.sha256(apk).hexdigest()=='914df3a1bb6942aaced446c3d7aaf0650cc2ab068f5a712d3bd8035ffdbfe523'
        with zipfile.ZipFile(io.BytesIO(apk)) as module:files['system/lib64/libfuse.so']=module.read('lib/arm64-v8a/libfuse.so')
        assert hashlib.sha256(files['system/lib64/libfuse.so']).hexdigest()=='3e97c7bea3a4479ff5643a14ccd37c86f9682ba7a2e29b9dbe853f4da350dd54'
        files['system/bin/linker64']=read('system/bin/linker64')
        queue=['system/bin/pocket-core-api32' if args.volume_session else 'system/bin/fuse-stock','system/lib64/libfuse.so'];temp=out/'dependency.so'
        while queue:
            current=queue.pop();temp.write_bytes(files[current]);dynamic=subprocess.check_output(['readelf','-dW',temp],text=True)
            for name in re.findall(r'Shared library: \[([^]]+)\]',dynamic):
                path='system/lib64/'+name
                if path not in files:files[path]=read(path);queue.append(path)
    tool=ROOT.parent/'helloworldandroid';android=tool/'vendor/android-platforms/android-36/android.jar'
    for name in ['classes','dex','tools']:
        shutil.rmtree(out/name,ignore_errors=True);(out/name).mkdir()
    sources=[ROOT/'runtime/app/src/dev/kreijstal/aosppocket/runtime/NativeLoader.java',ROOT/('runtime/tests/api32/fuse/'+activity+'.java')]
    if args.volume_session:
        sources += [ROOT/'runtime/app/src/dev/kreijstal/aosppocket/runtime'/name for name in ['ExternalFiles32.java','ExternalVolume32.java','HostDns.java','HostDnsPacket.java']]
        sources += [ROOT/'runtime/app/src/dev/kreijstal/aosppocket/bridge/HostWire.java']
    if args.external_proxy or args.volume_session:
        sources.append(ROOT/'runtime/app/src/dev/kreijstal/aosppocket/runtime/ExternalFuse32.java')
        run(llvm/'aarch64-linux-android28-clang','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-pthread','-Wl,-z,max-page-size=16384',ROOT/'runtime/storage/native/external_fuse32.c','-llog','-o',out/'libpocket_external32.so')
    run('javac','--release','8','-cp',android,'-d',out/'classes',*sources)
    run('jar','cf',out/'classes.jar','-C',out/'classes','.')
    run('java','-cp',ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/prebuilts/r8/r8.jar','com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',out/'dex',out/'classes.jar')
    run('javac','--release','8','-cp',tool/'build/tools','-d',out/'tools',ROOT/'runtime/tools/PocketManifest.java')
    manifest=(ROOT/'runtime/tests/api32/fuse/AndroidManifest.xml').read_text().replace('dev.kreijstal.aosppocket.api32fusetest',package).replace('FuseTransportActivity',activity)
    (out/'manifest.xml').write_text(manifest)
    run('java','-cp',str(tool/'build/tools')+':'+str(out/'tools'),'PocketManifest',out/'manifest.xml',out/'AndroidManifest.xml','28','28','1','0.1')
    with zipfile.ZipFile(out/'unsigned.apk','w',zipfile.ZIP_DEFLATED) as z:
        for name,path in [('AndroidManifest.xml',out/'AndroidManifest.xml'),('classes.dex',out/'dex/classes.dex')]:z.write(path,name)
        for name,data in sorted(files.items()):z.writestr('assets/root/'+name,data)
        z.writestr('assets/fuse-files.json',json.dumps({name:hashlib.sha256(data).hexdigest() for name,data in files.items()},sort_keys=True))
        z.writestr('assets/fuse-probe-kind.json',json.dumps({'directoryLibcChild':args.directory_rpc}))
        if args.external_proxy or args.volume_session:z.write(out/'libpocket_external32.so','lib/arm64-v8a/libpocket_external32.so')
    signer=['java','-cp',str(tool/'build/apksigner.jar'),'com.android.apksigner.ApkSignerTool'];target=out/'api32-fuse-probe.apk'
    run(*signer,'sign','--ks',ROOT/'out/debug.keystore','--ks-pass','pass:android','--key-pass','pass:android','--out',target,out/'unsigned.apk')
    run(*signer,'verify',target)
    result={'apk':str(target),'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'bytes':target.stat().st_size,'imageSha256':hashlib.file_digest(args.image.open('rb'),'sha256').hexdigest(),'assets':{n:hashlib.sha256(b).hexdigest() for n,b in sorted(files.items())},'scope':'Packaged12L libfuse/Bionic native protocol transport; not framework MediaProvider policy or mounted volume'}
    result.update(package=package,activity=package+'/dev.kreijstal.aosppocket.runtime.'+activity,externalProxy=args.external_proxy)
    if args.namespace_rpc:
        result['namespaceRpc']=True
        result['scope']='12L Bionic/loader namespace broker/client against a real-file-backed FUSE peer; no real MediaProvider, host AppFuse proxy mount, production mounted volume or scoped-policy claim'
    if args.directory_rpc:result.update(directoryRpc=True,directoryLibcChild=True)
    if args.external_proxy:
        result['scope']='Real host AppFuse data plane in guest Bionic/loader; test callbacks, not MediaProvider policy or mounted volume'
        result['externalJniSha256']=hashlib.file_digest((out/'libpocket_external32.so').open('rb'),'sha256').hexdigest()
    if args.volume_session:
        result.update(volumeSession=True,scope='Native session/namespace broker and real host AppFuse adapter with ordinary12L Bionic clients; filesystem model and callback only, not real MediaProvider policy or production framework volume lifecycle')
        result['externalJniSha256']=hashlib.file_digest((out/'libpocket_external32.so').open('rb'),'sha256').hexdigest()
    (out/'build.json').write_text(json.dumps(result,indent=2)+'\n');print('FUSE_PROBE32_BUILD_PASS',json.dumps({k:result[k] for k in ['apk','sha256','bytes']}))


if __name__=='__main__':main()
