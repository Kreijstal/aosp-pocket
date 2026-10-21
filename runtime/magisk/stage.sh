#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
stage="$1"
case "$stage" in
    post-fs-data)
        if /system/bin/toybox timeout 35 /sbin/magisk --post-fs-data; then
            setprop pocket.magisk.status ready
        else
            echo 'Magisk post-fs-data failed; continuing guest boot' >&2
            setprop pocket.magisk.status failed
        fi
        setprop pocket.magisk.postfs 1
        ;;
    service|boot-complete)
        [ -f /data/adb/pocket-magisk.disabled ] && exit 0
        [ "$(getprop pocket.magisk.status)" = ready ] || exit 0
        exec /sbin/magisk "--$stage"
        ;;
    *) exit 64 ;;
esac
