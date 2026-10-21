#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verify the support APK contains adapters but no embedded Magisk runtime."""
import sys
import zipfile
from pathlib import PurePosixPath


def validate(path):
    with zipfile.ZipFile(path) as apk:
        names=apk.namelist()
        forbidden={'magisk','magisk32','Magisk.apk','busybox','util_functions.sh','module_installer.sh',
                   'libbusybox.so.1.38.0','libandroid-selinux.so','libpcre2-8.so'}
        embedded=[name for name in names if PurePosixPath(name.removesuffix('.payload')).name in forbidden
                  or '/system/app/Magisk/' in name or name.startswith('assets/api28-rootfs/data/adb/')]
        if embedded: raise RuntimeError('Magisk runtime must be external: '+', '.join(embedded))
        prefix='assets/api28-rootfs/'
        for required in ['system/lib64/libpocket_direct_boot.so','system/lib64/libpocket_zygote_api28.so',
                         'system/etc/pocket-magisk/setup.sh','system/etc/init/init.magisk.rc']:
            if prefix+required+'.payload' not in names: raise RuntimeError('Missing compatibility adapter: '+required)
    print('PASS: Pocket compatibility adapters present; Magisk core, Manager and runtime tools absent')


if __name__=='__main__': validate(sys.argv[1])
