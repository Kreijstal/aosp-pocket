#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build two ordinary registered apps for real mounted-volume/private-policy tests."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile
from build_api32_guest_app import build_java_app
from prepare_api32 import ROOT


def build(out,lifecycle=False,reset=False,crash=False,ndk=None):
    if reset and not lifecycle:raise ValueError('Framework reset requires lifecycle validation')
    if crash and lifecycle:raise ValueError('Abrupt Manager death requires a distinct lifecycle')
    out=out.resolve();assert not (out/'build.json').exists(),'Use a fresh output for a changed fixture'
    out.mkdir(parents=True,exist_ok=True)
    if ndk is None:raise ValueError('An explicit NDK is required for the ordinary namespace syscall fixture')
    compiler=ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android32-clang'
    native=out/'libpocket_namespace_probe32.so'
    command=[str(compiler),'-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-Wl,-z,max-page-size=16384',
        str(ROOT/'runtime/tests/api32/externalvolume/namespace.c'),'-o',str(native)]
    subprocess.run(command,check=True)
    pins={}
    for folder in ['externalvolume','externalvolumeowner']:
        for p in (ROOT/'runtime/tests/api32'/folder).iterdir():
            if p.is_file():pins[p.relative_to(ROOT).as_posix()]=hashlib.sha256(p.read_bytes()).hexdigest()
    for name in ['tools/build_api32_volume_app.py','tools/build_api32_guest_app.py','runtime/tools/PocketManifest.java']:
        pins[name]=hashlib.sha256((ROOT/name).read_bytes()).hexdigest()
    specs={}
    for key,folder,package,filename in [
        ('owner','externalvolumeowner','dev.kreijstal.pocketvolumeowner32','PocketVolumeOwner32-v2-api32.apks'),
        ('app','externalvolume','dev.kreijstal.pocketvolume32','PocketVolume32-namespace-api32.apks')]:
        if key=='owner' and crash:filename='PocketVolumeOwner32-crash-api32.apks'
        libraries={'lib/arm64-v8a/libpocket_namespace_probe32.so':native} if key=='app' else None
        apk=build_java_app(ROOT/'runtime/tests/api32'/folder,out/key,package,'base.apk',libraries)
        signer=['java','-cp',str(ROOT.parent/'helloworldandroid/build/apksigner.jar'),'com.android.apksigner.ApkSignerTool']
        verification=subprocess.check_output(signer+['verify','--print-certs','--min-sdk-version','32','--max-sdk-version','32',str(apk)],text=True)
        cert=re.findall(r'Signer #\d+ certificate SHA-256 digest: ([a-f0-9]{64})',verification);assert len(cert)==1
        raw=apk.read_bytes();archive=out/filename
        with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_STORED) as z:
            info=zipfile.ZipInfo('base.apk',(1980,1,1,0,0,0));info.external_attr=0o100644<<16;z.writestr(info,raw)
        specs[key]={'package':package,'version':'0.1','versionCode':1,'minSdk':32,'targetSdk':32,
            'certificateSha256':cert[0],'apkModified':False,'url':'http://127.0.0.1:8794/'+filename,
            'archiveBytes':archive.stat().st_size,'archiveSha256':hashlib.file_digest(archive.open('rb'),'sha256').hexdigest(),
            'files':[{'name':'base.apk','bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}]}
    spec=dict(specs['app'],proof='external-volume-v1',storageOwner=specs['owner'],requireNamespaceSyscalls=True)
    if lifecycle:spec['volumeLifecycle']=True
    if reset:spec['volumeReset']=True
    if crash:spec.update(managerCrashRecovery=True,requireKeyCanary=True)
    (out/'app-spec.json').write_text(json.dumps(spec,indent=2)+'\n')
    (out/'build.json').write_text(json.dumps({'sourceSha256':pins,'apps':specs,
        'nativeProbe':{'command':command,'sha256':hashlib.file_digest(native.open('rb'),'sha256').hexdigest()},
        'scope':'Real MediaProvider operations, cross-UID private-directory denials and in-process namespace syscalls; runtime validation pending'},indent=2)+'\n')
    print('API32_VOLUME_APP_BUILD_PASS',out/'app-spec.json')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--ndk',type=Path,required=True,help='NDK used to compile the ordinary ARM64 namespace JNI fixture')
    parser.add_argument('--lifecycle',action='store_true',help='Also require actual framework unmount/remount and fresh ordinary workload')
    parser.add_argument('--reset',action='store_true',help='Require actual framework reset and callbacks instead of separate unmount/mount requests')
    parser.add_argument('--manager-crash',action='store_true',help='Require actual outer Manager SIGKILL and retained files/Keystore across recovery')
    args=parser.parse_args()
    if args.reset and not args.lifecycle:parser.error('--reset requires --lifecycle')
    if args.manager_crash and args.lifecycle:parser.error('--manager-crash requires a separate run from --lifecycle')
    build(args.out,args.lifecycle,args.reset,args.manager_crash,args.ndk)
