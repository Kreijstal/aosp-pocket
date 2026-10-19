#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Resolve direct-profile crash PC/LR through captured maps and ELF LOADs."""
import argparse
import json
from pathlib import Path
import re
import subprocess
from build_boot import headers
from prepare_api28 import ROOT

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('report',type=Path)
parser.add_argument('--field',default='system_server_log')
parser.add_argument('--ndk',type=Path,required=True)
args=parser.parse_args()
text=args.report.read_text()
if text.lstrip().startswith('{'):
    text=json.JSONDecoder().raw_decode(text.lstrip())[0][args.field]
chunks=text.split('DIRECT_CRASH ')[1:]
if not chunks: raise SystemExit('No direct crash in this report')
chunk=chunks[0]
print('DIRECT_CRASH '+chunk.splitlines()[0])
for field in ('pc','lr'):
    address=int(re.search(field+r'=0x([0-9a-f]+)',chunk)[1],16)
    found=False
    for line in chunk.splitlines()[1:]:
        match=re.fullmatch(r'([0-9a-f]+)-([0-9a-f]+)\s+\S+\s+([0-9a-f]+)\s+\S+\s+\d+\s+(.+)',line)
        if not match or not int(match[1],16)<=address<int(match[2],16): continue
        path=match[4].split('/api28-rootfs/',1)
        if len(path)!=2 or '..' in Path(path[1]).parts: break
        binary=ROOT/'out/boot/rootfs'/path[1]
        if not binary.is_file(): break
        offset=address-int(match[1],16)+int(match[3],16)
        segment=next(p for _,p in headers(binary.read_bytes()) if p[0]==1 and p[2]<=offset<p[2]+p[5])
        virtual=offset+segment[3]-segment[2]
        print(f'{field}: {path[1]} ELF virtual address {virtual:#x}, file offset {offset:#x}')
        subprocess.run([args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-addr2line','-f','-C','-e',binary,hex(virtual)],check=True)
        found=True
        break
    if not found: print(f'{field}: {address:#x} has no captured local ELF mapping')
