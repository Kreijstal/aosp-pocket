#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the app-private HWC with pinned Android 9 HAL headers and guest libraries."""
import argparse,hashlib,json,subprocess,tarfile,urllib.request
from pathlib import Path
from target import ROOT, clang, reference_dir

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk',type=Path,required=True)
    args=parser.parse_args()
    lock=json.loads((ROOT/'sources.lock.json').read_text())['composer_headers']
    work=ROOT/'out/composer-headers';work.mkdir(parents=True,exist_ok=True)
    archive=work/'include.tar.gz'
    if not archive.exists():
        temporary=archive.with_suffix('.part');urllib.request.urlretrieve(lock['url'],temporary);temporary.rename(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=lock['sha256']:raise SystemExit('Unexpected HAL header archive')
    with tarfile.open(archive) as tar:tar.extractall(work/'include',filter='data')
    core=ROOT/'out/api28/system-core';libs=reference_dir()/'system/lib64'
    if not (libs/'libhardware.so').is_file():raise SystemExit('Build the pinned Android 9 reference first')
    output=ROOT/'out/composer/hwcomposer.pocket.so';output.parent.mkdir(parents=True,exist_ok=True)
    compiler=clang(args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin',28)
    command=[str(compiler),'-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-fPIC','-shared',
             '-I'+str(work/'include'),'-I'+str(core/'libcutils/include'),'-I'+str(core/'libsystem/include'),
             str(ROOT/'runtime/direct/pocket_composer.c'),'-L'+str(libs),'-Wl,-rpath-link,'+str(libs),
             '-lhardware','-lcutils','-llog','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-o',str(output)]
    subprocess.run(command,check=True)
    print('PASS: Android 9 direct composer',output)

if __name__=='__main__':main()
