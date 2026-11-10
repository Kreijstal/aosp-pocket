#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the independent API28+ host AppFuse JNI for downloadable API32 images."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from target import ROOT

LIBRARY='libpocket_external32.so'
SOURCES=['runtime/storage/native/external_fuse32.c','runtime/storage/native/external_holders32.h','tools/build_api32_external_host.py']
def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def load_manifest(directory):
    directory=directory.resolve();metadata=json.loads((directory/'build.json').read_text());library=directory/LIBRARY
    if metadata['schema']!=1 or metadata['abi']!='arm64-v8a' or metadata['minSdk']!=28:raise ValueError('External host ABI differs')
    if library.is_symlink() or library.stat().st_size!=metadata['bytes'] or digest(library)!=metadata['sha256']:raise ValueError('External host library differs')
    if metadata['sources']!={p:digest(ROOT/p) for p in SOURCES}:raise ValueError('External host sources changed')
    header=library.read_bytes()[:20]
    if header[:6]!=b'\x7fELF\x02\x01' or header[18:20]!=b'\xb7\x00':raise ValueError('External host library is not ARM64')
    return metadata
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--ndk',type=Path,required=True);parser.add_argument('--out',type=Path,required=True);args=parser.parse_args()
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True);(out/'build.json').unlink(missing_ok=True)
    sources={p:digest(ROOT/p) for p in SOURCES}
    command=[str(args.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'),'-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-pthread','-Wl,-z,max-page-size=16384',str(ROOT/SOURCES[0]),'-llog','-o',str(out/LIBRARY)]
    subprocess.run(command,cwd=ROOT,check=True)
    if sources!={p:digest(ROOT/p) for p in SOURCES}:raise RuntimeError('External host sources changed during build')
    result={'schema':1,'abi':'arm64-v8a','minSdk':28,'library':LIBRARY,'bytes':(out/LIBRARY).stat().st_size,'sha256':digest(out/LIBRARY),'sources':sources,'command':command,'runtimeValidated':False}
    (out/'build.json').write_text(json.dumps(result,indent=2)+'\n');load_manifest(out);print('EXTERNAL_HOST32_BUILD_PASS',out/'build.json')
if __name__=='__main__':main()
