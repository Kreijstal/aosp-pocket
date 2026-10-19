#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run via SSH on the Pi, using only its dedicated virtual-device ADB helper."""
import json
from pathlib import Path
import re
import sys
import time
import uuid

sys.path.insert(0, "/home/pi/lineage-cuttlefish/tools")
import cuttlefish_arm64 as c

c.require_virtual()
package = "dev.kreijstal.aosppocket.directprobe"
directory = "/data/user/0/" + package
nonce = uuid.uuid4().hex
before = c.shell("getprop ro.build.version.sdk")
c.adb("-s", c.serial(), "install", "-r", "/home/pi/aosp-pocket-direct-probe.apk", timeout=90)
c.shell(f"am force-stop {package}")
c.shell(f"am start -W -n {package}/dev.kreijstal.aosppocket.direct.DirectProbeActivity --es nonce {nonce}")
result = None
for _ in range(180):
    text = c.shell(f"su -c 'cat {directory}/files/direct-result.json 2>/dev/null || true'")
    if text.strip():
        candidate = json.loads(text)
        if candidate.get("nonce") == nonce:
            result = candidate
            break
    time.sleep(1)
if result is None:
    print(c.shell(f"su -c 'cat {directory}/files/direct-suite.log {directory}/files/proot-suite.log 2>/dev/null || true'"))
    raise SystemExit("Direct backend probe timed out")
result["host_selinux_enforcing"] = c.shell("getenforce") == "Enforcing"
result["host_property_area_unchanged"] = c.shell("getprop ro.build.version.sdk") == before
Path("/home/pi/pocket-direct-validation.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
assert result["passed"], result.get("error")
assert result["host_uid"] >= 10000 and result["host_selinux_enforcing"] and result["host_property_area_unchanged"]
for mode in ("direct", "proot"):
    section = result[mode]
    assert "untrusted_app" in section["selinux"] and "DIRECT_CHILD_PASS" in section["log"]
    assert re.search(r"^Seccomp:\s+2$", section["status"], re.M)
    uids = re.search(r"^Uid:\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", section["status"], re.M)
    assert uids and all(int(uid) == result["host_uid"] for uid in uids.groups())
    assert re.search(r"^CapEff:\s+0+$", section["status"], re.M)
    tracer = int(re.search(r"^TracerPid:\s+(\d+)", section["status"], re.M)[1])
    assert tracer == 0 if mode == "direct" else tracer > 0
assert "direct-root/system/lib64/libc.so" in result["direct"]["log"]
for name in ("system/bin/linker64", "system/lib64/libc.so", "system/lib64/libdl.so"):
    assert "direct-root/" + name in result["direct"]["maps"], name
for line in result["direct"]["maps"].splitlines():
    path = line.split()[-1]
    if path.startswith("/") and path.endswith(".so"):
        assert path.startswith(directory + "/files/direct-root/system/lib64/"), path
assert len(result["benchmark"]) == 3 and result["full_guest_boot_verified"] is False
print("PASS: Android 9 linker/bionic controlled path ABI + thread + fork/exec; zero tracer in direct mode; PRoot comparison; real app UID/SELinux/seccomp retained")
