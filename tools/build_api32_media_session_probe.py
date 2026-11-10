#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the real MediaProvider session fixture, signed with the image's public test platform key."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from build_api32_guest_app import build_java_app
from build_boot import reserve_interpreter
from validate_native_loader import validate
from prepare_api32 import ROOT,CACHE

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ndk',type=Path,required=True);p.add_argument('--out',type=Path,default=CACHE/'media-session-app');p.add_argument('--external-proxy',action='store_true');p.add_argument('--external-paths',action='store_true');p.add_argument('--external-namespace',action='store_true');a=p.parse_args();a.external_proxy=a.external_proxy or a.external_paths or a.external_namespace;out=a.out.resolve()
    apk=build_java_app(ROOT/'runtime/tests/api32/mediasession',out,'dev.kreijstal.pocketmediasession32','media-session.apk')
    security=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/build/make/target/product/security'
    cert=security/'platform.x509.pem'
    certificate=subprocess.check_output(['openssl','x509','-in',str(cert),'-outform','DER'])
    assert hashlib.sha256(certificate).hexdigest()=='c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8'
    signer=['java','-cp',str(ROOT.parent/'helloworldandroid/build/apksigner.jar'),'com.android.apksigner.ApkSignerTool']
    subprocess.run(signer+['sign','--key',str(security/'platform.pk8'),'--cert',str(cert),'--out',str(apk),str(out/'unsigned.apk')],check=True)
    subprocess.run(signer+['verify','--min-sdk-version','32','--max-sdk-version','32',str(apk)],check=True)
    compiler=a.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang++'
    native=out/'pocket-media-client'
    extra=['-DPOCKET_EXTERNAL_PROXY32'] if a.external_proxy else []
    if a.external_paths:extra+=['-DPOCKET_EXTERNAL_PATH32','-DPOCKET_EXTERNAL_OPEN_TRACE']
    if a.external_namespace:extra+=['-DPOCKET_EXTERNAL_NAMESPACE32']
    subprocess.run([str(compiler),'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static-libstdc++','-pthread',*extra,'-Wl,-z,max-page-size=16384',str(ROOT/'runtime/tests/api32/media_session_client.cpp'),'-o',str(native)],check=True)
    reserve_interpreter(native)
    loader=out/'pocket-loader'
    subprocess.run([str(compiler.parent/'aarch64-linux-android28-clang'),'-O2','-g','-Wall','-Wextra','-Werror','-mno-outline-atomics','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-nostdlib','-static','-Wl,-T,runtime/loader/loader.ld,-z,max-page-size=16384',str(ROOT/'runtime/loader/loader.c'),str(ROOT/'runtime/loader/entry.S'),'-o',str(loader)],cwd=ROOT,check=True)
    validate(loader)
    metadata={'package':'dev.kreijstal.pocketmediasession32','minSdk':32,'targetSdk':32,'platformTestCertificateSha256':hashlib.sha256(certificate).hexdigest(),'scope':'Real guest MediaProvider session and metadata/private-FUSE denial contract; signature permission for binding, ordinary guest app UID; file data and mounted path integration remain unverified',
        'apkSha256':hashlib.sha256(apk.read_bytes()).hexdigest(),'nativeSha256':hashlib.sha256(native.read_bytes()).hexdigest(),'loaderSha256':hashlib.sha256(loader.read_bytes()).hexdigest(),'externalProxy':a.external_proxy,'externalPaths':a.external_paths}
    if a.external_proxy:
        jni=out/'libpocket_external32.so'
        subprocess.run([str(compiler.parent/'aarch64-linux-android28-clang'),'-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-pthread','-Wl,-z,max-page-size=16384',str(ROOT/'runtime/storage/native/external_fuse32.c'),'-o',str(jni)],check=True)
        metadata['externalJniSha256']=hashlib.sha256(jni.read_bytes()).hexdigest()
        metadata['scope']='Real MediaProvider file data through typed broker and host AppFuse, plus kernel thread identity/host-peer authentication; not production vold or app-path integration'
        if a.external_paths:metadata['scope']='Ordinary existing regular-file openat through loader/AppFiles/host AppFuse, real provider statfs and cross-app denial; directory metadata, create/mutation, private lower views, cache coherence and production vold integration remain pending'
        if a.external_namespace:
            metadata['externalNamespace']=True
            metadata['scope']+='; trusted native broker additionally exercises live MediaProvider namespace operations; ordinary-app mutation routing and production mounted MediaStore remain unverified'
    (out/'build.json').write_text(json.dumps(metadata,indent=2)+'\n');print('API32_MEDIA_SESSION_BUILD_PASS',json.dumps(metadata))
if __name__=='__main__':main()
