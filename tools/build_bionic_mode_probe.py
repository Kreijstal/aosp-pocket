#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build an isolated shim and probe; do not replace a working guest runtime."""
import argparse
from pathlib import Path
import shutil
import subprocess
from build_boot import generate_hooks, reserve_interpreter
from target import ROOT, clang, selected

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,required=True)
    args=parser.parse_args()
    if selected()!='arm64':raise SystemExit('The Pi probe requires POCKET_ARCH=arm64')
    out=ROOT/'out/bionic-mode'
    out.mkdir(parents=True,exist_ok=True)
    llvm=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    compiler=clang(llvm,28)
    generate_hooks(ROOT/'out/api28/reference/system/lib64/libc.so',llvm,out)
    common=[str(compiler),'-O2','-Wall','-Wextra','-Werror','-Wl,-z,max-page-size=16384']
    def run(*extra):subprocess.run(common+list(map(str,extra)),check=True)
    run('-fPIC','-shared','-Wno-misleading-indentation','-DPOCKET_DIRECT_BOOT','-I'+str(out),
        ROOT/'runtime/direct/guest_paths.c',ROOT/'runtime/direct/boot_runtime.c',ROOT/'runtime/direct/boot_uevent.c',
        ROOT/'out/api28/reference/system/lib64/libc.so','-ldl','-Wl,--no-undefined',
        '-Wl,-soname,libpocket_direct_boot.so','-o',out/'libpocket_direct_boot.so')
    run(ROOT/'runtime/tests/bionic_mode.c','-ldl','-o',out/'bionic-mode-probe')
    assert reserve_interpreter(out/'bionic-mode-probe')
    run('-static','-fno-emulated-tls',ROOT/'runtime/tests/bionic_launcher.c','-o',out/'bionic-mode-launcher')
    run('-static','-fno-emulated-tls',ROOT/'runtime/tests/syscall_guest_launcher.c','-o',out/'syscall-guest-launcher')
    run(ROOT/'runtime/tests/proot_owner.c','-o',out/'proot-owner-probe')
    assert reserve_interpreter(out/'proot-owner-probe')
    proot=ROOT/'out/runtime-native/libproot.so'
    if proot.is_file():shutil.copy2(proot,out/'libproot.so')
    print('PASS: built isolated Bionic-mode shim, executable-byte probe and static launcher')

if __name__=='__main__':main()
