#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Called after Pocket's data preparation, before starting guest zygote.
set -eu
TB=/system/bin/toybox
$TB ln -sf /system/etc/pocket-magisk/getprop /system/bin/getprop
$TB ln -sf /system/bin/toybox /system/bin/setprop
if [ -f /data/adb/pocket-magisk.disabled ]; then
    setprop pocket.magisk.postfs 1
    exit 0
fi
$TB mkdir -p /sbin/.magisk/device /sbin/.magisk/busybox /data/adb/magisk /data/adb/modules /cache
$TB chmod 700 /data/adb
$TB cp /system/etc/pocket-magisk/magisk /sbin/magisk
$TB chmod 755 /sbin/magisk
for name in busybox util_functions.sh module_installer.sh; do
    $TB cp "/system/etc/pocket-magisk/$name" "/data/adb/magisk/$name"
done
$TB chmod 755 /data/adb/magisk/busybox
$TB ln -sf /sbin/magisk /system/bin/magisk
$TB ln -sf /sbin/magisk /system/bin/su
$TB ln -sf /sbin/magisk /system/bin/resetprop
# Rotate one previous boot's diagnostic file; preserve module/data state.
if [ -f /logs/magisk.log ]; then $TB mv -f /logs/magisk.log /logs/magisk.previous.log; fi
setprop pocket.magisk.prepared 1
