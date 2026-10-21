#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Called after Pocket's data preparation, before starting guest zygote.
set -eu
TB=/system/bin/toybox
$TB rm -f /.pocket-module-view/ready
$TB ln -sf /system/etc/pocket-magisk/getprop /system/bin/getprop
$TB ln -sf /system/bin/toybox /system/bin/setprop
if [ -f /data/adb/pocket-magisk.disabled ] || [ ! -x /data/adb/magisk/magisk ]; then
    for name in magisk su resetprop; do
        if [ "$($TB readlink "/system/bin/$name" 2>/dev/null || :)" = /sbin/magisk ]; then
            $TB rm -f "/system/bin/$name"
        fi
    done
    $TB rm -f /sbin/magisk
    setprop pocket.magisk.status absent
    setprop pocket.magisk.postfs 1
    exit 0
fi
# Real root can open Android's read-only property files for writing via its
# kernel capabilities. Pocket's logical root has no host DAC override: allow
# owner writes on these private guest files before resetprop maps them. Every
# context is created by the guest property service before this script runs.
# This does not add isolation between guest processes sharing the host app UID.
$TB chmod u+w /dev/__properties__/properties_serial
for area in /dev/__properties__/u:object_r:*; do
    [ ! -f "$area" ] || $TB chmod u+w "$area"
done
$TB mkdir -p /sbin/.magisk/device /sbin/.magisk/busybox /data/adb/magisk /data/adb/modules /cache
$TB chmod 700 /data/adb
$TB cp /data/adb/magisk/magisk /sbin/magisk
$TB chmod 755 /sbin/magisk
$TB chmod 755 /data/adb/magisk/busybox
$TB ln -sf /sbin/magisk /system/bin/magisk
$TB ln -sf /sbin/magisk /system/bin/su
$TB ln -sf /sbin/magisk /system/bin/resetprop
# Rotate one previous boot's diagnostic file; preserve module/data state.
if [ -f /logs/magisk.log ]; then $TB mv -f /logs/magisk.log /logs/magisk.previous.log; fi
setprop pocket.magisk.prepared 1
