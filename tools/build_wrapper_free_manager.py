#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package Android 9 with syscall translation and no Bionic-patching payload."""
import argparse
from pathlib import Path
import subprocess
import zipfile
from syscall_image import syscall_init

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-apk',type=Path,required=True)
    parser.add_argument('--version',default='0.34')
    args=parser.parse_args()
    out=ROOT/('out/manager-'+args.version)
    out.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(args.base_apk) as base:
        rc=base.read('assets/api28-rootfs/system/etc/init/init.manager.rc.payload').decode()
        # The old optional module-view adapter relies on Bionic interception.
        # Preserve installed modules, but do not activate that adapter here.
        rc=syscall_init(rc)
    init=out/'init.syscall.rc';init.write_text(rc)
    subprocess.run(['make','-f','runtime/Makefile','out/manager-guest/pocket-control.jar'],cwd=ROOT,check=True)
    command=['python3',ROOT/'tools/build_manager_update.py','--base-apk',args.base_apk.resolve(),
        '--out',out,'--version-code',args.version.split('.')[-1],'--version-name',args.version,
        '--guest-control-jar',ROOT/'out/manager-guest/pocket-control.jar',
        '--guest-file','system/etc/init/init.syscall.rc='+str(init),
        '--guest-file','system/bin/pocket-syscall-tracer='+str(ROOT/'out/bionic-mode/libproot.so'),
        '--remove-guest-file','system/lib64/libpocket_direct_boot.so']
    for name in ('pocket-boot-probe','libpocket_hidl_api28.so','libpocket_zygote_api28.so'):
        directory='system/lib64/' if name.endswith('.so') else 'system/bin/'
        command+=['--guest-file',directory+name+'='+str(ROOT/'out/bionic-mode'/name)]
    subprocess.run(list(map(str,command)),cwd=ROOT,check=True)
    target=out/('aosp-pocket-manager-'+args.version+'.apk')
    with zipfile.ZipFile(args.base_apk) as base,zipfile.ZipFile(target) as apk:
        assert not any('libpocket_direct_boot.so' in n for n in apk.namelist())
        libc='assets/api28-rootfs/system/lib64/libc.so.payload'
        assert apk.read(libc)==base.read(libc),'Bionic payload changed'
        print('PASS: no Bionic-patching library bundled; libc payload unchanged')

if __name__=='__main__':main()
