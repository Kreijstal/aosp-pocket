#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build fixtures locally; run in phone Termux using its paired Pocket console."""
import argparse
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import zipfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dir',type=Path,required=True)
    parser.add_argument('--ndk',type=Path,help='Build local ARM64 fixtures instead of running device tests')
    args=parser.parse_args()
    if args.ndk:
        root=Path(__file__).resolve().parents[1]
        args.dir.mkdir(parents=True,exist_ok=True)
        clang=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
        flags=['-Wall','-Wextra','-Werror','-O2','-Wl,-z,max-page-size=16384']
        subprocess.run([clang,*flags,'-shared','-fPIC',root/'runtime/tests/loader/fixture.c',
                        '-o',args.dir/'libfixture.so'],check=True)
        subprocess.run([clang,*flags,root/'runtime/tests/loader/probe.c','-ldl',
                        '-o',args.dir/'loader-probe'],check=True)
        subprocess.run([clang,*flags,root/'runtime/tests/loader/signals.c',
                        '-o',args.dir/'signal-probe'],check=True)
        name='lib/arm64-v8a/libfixture.so'
        entry=zipfile.ZipInfo(name)
        pad=(-(30+len(name)+4))%16384
        entry.extra=struct.pack('<HH',0xffff,pad)+bytes(pad)
        with zipfile.ZipFile(args.dir/'fixture.apk','w') as archive:
            archive.writestr(entry,(args.dir/'libfixture.so').read_bytes())
        print('PASS: ARM64 fixtures built; uncompressed APK library aligned to 16 KiB')
        return
    spec=importlib.util.spec_from_file_location('pocketctl','/storage/emulated/0/Download/pocketctl.py')
    ctl=importlib.util.module_from_spec(spec);spec.loader.exec_module(ctl)
    config=json.loads((Path.home()/'.config/pocketctl/config.json').read_text())

    def command(argv,source=None):
        out,err=io.BytesIO(),io.BytesIO()
        code=ctl.request(config,2,argv,source=source,stdout=out,stderr=err)
        if code: raise RuntimeError(f'{argv}: exit {code}: '+out.getvalue().decode()+err.getvalue().decode())
        return out.getvalue().decode()

    base='/data/local/tmp/pocket-loader-tests'
    command(['/system/bin/toybox','mkdir','-p',base+'/allowed',base+'/outside'])
    for local,remote in [('loader-probe','probe'),('signal-probe','signal-probe'),('libfixture.so','allowed/libfixture.so'),
                         ('libfixture.so','outside/libfixture.so'),('fixture.apk','allowed/fixture.apk')]:
        target=base+'/'+remote
        with (args.dir/local).open('rb') as source:
            command(['/system/bin/sh','-c',
                '/system/bin/toybox cat > "$1.next" && /system/bin/toybox chmod 755 "$1.next" && /system/bin/toybox mv "$1.next" "$1"',
                'upload',target],source)
    receipt=[]
    for mode in ('plain','ext','namespace'):
        for suffix in ('allowed/libfixture.so','allowed/fixture.apk!/lib/arm64-v8a/libfixture.so'):
            result=command([base+'/probe',mode,base+'/'+suffix]);print(mode,suffix,result.strip(),flush=True)
            receipt.append({'mode':mode,'library':suffix,'result':'PASS'})
    result=command([base+'/probe','namespace','libfixture.so']);print(result.strip(),flush=True)
    receipt.append({'mode':'namespace','library':'soname search','result':'PASS'})
    result=command([base+'/probe','deny',base+'/outside/libfixture.so']);print(result.strip(),flush=True)
    receipt.append({'mode':'namespace','library':'outside permitted directory','result':'PASS: denied'})
    result=command([base+'/signal-probe']);print(result.strip(),flush=True)
    receipt.append({'mode':'signals','result':'PASS: runtime leaves default and app handlers alone'})
    (args.dir/'loader-validation.json').write_text(json.dumps(receipt,indent=2)+'\n')


if __name__=='__main__':main()
