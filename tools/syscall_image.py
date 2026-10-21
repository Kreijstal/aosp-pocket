# SPDX-License-Identifier: GPL-3.0-only
"""Shared init migration for the wrapper-free Android 9 Manager image."""
def syscall_init(rc):
    assert 'start-system-server' in rc and 'GuestControl' in rc
    rc=rc.replace('import /system/etc/init/init.magisk.rc','')
    trigger='# Optional runtime controls data/zygote ordering'
    if trigger in rc:
        assert rc.count(trigger)==1
        rc=rc.replace(trigger,'on property:pocket.data.ready=1\n    setprop pocket.magisk.status adapter-required\n    class_start pocket-zygote')
    assert 'on property:pocket.data.ready=1' in rc
    return rc.replace('setenv LD_PRELOAD /system/lib64/libpocket_binder_api28.so\n',
        'setenv LD_PRELOAD /system/lib64/libpocket_binder_api28.so:/system/lib64/libpocket_hidl_api28.so\n')
