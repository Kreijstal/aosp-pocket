#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the four ARM64 Manager native libraries without packaging a guest image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from target import ROOT

LIBRARIES=('libpocket_runtime.so','libproot.so','libpocket_frames.so','libpocket_ce.so')


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source,'sha256').hexdigest()


def source_pins():
    inputs=list((ROOT/'runtime/native').rglob('*'))
    inputs+=[ROOT/name for name in [
        'runtime/frame_buffers.c','runtime/storage/native/ce_fuse.c',
        'runtime/anbox-renderer.patch','runtime/anbox-input.patch',
        'runtime/proot-android.patch','runtime/proot-pocket-owner.patch',
        'runtime/proot-crash-diagnostics.patch','runtime/proot-sysexit-errors.patch',
        'runtime/proot-exitkill.patch','runtime/proot-nested-bindings.patch',
        'runtime/proot_pocket_owner.h','runtime/talloc.cross',
        'tools/build_runtime_native.py','tools/build_host_native.py','tools/target.py','sources.lock.json']]
    return {p.relative_to(ROOT).as_posix():digest(p) for p in sorted(inputs) if p.is_file()}


def load_manifest(directory):
    directory=directory.resolve()
    manifest=json.loads((directory/'build.json').read_text())
    if manifest.get('schema')!=1 or manifest.get('abi')!='arm64-v8a' or manifest.get('minSdk')!=28:
        raise ValueError('Expected ARM64 API28 Manager host libraries')
    if set(manifest.get('libraries',{}))!=set(LIBRARIES):
        raise ValueError('Expected all four Manager host libraries')
    for name,record in manifest['libraries'].items():
        file=directory/name
        if file.is_symlink() or file.stat().st_size!=record['bytes'] or digest(file)!=record['sha256']:
            raise ValueError('Host native library differs from its source build: '+name)
        with file.open('rb') as source:header=source.read(20)
        if header[:6]!=b'\x7fELF\x02\x01' or header[18:20]!=b'\xb7\x00':
            raise ValueError('Host library is not a little-endian ARM64 ELF: '+name)
    for name,expected in manifest['sources'].items():
        file=(ROOT/name).resolve()
        if not file.is_relative_to(ROOT) or digest(file)!=expected:
            raise ValueError('Host native source changed after building: '+name)
    return manifest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--jobs',type=int,default=8)
    args=parser.parse_args()
    if args.jobs<1:parser.error('--jobs must be positive')
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    manifest=out/'build.json'
    # A failed rebuild must not leave a success record for earlier outputs.
    manifest.unlink(missing_ok=True)
    compiler=args.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    sources=source_pins()
    commands=[]
    def run(command):
        command=list(map(str,command));commands.append(command)
        subprocess.run(command,cwd=ROOT,check=True,env={**os.environ,'POCKET_ARCH':'arm64'})
    run([sys.executable,ROOT/'tools/build_runtime_native.py','--ndk',args.ndk.resolve(),'--jobs',str(args.jobs)])
    for name in LIBRARIES[:2]:shutil.copyfile(ROOT/'out/runtime-native'/name,out/name)
    for source,name,extra in [
        ('runtime/frame_buffers.c','libpocket_frames.so',['-landroid']),
        ('runtime/storage/native/ce_fuse.c','libpocket_ce.so',['-pthread','-llog'])]:
        run([compiler,'-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',
             '-Wl,-z,max-page-size=16384',ROOT/source,*extra,'-o',out/name])
    if sources!=source_pins():raise RuntimeError('Host native source changed during the build')
    report={'schema':1,'abi':'arm64-v8a','minSdk':28,
        'sources':sources,
        'libraries':{name:{'bytes':(out/name).stat().st_size,'sha256':digest(out/name)} for name in LIBRARIES},
        'commands':commands,'cachedDependencies':True,'runtimeValidated':False,
        'scope':'Native builders invoked against pinned dependencies; source/compiler caches retained. This is not a clean-room reproducibility or runtime validation claim.'}
    manifest.write_text(json.dumps(report,indent=2)+'\n')
    load_manifest(out)
    print('HOST_NATIVE_SOURCE_BUILD_PASS',manifest)


if __name__=='__main__':main()
