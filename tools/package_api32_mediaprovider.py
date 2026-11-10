#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Repackage the source-built provider JNI using the original public AOSP media key."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile
from prepare_api32 import ROOT

LIBRARY='lib/arm64-v8a/libfuse_jni.so'


def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def signature(name):
    upper=name.upper()
    return upper.startswith('META-INF/') and (upper=='META-INF/MANIFEST.MF' or upper.endswith(('.SF','.RSA','.DSA','.EC')))


def load_package(directory):
    metadata=json.loads((directory/'build.json').read_text());assert metadata['schema']==1 and metadata['sdk']==32
    assert metadata['certificatePreserved'] and metadata['signatureVerifiedForSdk']==32 and metadata['changedCodeEntries']==[LIBRARY]
    assert digest(directory/'MediaProvider.apk')==metadata['sha256']
    for name,pin in metadata['sources'].items():assert digest(ROOT/name)==pin,name
    return metadata


def build(native,base,out,sdk,security):
    record=json.loads((native/'build.json').read_text());assert record['schema']==1 and record['sdk']==32
    for name,pin in record['sources'].items():assert digest(ROOT/name)==pin,name
    assert digest(base)==record['baseApkSha256'] and digest(native/'libfuse_jni.so')==record['sha256']
    cert=security/'media.x509.pem';key=security/'media.pk8'
    der=subprocess.check_output(['openssl','x509','-in',str(cert),'-outform','DER'])
    assert hashlib.sha256(der).hexdigest()==record['certificateSha256'],'Original MediaProvider signing certificate does not match this key'
    assert key.is_file();out=out.resolve();out.mkdir(parents=True,exist_ok=True)
    assert not (out/'build.json').exists(),'Preserve completed package receipts; choose a new output directory'
    unsigned=out/'unsigned.apk';aligned=out/'aligned.apk';target=out/'MediaProvider.apk'
    with zipfile.ZipFile(base) as source,zipfile.ZipFile(unsigned,'w') as destination:
        assert LIBRARY in source.namelist()
        for entry in source.infolist():
            if signature(entry.filename):continue
            destination.writestr(entry,(native/'libfuse_jni.so').read_bytes() if entry.filename==LIBRARY else source.read(entry))
    commands=[list(map(str,c)) for c in [
        [sdk/'zipalign','-P','16','-f','4',unsigned,aligned],
        [sdk/'apksigner','sign','--key',key,'--cert',cert,'--min-sdk-version','32','--v4-signing-enabled','false','--alignment-preserved','true','--lib-page-alignment','16384','--out',target,aligned],
        [sdk/'apksigner','verify','--print-certs','--min-sdk-version','32','--max-sdk-version','32',target],
        [sdk/'zipalign','-c','-P','16','4',target]]]
    verification=''
    for command in commands:
        completed=subprocess.run(command,capture_output=True,text=True)
        if completed.returncode:raise RuntimeError('Command failed: '+str(command)+'\n'+completed.stdout[-4000:]+completed.stderr[-4000:])
        if command[1]=='verify':verification=completed.stdout
    certs=[line.split(': ',1)[1] for line in verification.splitlines() if 'certificate SHA-256 digest: ' in line]
    assert certs==[record['certificateSha256']]
    with zipfile.ZipFile(base) as a,zipfile.ZipFile(target) as b:
        entries={n for n in a.namelist() if not signature(n)};assert entries=={n for n in b.namelist() if not signature(n)}
        changed=[n for n in sorted(entries) if a.read(n)!=b.read(n)];assert changed==[LIBRARY],changed
        assert hashlib.sha256(b.read(LIBRARY)).hexdigest()==record['sha256']
    sources=dict(record['sources']);sources['tools/package_api32_mediaprovider.py']=digest(Path(__file__).resolve())
    metadata={'schema':1,'sdk':32,'apk':str(target),'sha256':digest(target),'bytes':target.stat().st_size,
        'baseApkSha256':record['baseApkSha256'],'nativeBuildReceiptSha256':digest(native/'build.json'),
        'librarySha256':record['sha256'],'certificateSha256':record['certificateSha256'],'certificatePreserved':True,
        'signatureVerifiedForSdk':32,'zipAlignment':16384,'changedCodeEntries':changed,'sources':sources,'commands':commands,
        'runtimeValidated':False,'scope':'JNI source replacement only; all Java/dex/resources/manifest and C libfuse payloads remain byte-identical, original AOSP media signing certificate preserved. Runtime policy and source/binary ABI compatibility still require the full guest trial.'}
    (out/'build.json').write_text(json.dumps(metadata,indent=2)+'\n');load_package(out)
    print('API32_MEDIAPROVIDER_PACKAGE_PASS',target,metadata['sha256'],flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native-build',type=Path,required=True);p.add_argument('--base-apk',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--sdk-build-tools',type=Path,default=Path('/home/kreijstal/.local/share/android-sdk/build-tools/35.0.0'))
    p.add_argument('--security',type=Path,default=ROOT.parent/'s20fe-hardware-lab/artifacts/lineage/source/build/make/target/product/security')
    a=p.parse_args();build(a.native_build.resolve(),a.base_apk.resolve(),a.out,a.sdk_build_tools.resolve(),a.security.resolve())
