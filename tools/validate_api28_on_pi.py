#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run over SSH on the Pi: no default ADB server or API 25 guest changes."""
import argparse
import json
from pathlib import Path
import re
import sys
import time
import zipfile

sys.path.insert(0, "/home/pi/lineage-cuttlefish/tools")
import cuttlefish_arm64 as c

options = argparse.ArgumentParser(description=__doc__)
options.add_argument("--zygote-only", action="store_true")
options.add_argument("--direct", action="store_true", help="Use the separate direct platform APK")
options.add_argument("--system-server", action="store_true")
options.add_argument("--skip-install",action="store_true",help="Reuse the APK already installed by a previous validation")
options.add_argument("--overlay",action="store_true",help="Apply /home/pi/aosp-pocket-native-overlay.zip to the installed development APK")
args = options.parse_args()
if args.system_server and not args.direct: options.error("--system-server requires --direct")
if args.overlay and not (args.direct and args.skip_install): options.error("--overlay requires --direct --skip-install")
c.require_virtual()
host_sdk_before = c.shell("getprop ro.build.version.sdk").strip()
package = "dev.kreijstal.aosppocket.bootprobe" if args.direct else "dev.kreijstal.aosppocket.api28probe"
activity = "dev.kreijstal.aosppocket.runtime.BootProbeActivity" if args.direct else "dev.kreijstal.aosppocket.runtime.Api28ProbeActivity"
directory = "/data/user/0/" + package
apk = "/home/pi/aosp-pocket-boot-probe.apk" if args.direct else "/home/pi/aosp-pocket-api28-probe.apk"
if not args.skip_install:
    c.adb("-s", c.serial(), "install", "-r", apk, timeout=180)
c.shell(f"am force-stop {package}")
c.shell(f"su -c 'rm -f {directory}/files/api28-result.json'")
stage = " --es stage system-server" if args.system_server else (" --es stage zygote" if args.zygote_only else "")
if args.overlay:
    with zipfile.ZipFile("/home/pi/aosp-pocket-native-overlay.zip") as update:
        expected_overlay=set(json.loads(update.read("manifest.json"))["files"])
    assert expected_overlay,"Overlay has no changed files"
    external=f"/sdcard/Android/data/{package}/files"
    c.shell(f"mkdir -p {external}")
    c.adb("-s",c.serial(),"push","/home/pi/aosp-pocket-native-overlay.zip",external+"/native-overlay.zip",timeout=60)
    stage+=" --ez native_overlay true"
c.shell(f"am start -W -n {package}/{activity}{stage}")
result = None
for _ in range(480 if args.system_server else 260):
    output = c.shell(f"su -c 'cat {directory}/files/api28-result.json 2>/dev/null || true'")
    if output.strip():
        result = json.loads(output)
        break
    time.sleep(1)
if result is None:
    print(c.shell(f"su -c 'cat {directory}/files/manager.log {directory}/files/client.log 2>/dev/null || true'"))
    raise SystemExit("API 28 integration probe timed out (is the VM unlocked?)")
if args.overlay:
    assert set(result.get("native_overlay_files",[]))==expected_overlay,"Installed APK did not apply the expected overlay"
pid = c.shell(f"pidof {package}").strip()
assert pid.isdecimal(), pid
status = c.shell(f"su -c 'cat /proc/{pid}/status'")
selinux = c.shell(f"su -c 'cat /proc/{pid}/attr/current'").strip()
enforcing = c.shell("getenforce").strip() == "Enforcing"
uid = int(re.search(r"^Uid:\s+(\d+)", status, re.M)[1])
seccomp = int(re.search(r"^Seccomp:\s+(\d+)", status, re.M)[1])
result.update(host_process_uid=uid, host_seccomp=seccomp, host_selinux=selinux, host_selinux_enforcing=enforcing)
result["host_property_area_unchanged"] = c.shell("getprop ro.build.version.sdk").strip() == host_sdk_before
assert uid >= 10000 and uid == result["host_uid"], result
assert seccomp == 2 and enforcing and "untrusted_app" in selinux, result
Path("/home/pi/pocket-boot-validation.json" if args.direct else "/home/pi/pocket-api28-validation.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
if args.system_server:
    raise SystemExit(0 if result.get("full_guest_boot_verified") else 2)
assert result["passed"], "API 28 integration failed"
assert result["zygote_image_only_cache_absent"], result
assert "untrusted_app" in result["zygote_child_selinux"], result
child_uids = re.search(r"^Uid:\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", result["zygote_child_status"], re.M)
assert child_uids and all(int(v) == uid for v in child_uids.groups()), result
assert re.search(r"^CapEff:\s+0+$", result["zygote_child_status"], re.M), result
unsupported="API28_ZYGOTE_UNSUPPORTED_CHILD_ZYGOTE" if args.direct else "API28_ZYGOTE_UNSUPPORTED_STORAGE"
assert unsupported+" rejected=1 server_alive=1" in result["zygote_client_log"], result
if args.direct:
    assert result["zygote_child"]["app_profile"], result
    assert re.search(r"^TracerPid:\s+0$",result["zygote_child_status"],re.M),result
if args.zygote_only:
    assert result["scope"] == "zygote_only" and result["zygote_passed"], result
    assert result["zygote_child"]["binder"] and result["zygote_child"]["regain_root_denied"], result
    assert "API28_ZYGOTE_FORK_PASS" in result["zygote_client_log"], result
    assert re.search(r"^Seccomp:\s+2$", result["zygote_child_status"], re.M), result
    print("PASS: focused Android 9 zygote ABI/fork/Java/Binder/guest identity/child exit/shutdown under host app confinement")
    raise SystemExit(0)
output = result["client_log"]
match = re.search(r"server_pid=(\d+) client_pid=(\d+) server_uid=(\d+) client_uid=(\d+) fd=api28fd", output)
assert match and match[1] != match[2] and int(match[3]) == int(match[4]) == uid, output
caller = re.search(r"caller_uid=(\d+) caller_pid=(\d+)", output)
assert caller and int(caller[1]) == uid and caller[2] == match[2], output
assert result["ordinary_uid_passed"] and result["virtual_uid_passed"], result
assert result["properties_passed"] and result["property_restart_passed"] and result["host_property_area_unchanged"], result
assert result["init_passed"] and "API28_INIT_CLIENT_PASS" in result["init_client_log"], result
assert "remaining=0 failed=0" in result["init_log"] and "grandchild=gone" in result["init_shutdown_log"], result
assert "status=42" in result["init_log"] and "INIT_ONESHOT_PASS" in result["init_log"], result
assert result["zygote_passed"] and result["zygote_child"]["binder"] and result["zygote_child"]["regain_root_denied"], result
assert "API28_ZYGOTE_FORK_PASS" in result["zygote_client_log"], result
assert "API28_ZYGOTE_ABI value=arm64-v8a" in result["zygote_query_after_child_log"], result
assert re.search(r"^Seccomp:\s+2$", result["zygote_child_status"], re.M), result
assert result["art_passed"] and "API28_ART_PASS" in result["art_log"], result
assert "sdk=28" in output and "serial_changed=1 uid=39999" in result["property_wait_log"], result
assert "server_uid=0 client_uid=39999 fd=api28fd caller_uid=39999" in result["virtual_client_log"], result
print("PASS: API 28 Binder/FD/virtual UID, private properties/read-only/notify/clean restart, ART Java/thread/reflection/exception/native child, AOSP init parser/actions/service crash + restart + sockets + shutdown, real zygote ABI query/fork/Java child/Binder/guest identity/child reaping, ordinary host app UID + enforcing SELinux + seccomp")
