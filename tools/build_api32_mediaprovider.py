#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the pinned MediaProvider JNI with its selected rootless private view.

The stock Java provider and libfuse remain unchanged. This builds a candidate
library only; a successful compile does not validate provider access policy.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request
import zipfile
from prepare_api32 import ROOT,CACHE,prepare_sources,specification

MANIFEST=ROOT/'runtime/api32/mediaprovider-native-sources.json'
PATCH=ROOT/'runtime/api32/mediaprovider-rootless.patch'
POLICY=ROOT/'runtime/api32/mediaprovider_private_view.h'
DIRECTORY_ANCHOR=ROOT/'runtime/api32/mediaprovider_directory_anchor.h'
UNITS=['jni_init','com_android_providers_media_FuseDaemon','FuseDaemon','FuseUtils',
       'MediaProviderWrapper','ReaddirHelper','RedactionInfo','node']
BASE_UNITS=['abi_compatibility','chrono_utils','cmsg','file','logging','mapped_file',
            'parsebool','parsenetaddress','process','properties','stringprintf','strings',
            'threads','test_utils','errors_unix']


def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def build(ndk,apk,out,reference=None):
    out=out.resolve();out.mkdir(parents=True,exist_ok=True)
    reference=(reference or CACHE/'mediaprovider-native-reference').resolve()
    metadata=json.loads(MANIFEST.read_text());assert len(metadata['files'])>=25
    assert digest(apk)==metadata['baseApkSha256'],'Unexpected base module APK'
    source_pins={str(p.relative_to(ROOT)):digest(p) for p in [MANIFEST,PATCH,POLICY,DIRECTORY_ANCHOR,Path(__file__).resolve()]}
    marker=out/'.pocket-mediaprovider-build.json'
    source_marker={k:v for k,v in source_pins.items() if k!=str(Path(__file__).resolve().relative_to(ROOT))}
    if marker.exists():assert json.loads(marker.read_text())==source_marker,'Use a new output directory for changed provider sources'
    else:marker.write_text(json.dumps(source_marker,indent=2)+'\n')
    if (out/'build.json').exists():
        old=json.loads((out/'build.json').read_text())
        assert all(old['sources'].get(k)==v for k,v in source_pins.items()),'Preserve the completed build; select a new output directory'
        assert all(digest(ROOT/k)==v for k,v in old['sources'].items()),'Completed build dependency changed'
        assert old['ndkPropertiesSha256']==digest(ndk/'source.properties')
        target=out/'libfuse_jni.so';assert digest(target)==old['sha256']
        print('API32_MEDIAPROVIDER_JNI_CACHE_VERIFIED',target,flush=True);return target
    inputs={};source=out/'source'
    for name,pin in sorted(metadata['files'].items()):
        parts=Path(name).parts;assert len(parts)>1 and '..' not in parts and not Path(name).is_absolute()
        cached=reference/name
        if not cached.exists():
            url=pin['repository']+'/+/'+pin['commit']+'/'+Path(*parts[1:]).as_posix()+'?format=TEXT'
            with urllib.request.urlopen(url,timeout=30) as response:data=base64.b64decode(response.read(),validate=True)
            assert len(data)==pin['bytes'] and hashlib.sha256(data).hexdigest()==pin['sha256'],name
            cached.parent.mkdir(parents=True,exist_ok=True);cached.write_bytes(data)
        assert cached.stat().st_size==pin['bytes'] and digest(cached)==pin['sha256'],name
        target=source/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(cached.read_bytes());inputs[name]=pin
    commands=[]
    def run(*argv,**kwargs):
        command=list(map(str,argv));commands.append(command);subprocess.run(command,check=True,**kwargs)
    run('patch','--batch','--fuzz=0','-p1','-i',PATCH,cwd=source/'mediaprovider')
    prepare_sources(['libbase','fmtlib','libnativehelper'])
    base=CACHE/'libbase';base_revision=subprocess.check_output(['git','-C',str(base),'rev-parse','HEAD'],text=True).strip()
    expected=next(e for e in specification()['headers'] if e['cache']=='libbase');assert base_revision==expected['commit']
    base_out=source/'base';base_out.mkdir(exist_ok=True)
    private_headers=subprocess.check_output(['git','-C',str(base),'ls-tree','--name-only',base_revision],text=True).splitlines()
    for name in [*(n+'.cpp' for n in BASE_UNITS),*(n for n in private_headers if n.endswith('.h'))]:
        data=subprocess.check_output(['git','-C',str(base),'show',base_revision+':'+name]);(base_out/name).write_bytes(data)
        inputs['libbase/'+name]={'repository':expected['url'],'commit':base_revision,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
    native=out/'libfuse.so'
    with zipfile.ZipFile(apk) as archive:native.write_bytes(archive.read('lib/arm64-v8a/libfuse.so'))
    assert digest(native)==metadata['baseLibfuseSha256']
    llvm=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin';compiler=llvm/'aarch64-linux-android32-clang++'
    includes=[source/'mediaprovider/jni',source/'mediaprovider/jni/include',source/'libfuse/include',source/'libfuse/lib',
              base_out,base/'include',CACHE/'fmtlib/include',CACHE/'libnativehelper/header_only_include',ROOT/'runtime/api32']
    common=[compiler,'-std=gnu++17','-O2','-fPIC','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-variable',
            '-Wno-missing-field-initializers','-Wno-sign-compare','-Wno-vla-cxx-extension','-Wno-c99-designator','-D_GNU_SOURCE','-D_FILE_OFFSET_BITS=64','-DFUSE_USE_VERSION=34','-DFMT_HEADER_ONLY',
            *['-I'+str(p) for p in includes]]
    objects=[]
    units=[(source/'mediaprovider/jni'/(name+'.cpp'),'jni-'+name) for name in UNITS]
    units += [(base_out/(name+'.cpp'),'base-'+name) for name in BASE_UNITS]
    for path,name in units:
        target=out/(name+'.o');run(*common,'-c',path,'-o',target);objects.append(target)
    target=out/'libfuse_jni.so'
    run(compiler,'-shared','-static-libstdc++',*objects,native,'-llog','-landroid','-ldl',
        '-Wl,--no-undefined','-Wl,--exclude-libs,ALL','-Wl,-soname,libfuse_jni.so','-Wl,-z,max-page-size=16384','-o',target)
    dynamic=subprocess.check_output(['readelf','-dW',str(target)],text=True)
    assert 'libfuse.so' in dynamic and 'libc++_shared.so' not in dynamic
    symbols=subprocess.check_output(['readelf','-Ws',str(target)],text=True);assert 'JNI_OnLoad' in symbols
    # Include the exact checked-out public dependency headers in the receipt.
    for directory in [base/'include',CACHE/'fmtlib/include',CACHE/'libnativehelper/header_only_include']:
        for path in sorted(directory.rglob('*.h')):source_pins[str(path.relative_to(ROOT))]=digest(path)
    result={'schema':1,'abi':'arm64-v8a','sdk':32,'baseApkSha256':metadata['baseApkSha256'],
        'baseLibfuseSha256':metadata['baseLibfuseSha256'],'certificateSha256':metadata['certificateSha256'],
        'sources':source_pins,'upstreamInputs':inputs,'commands':commands,'library':str(target),
        'sha256':digest(target),'bytes':target.stat().st_size,'ndkPropertiesSha256':digest(ndk/'source.properties'),
        'runtimeValidated':False,'scope':'Source-built JNI only. Stock Java provider and C libfuse unchanged; selected Pocket private view delegates to live MediaProvider UID/package policy, including child LOOKUP checks. Repacking/signature compatibility and ordinary-app policy remain to validate.'}
    (out/'build.json').write_text(json.dumps(result,indent=2)+'\n');print('API32_MEDIAPROVIDER_JNI_BUILD_PASS',target,flush=True)
    return target


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ndk',type=Path,required=True);p.add_argument('--apk',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--reference',type=Path);a=p.parse_args()
    build(a.ndk.resolve(),a.apk.resolve(),a.out,a.reference)
