#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Narrow rebuild of the native interpreter and guest firewall frontend."""
import argparse
from pathlib import Path
import subprocess
from target import ROOT
from validate_native_loader import validate as validate_loader

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,required=True)
    parser.add_argument('--out',type=Path,default=ROOT/'out/guest-firewall')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    cc=args.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang'
    def run(*args):subprocess.run([str(x) for x in args],cwd=ROOT,check=True)
    run(cc,'-O2','-g','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic',
        '-nostdlib','-static','-Wl,-T,runtime/loader/loader.ld,-z,max-page-size=16384',
        ROOT/'runtime/loader/loader.c',ROOT/'runtime/loader/entry.S','-o',out/'pocket-loader')
    validate_loader(out/'pocket-loader')
    run(cc,'-O2','-Wall','-Wextra','-Werror',ROOT/'runtime/loader/iptables.c',
        '-Wl,-z,max-page-size=16384','-o',out/'iptables')
    print('PASS: ARM64 interpreter and guest iptables built with -Werror')

if __name__=='__main__':main()
