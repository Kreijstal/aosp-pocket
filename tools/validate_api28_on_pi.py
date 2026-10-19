#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run over SSH on the Pi: no default ADB server or API 25 guest changes."""
import argparse
import json
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, "/home/pi/lineage-cuttlefish/tools")
import cuttlefish_arm64 as c

options = argparse.ArgumentParser(description=__doc__)
options.add_argument("--zygote-only", action="store_true")
args = options.parse_args()
c.require_virtual()
host_sdk_before = c.shell("getprop ro.build.version.sdk").strip()
package = "dev.kreijstal.aosppocket.api28probe"
activity = "dev.kreijstal.aosppocket.runtime.Api28ProbeActivity"
directory = "/data/user/0/" + package
apk = "/home/pi/aosp-pocket-api28-probe.apk"
c.adb("-s", c.serial(), "install", "-r", apk, timeout=180)
c.shell(f"am force-stop {package}")
c.shell(f"su -c 'rm -f {directory}/files/api28-result.json'")
stage = " --es stage zygote" if args.zygote_only else ""
c.shell(f"am start -W -n {package}/{activity}{stage}")
result = None
for _ in range(260):
    output = c.shell(f"su -c 'cat {directory}/files/api28-result.json 2>/dev/null || true'")
    if output.strip():
        result = json.loads(output)
        break
    time.sleep(1)
if result is None:
    print(c.shell(f"su -c 'cat {directory}/files/manager.log {directory}/files/client.log 2>/dev/null || true'"))
    raise SystemExit("API 28 integration probe timed out (is the VM unlocked?)")
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
Path("/home/pi/pocket-api28-validation.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
assert result["passed"], "API 28 integration failed"
assert result["zygote_image_only_cache_absent"], result
assert "untrusted_app" in result["zygote_child_selinux"], result
child_uids = re.search(r"^Uid:\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", result["zygote_child_status"], re.M)
assert child_uids and all(int(v) == uid for v in child_uids.groups()), result
assert re.search(r"^CapEff:\s+0+$", result["zygote_child_status"], re.M), result
assert "API28_ZYGOTE_UNSUPPORTED_STORAGE rejected=1 server_alive=1" in result["zygote_client_log"], result
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
