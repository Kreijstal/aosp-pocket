#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Rebuild only Pocket's syscall tracer, reusing the prepared talloc archive."""
import argparse
from pathlib import Path
import shutil
from build_runtime_native import WORK, prepare_proot, run
from target import ROOT, clang

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,required=True)
    args=parser.parse_args()
    llvm=args.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    talloc=WORK/'talloc-prefix'
    if not (talloc/'lib/libtalloc.a').is_file():raise SystemExit('Prepare the native runtime/talloc first')
    proot=prepare_proot(llvm)
    cc=clang(llvm,28)
    run(['make','-C',proot/'src','-j8','HAS_SWIG=','HAS_PYTHON_CONFIG=',
        'GIT_VERSION=pocket','GIT_COMMIT=pocket',f'CC={cc}',f'LD={cc}',
        f'STRIP={llvm/"llvm-strip"}',f'OBJCOPY={llvm/"llvm-objcopy"}',f'OBJDUMP={llvm/"llvm-objdump"}',
        f'CFLAGS=-O2 -g -fno-omit-frame-pointer -I{talloc/"include"} -D_GNU_SOURCE',
        f'LDFLAGS={talloc/"lib/libtalloc.a"} -Wl,-z,max-page-size=16384'])
    out=ROOT/'out/bionic-mode'
    out.mkdir(parents=True,exist_ok=True)
    shutil.copy2(proot/'src/proot',out/'libproot.so')
    shutil.copy2(proot/'src/proot',ROOT/'out/runtime-native/libproot.so')
    print('PASS: focused syscall-tracer rebuild')

if __name__=='__main__':main()
