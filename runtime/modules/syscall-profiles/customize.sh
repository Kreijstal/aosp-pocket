#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
[ "$ARCH" = arm64 ] || abort 'This module currently supports ARM64 only'
[ -n "$POCKET_DIRECT_ROOT" ] || abort 'This version requires Pocket userspace compatibility'
mkdir -p /data/adb/pocket-syscall-profiles
chmod 700 /data/adb/pocket-syscall-profiles
ui_print 'No app profiles are enabled automatically.'
ui_print 'Profiles: /data/adb/pocket-syscall-profiles/PROCESS.profile'
ui_print 'Enable Zygisk and restart the Pocket guest to load this module.'
