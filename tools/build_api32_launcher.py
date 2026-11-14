#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Backport the upstream Launcher recents null guard with its original AOSP signer."""
import argparse
import base64
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import ssl
import subprocess
import urllib.request
import zipfile
from prepare_api32 import ROOT, CACHE

ORIGINAL_SHA='2204aa1c0497881fb83a8b298eaa83c2f4d4cc6b1674c919cfd895d32178b31d'
CERTIFICATE_SHA='a40da80a59d170caa950cf15c18c454d47a39b26989d8b640ecd745ba71bf5dc'
UPSTREAM='https://android.googlesource.com/platform/packages/apps/Launcher3/+/f1575e527c/quickstep/src/com/android/quickstep/SystemUiProxy.java'
PUBLIC_SECURITY={
    'testkey.x509.pem':'a4384ba815b9499a5ce349b4e33c1755278873fe2eac150a068823f526e6dbde',
    'testkey.pk8':'495675d32e89a149d5abe191f4e9c0e218b9068714e9b53a7c91e164a0741a23',
}
SDK_JARS={
    'com/android/tools/smali/smali-baksmali/3.0.3/smali-baksmali-3.0.3.jar':'ffd6f3fbba2453440e4c04add42b9f903d84151b5b71f7e6bbb181d8096397a4',
    'com/android/tools/smali/smali-util/3.0.3/smali-util-3.0.3.jar':'8078a04709f42072a240a6fe7666b71755324745c505b39f86ededfd9c2b90c4',
    'com/android/tools/smali/smali-dexlib2/3.0.3/smali-dexlib2-3.0.3.jar':'11cc5e7b75b23feb65e41e28e5c225b4a77dbf7210d6cedd4bdb3f04c1f6ea24',
    'com/google/guava/guava/32.0.1-jre/guava-32.0.1-jre.jar':'bd7fa227591fb8509677d0d1122cf95158f3b8a9f45653f58281d879f6dc48c5',
    'com/google/guava/failureaccess/1.0.1/failureaccess-1.0.1.jar':'a171ee4c734dd2da837e4b16be9df4661afab72a41adaf31eb84dfdaf936ca26',
    'com/beust/jcommander/1.78/jcommander-1.78.jar':'7891debb84b5f83e9bd57593ebece3399abbe0fd938cf306b3534c57913b9615',
}


def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def run(command):
    command=list(map(str,command));subprocess.run(command,check=True);return command


def signature(name):
    return re.fullmatch(r'META-INF/(MANIFEST\.MF|[^/]+\.(SF|RSA|DSA|EC))',name,re.I) is not None


def public_security(directory):
    directory.mkdir(parents=True,exist_ok=True);pins={}
    for name,expected in PUBLIC_SECURITY.items():
        target=directory/name
        url='https://android.googlesource.com/platform/build/+/refs/tags/android-12.1.0_r27/target/product/security/'+name+'?format=TEXT'
        assert not target.is_symlink(),'Public development key cache redirects'
        if not target.exists():
            with urllib.request.urlopen(url,timeout=30) as response:raw=base64.b64decode(response.read(16385))
            assert hashlib.sha256(raw).hexdigest()==expected,'Pinned public AOSP key changed'
            with os.fdopen(os.open(target,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'wb') as output:
                output.write(raw);output.flush();os.fsync(output.fileno())
        assert digest(target)==expected,'Public AOSP key cache differs'
        pins[name]={'url':url,'sha256':expected}
    der=ssl.PEM_cert_to_DER_cert((directory/'testkey.x509.pem').read_text())
    assert hashlib.sha256(der).hexdigest()==CERTIFICATE_SHA
    return pins


def verify_disassembly(original,patched):
    before={p.relative_to(original).as_posix():p.read_text() for p in original.rglob('*.smali')}
    after={p.relative_to(patched).as_posix():p.read_text() for p in patched.rglob('*.smali')}
    assert before.keys()==after.keys() and len(before)==5181
    normalized=[];changed=[]
    for name,text in before.items():
        if text==after[name]:continue
        default=lambda s:re.sub(r'(?m)^(\.field [^\n]+:Z) = false$',r'\1',s)
        if default(text)==default(after[name]):normalized.append(name)
        else:changed.append(name)
    assert changed==['com/android/quickstep/SystemUiProxy.smali'],changed
    assert sorted(normalized)==['com/android/launcher3/BuildConfig.smali',
        'com/android/launcher3/dragndrop/LauncherDragController.smali','com/android/launcher3/uioverrides/QuickstepLauncher.smali']
    old=before[changed[0]];new=after[changed[0]]
    method=re.search(r'(?ms)^\.method public getRecentTasks\(II\)Ljava/util/ArrayList;\n.*?^\.end method$',old)
    assert method
    body=method.group().replace(':cond_1d',':cond_1f').replace(':try_end_14',':try_end_16').replace(':catch_15',':catch_17')
    consumer='    invoke-static {v1}, Ljava/util/Arrays;->asList([Ljava/lang/Object;)Ljava/util/List;'
    assert body.count(consumer)==1
    body=body.replace(consumer,'    if-eqz v1, :cond_1f\n\n'+consumer)
    assert old[:method.start()]+body+old[method.end():]==new,'Unexpected Launcher code/debug/handler change'
    patch=''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='original/'+changed[0],tofile='patched/'+changed[0]))
    return {'classes':len(before),'changedMethod':'SystemUiProxy.getRecentTasks(II)',
        'equivalentDefaultFalseEncodingNormalization':sorted(normalized),'diff':patch}


def load_package(directory):
    metadata=json.loads((directory/'build.json').read_text())
    assert metadata['schema']==1 and metadata['sdk']==32 and metadata['baseApkSha256']==ORIGINAL_SHA
    assert metadata['certificatePreserved'] and metadata['certificateSha256']==CERTIFICATE_SHA and metadata['signatureVerifiedForSdk']==32
    assert metadata['changedCodeEntries']==['classes4.dex'] and metadata['semanticAudit']['classes']==5181
    assert digest(directory/'Launcher3QuickStep.apk')==metadata['sha256']
    for name,pin in metadata['sources'].items():assert digest(ROOT/name)==pin,name
    return metadata


def build(base,out,sdk):
    base=base.resolve();out=out.resolve();sdk=sdk.resolve()
    assert digest(base)==ORIGINAL_SHA,'Expected the pinned unmodified API32 Launcher APK'
    assert not out.exists(),'Preserve earlier builds; select a fresh output'
    out.mkdir(parents=True)
    external=sdk/'cmdline-tools/latest/lib/external';jars=[]
    for name,expected in SDK_JARS.items():
        path=external/name;assert digest(path)==expected,'SDK tool differs: '+name;jars.append(path)
    classpath=os.pathsep.join(map(str,jars));commands=[]
    helper=ROOT/'runtime/tools/PocketLauncherRecents.java'
    classes=out/'helper-classes';classes.mkdir()
    commands.append(run(['javac','--release','8','-cp',classpath,'-d',classes,helper]))
    commands.append(run(['java','-cp',str(classes)+os.pathsep+classpath,'PocketLauncherRecents',base,out/'patched-dex']))
    assert [p.name for p in (out/'patched-dex').glob('*.dex')]==['classes4.dex']
    with zipfile.ZipFile(base) as archive:
        assert len(archive.namelist())==len(set(archive.namelist()))
        (out/'original-classes4.dex').write_bytes(archive.read('classes4.dex'))
    for source,destination in [(out/'original-classes4.dex',out/'smali-original'),(out/'patched-dex/classes4.dex',out/'smali-patched')]:
        commands.append(run(['java','-cp',classpath,'com.android.tools.smali.baksmali.Main','disassemble',source,'-o',destination]))
    audit=verify_disassembly(out/'smali-original',out/'smali-patched');patch=audit.pop('diff')
    (out/'null-result-backport.diff').write_text(patch)
    security=CACHE/'aosp-public-testkey-r27';public_keys=public_security(security)
    unsigned=out/'unsigned.apk';aligned=out/'aligned.apk';target=out/'Launcher3QuickStep.apk'
    with zipfile.ZipFile(base) as source,zipfile.ZipFile(unsigned,'w') as destination:
        for entry in source.infolist():
            if signature(entry.filename):continue
            replacement=out/'patched-dex'/entry.filename
            destination.writestr(entry,replacement.read_bytes() if replacement.is_file() else source.read(entry.filename))
    build_tools=sdk/'build-tools/35.0.0';signer=['java','-cp',build_tools/'lib/apksigner.jar','com.android.apksigner.ApkSignerTool']
    commands.append(run([build_tools/'zipalign','-P','16','-f','4',unsigned,aligned]))
    commands.append(run(signer+['sign','--key',security/'testkey.pk8','--cert',security/'testkey.x509.pem',
        '--min-sdk-version','32','--v4-signing-enabled','false','--alignment-preserved','true','--lib-page-alignment','16384','--out',target,aligned]))
    verify=list(map(str,signer+['verify','--print-certs','--min-sdk-version','32','--max-sdk-version','32',target]))
    completed=subprocess.run(verify,check=True,capture_output=True,text=True);commands.append(verify)
    assert re.findall(r'Signer #\d+ certificate SHA-256 digest: ([a-f0-9]{64})',completed.stdout)==[CERTIFICATE_SHA]
    commands.append(run([build_tools/'zipalign','-c','-P','16','4',target]))
    with zipfile.ZipFile(base) as before,zipfile.ZipFile(target) as after:
        entries={n for n in before.namelist() if not signature(n)}
        assert entries=={n for n in after.namelist() if not signature(n)}
        changed=sorted(n for n in entries if before.read(n)!=after.read(n));assert changed==['classes4.dex']
    sources={name:digest(ROOT/name) for name in ['tools/build_api32_launcher.py','runtime/tools/PocketLauncherRecents.java']}
    metadata={'schema':1,'sdk':32,'apk':str(target),'sha256':digest(target),'bytes':target.stat().st_size,
        'baseApkSha256':ORIGINAL_SHA,'certificateSha256':CERTIFICATE_SHA,'certificatePreserved':True,'signatureVerifiedForSdk':32,
        'zipAlignment':16384,'changedCodeEntries':changed,'semanticAudit':audit,'upstreamReference':UPSTREAM,
        'publicAospDevelopmentKeys':public_keys,'sources':sources,'sdkJarSha256':SDK_JARS,'commands':commands,
        'runtimeValidated':False,'scope':'Caller-side null-array guard only; nonnull task data/error handling preserved. Android ART fault injection and real Launcher task UI remain required.'}
    (out/'build.json').write_text(json.dumps(metadata,indent=2)+'\n');load_package(out)
    print('API32_LAUNCHER_NULL_GUARD_BUILD_PASS',target,metadata['sha256'],flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--base-apk',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True);parser.add_argument('--sdk-root',type=Path,default=Path.home()/'.local/share/android-sdk')
    args=parser.parse_args();build(args.base_apk,args.out,args.sdk_root)
