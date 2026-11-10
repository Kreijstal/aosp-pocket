#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check every source JNI registration/cache binding against the actual APK DEX."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zipfile


def definitions(data):
    assert data[:4]==b'dex\n' and struct.unpack_from('<I',data,40)[0]==0x12345678
    def uint(at):return struct.unpack_from('<I',data,at)[0]
    def uleb(at):
        value=0
        for shift in range(0,35,7):
            byte=data[at];at+=1;value|=(byte&127)<<shift
            if not byte&128:return value,at
        raise ValueError('Oversized DEX LEB128')
    count,offset=uint(56),uint(60);strings=[]
    for i in range(count):
        _,at=uleb(uint(offset+4*i));end=data.index(0,at);strings.append(data[at:end].decode('utf-8','replace'))
    count,offset=uint(64),uint(68);types=[strings[uint(offset+4*i)] for i in range(count)]
    count,offset=uint(72),uint(76);protos=[]
    for i in range(count):
        at=offset+i*12;result=types[uint(at+4)];params=uint(at+8)
        args=[] if not params else [types[struct.unpack_from('<H',data,params+4+j*2)[0]] for j in range(uint(params))]
        protos.append('('+''.join(args)+')'+result)
    count,offset=uint(80),uint(84);fields=[]
    for i in range(count):
        cls,typ,name=struct.unpack_from('<HHI',data,offset+i*8);fields.append((types[cls],strings[name],types[typ]))
    count,offset=uint(88),uint(92);methods=[]
    for i in range(count):
        cls,proto,name=struct.unpack_from('<HHI',data,offset+i*8);methods.append((types[cls],strings[name],protos[proto]))
    result_methods={};result_fields={};count,offset=uint(96),uint(100)
    for i in range(count):
        at=uint(offset+i*32+24)
        if not at:continue
        sizes=[]
        for _ in range(4):value,at=uleb(at);sizes.append(value)
        for size in sizes[:2]:
            index=0
            for _ in range(size):
                delta,at=uleb(at);flags,at=uleb(at);index+=delta;result_fields[fields[index]]=flags
        for size in sizes[2:]:
            index=0
            for _ in range(size):
                delta,at=uleb(at);flags,at=uleb(at);_,at=uleb(at);index+=delta;result_methods[methods[index]]=flags
    return result_methods,result_fields


def validate(apk,source):
    methods={};fields={}
    with zipfile.ZipFile(apk) as archive:
        for name in archive.namelist():
            if re.fullmatch(r'classes(?:\d+)?\.dex',name):
                m,f=definitions(archive.read(name));methods.update(m);fields.update(f)
    native=(source/'com_android_providers_media_FuseDaemon.cpp').read_text()
    wrapper=(source/'MediaProviderWrapper.cpp').read_text();bindings=[]
    table=re.findall(r'\{"(native_[^"]+)",\s*"([^"]+)"',native);assert len(table)>=8
    for name,signature in table:
        key=('Lcom/android/providers/media/fuse/FuseDaemon;',name,signature)
        assert key in methods and methods[key]&0x100,key;bindings.append({'kind':'native','name':name,'signature':signature})
    cached=re.findall(r'CacheMethod\(env,\s*"([^"]+)",\s*((?:"[^"]*"\s*)+)',wrapper);assert len(cached)>=13
    for name,encoded in cached:
        signature=''.join(re.findall(r'"([^"]*)"',encoded));key=('Lcom/android/providers/media/MediaProvider;',name+'ForFuse',signature)
        assert key in methods,key;bindings.append({'kind':'provider','name':key[1],'signature':signature})
    classes={'file_lookup_result_class_':'FileLookupResult','file_open_result_class_':'FileOpenResult'}
    cached_fields=re.findall(r'CacheField\(env,\s*(\w+),\s*"([^"]+)",\s*"([^"]+)"',wrapper);assert len(cached_fields)>=10
    for owner,name,signature in cached_fields:
        key=('Lcom/android/providers/media/'+classes[owner]+';',name,signature)
        assert key in fields and not fields[key]&8,key;bindings.append({'kind':'field','name':classes[owner]+'.'+name,'signature':signature})
    return {'validation':'PASS','apkSha256':hashlib.file_digest(apk.open('rb'),'sha256').hexdigest(),'bindings':bindings,
        'nativeBindings':len(table),'providerMethods':len(cached),'instanceFields':len(cached_fields),
        'sourceSha256':{str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in [source/'com_android_providers_media_FuseDaemon.cpp',source/'MediaProviderWrapper.cpp']},
        'scope':'Exact native/source method and instance-field descriptors against actual DEX definitions; not runtime JNI initialization or private policy proof.'}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--apk',type=Path,required=True);p.add_argument('--source',type=Path,required=True);p.add_argument('--report',type=Path,required=True);a=p.parse_args()
    report=validate(a.apk,a.source);a.report.write_text(json.dumps(report,indent=2)+'\n');print('API32_MEDIAPROVIDER_JNI_ABI_PASS',report['nativeBindings'],report['providerMethods'],report['instanceFields'])
