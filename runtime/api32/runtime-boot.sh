# SPDX-License-Identifier: GPL-3.0-only
# The initial shell execs init. An init-owned oneshot performs service startup.
set -eu
export POCKET_API32_BOOT_LOG="${POCKET_API32_BOOT_LOG:-/logs/manager-init.log}"
property_pid=''
log_path=/tmp/properties.log
await_marker() {
    count=0
    until /system/bin/toybox grep -q "$1" "$log_path"; do
        kill -0 "$property_pid" || return 1
        count=$((count+1));test "$count" -lt 200 || return 1
        /system/bin/toybox sleep 0.05
    done
}
start_service() {
    /system/bin/pocket-setprop-api32 ctl.start "$1"
}
wait_service() {
    LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-servicemanager-api32 wait "$1"
}
if test "${1:-}" = --services; then
    property_pid=$PPID
    log_path=$POCKET_API32_BOOT_LOG
    await_marker API32_INIT_READY
    /system/bin/pocket-setprop-api32 ro.pocket.lazy_media 1
    POCKET_SYSCALL_BACKEND=native /system/bin/pocket-boot-probe-api32 prepare-data
    /system/bin/toybox mkdir -p /data/misc/installd /data/misc/gatekeeper /data/misc/keystore /dev/input
    /system/bin/toybox chmod 0700 /data/misc/keystore
    /system/bin/toybox chown 1017:1017 /data/misc/keystore
    start_service pocket-registry
    await_marker POCKET_SERVICEMANAGER_READY
    start_service pocket-installd
    wait_service installd
    start_service pocket-network
    await_marker API32_NETD_READY
    start_service pocket-memory
    await_marker API32_MEMORY_READY
    start_service pocket-mdns-control
    await_marker API32_MDNS_CONTROL_READY
    start_service pocket-dns
    await_marker API32_DNS_READY
    start_service pocket-apex
    await_marker API32_APEX_READY
    start_service pocket-storage
    await_marker API32_STORAGE_READY
    start_service pocket-hardware
    await_marker DIRECT_HWB_REGISTRY_READY
    await_marker API32_SUSPEND_CONTROL_READY
    start_service pocket-audio
    start_service pocket-keymaster
    start_service pocket-keystore
    wait_service android.system.keystore2.IKeystoreService/default
    start_service pocket-gatekeeper-hal
    start_service gatekeeperd
    start_service pocket-tombstoned
    await_marker API32_TOMBSTONED_SCOPE_READY
    start_service pocket-traced
    start_service idmap2d
    start_service pocket-surfaceflinger
    start_service pocket-mediaextractor
    wait_service media.extractor
    start_service pocket-mediaserver
    wait_service media.player
    LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-framework-probe-api32 surface
    LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-tools.jar /system/bin dev.kreijstal.aosppocket.guest.SystemServerDexopt32
    start_service pocket-framework
    LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-framework-probe-api32 wait
    await_marker API32_INPUT_CONTROL_READY
    if test -s /data/misc/adb/adb_keys; then start_service pocket-adbd; fi
    # Android's mksh provides print as a builtin; printf spawns toybox here.
    # Explicit builtin dispatch also preserves option-like nonces such as -n.
    builtin print -r -- "$POCKET_API32_RUN_ID" > /tmp/pocket-runtime-services-ready.new
    /system/bin/toybox mv /tmp/pocket-runtime-services-ready.new /tmp/pocket-runtime-services-ready
    exit 0
fi
if test "$#" -ne 0; then exit 64; fi
cleanup() {
    if test -n "$property_pid"; then
        kill "$property_pid" 2>/dev/null || :
        wait "$property_pid" || :
    fi
    /system/bin/toybox cat /tmp/properties.log
}
trap cleanup EXIT
trap 'exit 143' TERM INT
/system/bin/toybox rm -f /tmp/pocket-runtime-ready /tmp/pocket-runtime-services-ready /tmp/pocket-runtime-services-ready.new
: > /tmp/properties.log
/system/bin/pocket-property-service-api32 > /tmp/properties.log 2>&1 &
property_pid=$!
await_marker API32_PROPERTIES_READY
/system/bin/linkerconfig --target /linkerconfig
test -s /linkerconfig/ld.config.txt
unset LD_LIBRARY_PATH
/apex/com.android.sdkext/bin/derive_classpath /tmp/pocket-runtime-classpath
. /system/etc/pocket-runtime-env.sh
kill "$property_pid"
wait "$property_pid"
property_pid=''
/system/bin/toybox rm -f /dev/__properties__/*
trap - EXIT TERM INT
exec /system/bin/pocket-init-api32 /system/etc/init/pocket-runtime.rc
