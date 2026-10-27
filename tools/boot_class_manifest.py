#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Order our boot definitions before subclasses/interfaces, without JVMTI."""
import struct
import zipfile

GUEST_PATH='system/framework/pocket-zygote.classes'

def manifest(jar):
    with zipfile.ZipFile(jar) as archive:
        dex=archive.read('classes.dex')
        if any(name.startswith('classes') and name.endswith('.dex') and name!='classes.dex' for name in archive.namelist()):
            raise ValueError('Bootstrap JAR must contain exactly one DEX')
    if len(dex)<112 or dex[:4]!=b'dex\n' or struct.unpack_from('<I',dex,32)[0]!=len(dex):
        raise ValueError('Invalid bootstrap DEX')
    def word(at):return struct.unpack_from('<I',dex,at)[0]
    def string(index):
        at=word(word(60)+index*4)
        for _ in range(5):
            byte=dex[at];at+=1
            if byte<128:break
        else:raise ValueError('Invalid DEX string length')
        return dex[at:dex.index(0,at)].decode('ascii')
    types=[string(word(word(68)+i*4)) for i in range(word(64))]
    classes={}
    for i in range(word(96)):
        at=word(100)+i*32;name=types[word(at)]
        if not name.startswith('Ldev/kreijstal/aosppocket/') or not name.endswith(';'):
            raise ValueError('Unexpected bootstrap class '+name)
        parents=[];superclass=word(at+8)
        if superclass!=0xffffffff:parents.append(types[superclass])
        interfaces=word(at+12)
        if interfaces:
            parents.extend(types[struct.unpack_from('<H',dex,interfaces+4+j*2)[0]] for j in range(word(interfaces)))
        if name in classes:raise ValueError('Duplicate bootstrap class '+name)
        classes[name]=parents
    result=[];active=set();done=set()
    def visit(name):
        if name in done:return
        if name in active:raise ValueError('Cyclic bootstrap inheritance '+name)
        active.add(name)
        for parent in classes[name]:
            if parent in classes:visit(parent)
        active.remove(name);done.add(name);result.append(name[1:-1].replace('/','.'))
    for name in sorted(classes):visit(name)
    if not result or len(result)>1024:raise ValueError('Invalid bootstrap class count')
    return ('PCKBOOT1\n'+'\n'.join(result)+'\n').encode('ascii')

def write_manifest(jar,path):
    path.write_bytes(manifest(jar));return path
