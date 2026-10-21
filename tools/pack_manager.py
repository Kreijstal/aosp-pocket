#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Add the separately versioned direct image to the dual-backend manager."""
import argparse
from pathlib import Path
import zipfile
from syscall_image import syscall_init

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    rc=(ROOT/'out/boot/rootfs/system/etc/init/init.system-server.rc').read_text()
    old='dev.kreijstal.aosppocket.guest.FrameworkProbe /system/etc/pocket-smoke.apk'
    if rc.count(old)!=1:raise SystemExit('Unexpected framework probe init configuration')
    rc=rc.replace('pocket-framework-probe','pocket-control').replace(old,'dev.kreijstal.aosppocket.guest.GuestControl')
    start=rc.index('\nservice pocket-control ')
    end=rc.index('\nservice ',start+1)
    block=rc[start:end]
    if block.count('    oneshot\n')!=1:raise SystemExit('Unexpected control service options')
    rc=rc[:start]+block.replace('    oneshot\n','    oneshot\n    socket pocket_control stream 0600 1000 1000\n')+rc[end:]
    with zipfile.ZipFile(ROOT/'out/runtime/packed.apk') as runtime,zipfile.ZipFile(args.out/'base.apk') as base,zipfile.ZipFile(args.out/'packed.apk','w',zipfile.ZIP_DEFLATED,compresslevel=1) as apk:
        for item in runtime.infolist():
            if item.filename not in ('AndroidManifest.xml','classes.dex'):
                # Copy compressed guest data once; each backend retains its own
                # data directory and neither image is injected into the other.
                apk.writestr(item,runtime.read(item))
        for item in base.infolist():apk.writestr(item,base.read(item))
        for path in sorted((ROOT/'out/boot/rootfs').rglob('*')):
            if path.is_file():
                name=path.relative_to(ROOT/'out/boot/rootfs').as_posix()
                if name=='system/lib64/libpocket_direct_boot.so':continue
                if name=='system/build.prop':
                    props=path.read_text().replace('ro.hardware.hwcomposer=redroid','ro.hardware.hwcomposer=pocket').replace('ro.boot.redroid_fps=30','ro.boot.redroid_fps=60')
                    apk.writestr('assets/api28-rootfs/'+name+'.payload',props)
                else:apk.write(path,'assets/api28-rootfs/'+name+'.payload')
        apk.write(ROOT/'out/manager-guest/pocket-control.jar','assets/api28-rootfs/system/framework/pocket-zygote.jar.payload')
        apk.write(ROOT/'out/composer/hwcomposer.pocket.so','assets/api28-rootfs/vendor/lib64/hw/hwcomposer.pocket.so.payload')
        apk.write(ROOT/'out/manager-guest/host-phone.apk','assets/api28-rootfs/system/app/PocketHostPhone/PocketHostPhone.apk.payload')
        apk.writestr('assets/api28-rootfs/system/etc/init/init.manager.rc.payload',rc)
        apk.writestr('assets/api28-rootfs/system/etc/init/init.syscall.rc.payload',syscall_init(rc))
        apk.write(ROOT/'out/runtime-native/libproot.so','assets/api28-rootfs/system/bin/pocket-syscall-tracer.payload')
        apk.write(ROOT/'out/manager-native/getprop','assets/api28-rootfs/system/etc/pocket-magisk/getprop.payload')
        apk.write(ROOT/'out/api28/reference/system/etc/NOTICE.xml.gz','assets/licenses/android9-NOTICE.xml.gz')
        apk.write(ROOT/'out/boot/hooks.json','assets/direct-boot-hooks.json')
        apk.write(ROOT/'tools/pocketctl.py','assets/pocketctl.py')
    print('Packed dual-backend manager:',args.out/'packed.apk')

if __name__=='__main__':main()
