# SPDX-License-Identifier: GPL-3.0-only
# A validation client of normal startup; this is not the production entrypoint.
set -eu
boot_pid=''
export POCKET_API32_BOOT_LOG=/tmp/pocket-runtime-validation.log
cleanup() {
    if test -n "$boot_pid"; then
        kill "$boot_pid" 2>/dev/null || :
        wait "$boot_pid" || :
        boot_pid=''
    fi
    /system/bin/toybox cat "$POCKET_API32_BOOT_LOG"
}
trap cleanup EXIT
trap 'exit 143' TERM INT
/system/bin/toybox rm -f /tmp/pocket-runtime-ready
: > "$POCKET_API32_BOOT_LOG"
/system/bin/sh /system/etc/pocket-runtime-boot.sh >> "$POCKET_API32_BOOT_LOG" 2>&1 &
boot_pid=$!
count=0
until test -f /tmp/pocket-runtime-ready && test "$(/system/bin/toybox cat /tmp/pocket-runtime-ready)" = "$POCKET_API32_RUN_ID"; do
    kill -0 "$boot_pid"
    count=$((count+1));test "$count" -lt 2400
    /system/bin/toybox sleep 0.1
done
. /system/etc/pocket-runtime-env.sh
run_tool() {
    LD_PRELOAD=/system/lib64/libpocket_binder_api32.so /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /system/framework/pocket-tools.jar /system/bin "$@"
}
run_tool dev.kreijstal.aosppocket.guest.FrameworkReadyProbe
run_tool dev.kreijstal.aosppocket.guest.UiRuntimeProbe32 compile
run_tool dev.kreijstal.aosppocket.guest.AppLifecycleProbe32 /data/local/tmp/api32-guest-test.apk
run_tool dev.kreijstal.aosppocket.guest.UiRuntimeProbe32 verify
kill -0 "$boot_pid"
# Ask init to stop its services and finish reaping before the owner exits.
/system/bin/pocket-setprop-api32 pocket.control.stop 1
wait "$boot_pid"
boot_pid=''
echo POCKET_RUNTIME_VALIDATION_PASS sdk=32 component_checks=0
