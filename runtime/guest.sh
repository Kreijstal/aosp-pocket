#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
DIR=$1
PROOT=$2
ROOT=$DIR/rootfs
cd "$ROOT"
# Each path below belongs to this app's imported guest, never the host rootfs.
rm -rf "$ROOT/dev/socket" "$ROOT/dev/__properties__"
mkdir -p "$ROOT/dev/socket" "$ROOT/dev/__properties__" "$ROOT/dev/input" \
    "$ROOT/data/media/0" "$ROOT/data/system_ce/0" "$ROOT/data/misc_ce/0" \
    "$ROOT/mnt/user/0" "$DIR/tmp"
: > "$ROOT/dev/kmsg"
: > "$ROOT/dev/pmsg0"
# Host seccomp TRAP takes priority over PRoot's SECCOMP_RET_TRACE shortcut.
# PTRACE_SYSCALL must intercept old guest identity syscalls before that filter.
export PROOT_NO_SECCOMP=1
export PROOT_TMP_DIR=$DIR/tmp
export PATH=/sbin:/system/bin:/system/sbin:/system/xbin:/system/vendor/bin
export ANDROID_ASSETS=/assets ANDROID_DATA=/data ANDROID_ROOT=/system
export ANDROID_STORAGE=/storage ASEC_MOUNTPOINT=/mnt/asec EXTERNAL_STORAGE=/sdcard
exec "$PROOT" --kill-on-exit -v 0 -r "$ROOT" -0 -w / \
    -b /dev -b /proc -b /sys \
    -b "$ROOT/dev/kmsg:/dev/kmsg" -b "$ROOT/dev/pmsg0:/dev/pmsg0" \
    -b "$ROOT/system/vendor:/vendor" \
    -b "$ROOT/dev/__properties__:/dev/__properties__" \
    -b "$ROOT/dev/socket:/dev/socket" -b /dev/binder:/dev/binder \
    -b "$DIR/qemu_pipe:/dev/qemu_pipe" -b "$ROOT/dev/input:/dev/input" \
    -b "$ROOT/mnt/user/0:/storage/self" /init
